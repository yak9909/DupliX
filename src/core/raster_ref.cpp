// The CPU rasterizer as of 2026-11-25 (before the tiled version), kept as the reference the unit tests compare
// the tiled CPU and the GPU rasterizers against (exact: every fragment of a pixel sorted by its depth).
#include "core/raster_ref.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <emmintrin.h>
#include <thread>
#include <vector>

#include "core/fast_plane.h"

namespace rnd {

namespace {

struct P8 { uint8_t c[4]; };
struct P16 { uint16_t c[4]; };
struct PF { float c[4]; };

// Load: the 4 channels as floats in the pixel's own units. Store: from 0..1 floats, rounded (+0.5) and clamped.
template <class P> struct Traits;
template <> struct Traits<P8> {
    static constexpr float kMax = 255.f;
    static __m128 Load(const P8* p)
    {
        __m128i z = _mm_setzero_si128();
        __m128i v = _mm_cvtsi32_si128(*(const int*)p);
        return _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(v, z), z));
    }
    static void Store(P8& p, __m128 v)
    {
        v = _mm_add_ps(_mm_mul_ps(v, _mm_set1_ps(kMax)), _mm_set1_ps(0.5f));
        v = _mm_min_ps(_mm_max_ps(v, _mm_setzero_ps()), _mm_set1_ps(kMax));
        __m128i i = _mm_cvttps_epi32(v);
        i = _mm_packs_epi32(i, i);
        i = _mm_packus_epi16(i, i);
        *(int*)&p = _mm_cvtsi128_si32(i);
    }
};
template <> struct Traits<P16> {
    static constexpr float kMax = 32768.f;
    static __m128 Load(const P16* p)
    {
        __m128i v = _mm_loadl_epi64((const __m128i*)p);
        return _mm_cvtepi32_ps(_mm_unpacklo_epi16(v, _mm_setzero_si128()));
    }
    static void Store(P16& p, __m128 v)
    {
        v = _mm_add_ps(_mm_mul_ps(v, _mm_set1_ps(kMax)), _mm_set1_ps(0.5f));
        v = _mm_min_ps(_mm_max_ps(v, _mm_setzero_ps()), _mm_set1_ps(kMax));
        alignas(16) int32_t o[4];
        _mm_store_si128((__m128i*)o, _mm_cvttps_epi32(v));
        for (int k = 0; k < 4; k++) p.c[k] = (uint16_t)o[k];
    }
};
template <> struct Traits<PF> {
    static constexpr float kMax = 1.f;
    static __m128 Load(const PF* p) { return _mm_loadu_ps(p->c); }
    static void Store(PF& p, __m128 v) { _mm_storeu_ps(p.c, v); }
};

// Bounding box (dst buffer px, inclusive) of a copy; false if it misses dst.
bool CopyBox(const RasterImage& src, const RasterCopy& c, double dsX, double dsY, const RasterImage& dst, int* x0,
             int* y0, int* x1, int* y1)
{
    double F[9];
    bool whole = !Invert3(c.screenToSrc, F);
    double mnx = 1e300, mny = 1e300, mxx = -1e300, mxy = -1e300;
    const double us[2] = {src.originX / dsX, (src.originX + src.width) / dsX};
    const double vs[2] = {src.originY / dsY, (src.originY + src.height) / dsY};
    for (double u : us)
        for (double v : vs) {
            if (whole) break;
            double a = u * F[0] + v * F[3] + F[6], b = u * F[1] + v * F[4] + F[7], w = u * F[2] + v * F[5] + F[8];
            if (!(w > 0)) {
                whole = true;
                break;
            }
            double sx = (a / w - dst.screenShiftX) * dsX - dst.originX - 0.5,
                   sy = (b / w - dst.screenShiftY) * dsY - dst.originY - 0.5;
            mnx = std::min(mnx, sx), mxx = std::max(mxx, sx), mny = std::min(mny, sy), mxy = std::max(mxy, sy);
        }
    if (whole) {
        *x0 = 0, *y0 = 0, *x1 = dst.width - 1, *y1 = dst.height - 1;
        return true;
    }
    double lx = std::floor(mnx) - 1, hx = std::ceil(mxx) + 1, ly = std::floor(mny) - 1, hy = std::ceil(mxy) + 1;
    if (hx < 0 || hy < 0 || lx > dst.width - 1 || ly > dst.height - 1) return false;
    *x0 = (int)std::max(lx, 0.0);
    *y0 = (int)std::max(ly, 0.0);
    *x1 = (int)std::min(hx, (double)dst.width - 1);
    *y1 = (int)std::min(hy, (double)dst.height - 1);
    return true;
}

struct Box {
    int x0, y0, x1, y1;
    bool hit;
};

// One copy's contribution to one pixel.
struct alignas(16) Frag {
    __m128 c;   // premultiplied colour x opacity, 0..1
    float iz;   // 1 / camera z at this pixel (larger = nearer)
    int next;   // the pixel's previous fragment (-1: none)
};
// Per-thread working memory, reused row after row.
struct RowScratch {
    std::vector<int> head;  // per pixel of the row: its last fragment
    std::vector<Frag> frags;
    std::vector<const Frag*> order;
};

// Draws rows [rowBegin, rowEnd). Per row, every copy's fragments are collected per pixel, then each pixel
// composites its fragments far to near by the depth AT THAT PIXEL (copies seen at an angle overlap in an
// order no single per-copy depth gives; spike of the user's orbiting camera, 11/24). Equal depths keep the
// copies' order (sorted by `depth`, far first).
template <class P>
void DrawRows(const RasterImage& src, const RasterCopy* copies, const Box* boxes, int n, double dsX, double dsY,
              const RasterImage& dst, int rowBegin, int rowEnd, RowScratch& sc)
{
    const int w = dst.width;
    sc.head.resize((size_t)w);
    const float inv = 1.f / Traits<P>::kMax;
    const int sw = src.width, sh = src.height;
    // AE's worlds are straight alpha: filter and composite premultiplied (colour x alpha / max).
    const __m128 invv = _mm_set1_ps(inv), ones = _mm_set1_ps(1.f);
    auto Premul = [invv, ones](__m128 v) {
        // (1, a, a, a) / max: alpha broadcast, lane 0 replaced by 1
        __m128 m = _mm_move_ss(_mm_mul_ps(_mm_shuffle_ps(v, v, 0), invv), ones);
        return _mm_mul_ps(v, m);
    };
    const double stepX = 1.0 / dsX;
    // Raw pointers in the loops: AE loads the Debug build, whose checked std::vector is slow and locks.
    int* head = sc.head.data();
    if (sc.frags.size() < (size_t)w * 4) sc.frags.resize((size_t)w * 4);
    if (sc.order.size() < 64) sc.order.resize(64);
    for (int y = rowBegin; y < rowEnd; y++) {
        for (int x = 0; x < w; x++) head[x] = -1;
        Frag* frags = sc.frags.data();
        size_t nFrag = 0, capFrag = sc.frags.size();
        const double sy = (y + dst.originY + 0.5) / dsY + dst.screenShiftY;
        for (int i = 0; i < n; i++) {
            const Box& b = boxes[i];
            if (!b.hit || y < b.y0 || y > b.y1) continue;
            const double* H = copies[i].screenToSrc;
            const float op = (float)copies[i].opacity * inv;
            const double sx0 = (b.x0 + dst.originX + 0.5) / dsX + dst.screenShiftX;
            // homogeneous source position along the row: linear in x (hw = 1 / camera z)
            double ha = sx0 * H[0] + sy * H[3] + H[6], hb = sx0 * H[1] + sy * H[4] + H[7], hw = sx0 * H[2] + sy * H[5] + H[8];
            const double da = H[0] * stepX, db = H[1] * stepX, dw = H[2] * stepX;
            for (int x = b.x0; x <= b.x1; x++, ha += da, hb += db, hw += dw) {
                if (!(hw > 0)) continue;
                const double iw = 1.0 / hw;
                const double bx = ha * iw * dsX - 0.5 - src.originX, by = hb * iw * dsY - 0.5 - src.originY;
                if (!(bx > -1 && by > -1 && bx < sw && by < sh)) continue;
                const int ix = (int)(bx + 1.0) - 1, iy = (int)(by + 1.0) - 1;  // floor; bx, by > -1
                const float fx = (float)(bx - ix), fy = (float)(by - iy);
                const float ws[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
                __m128 s;
                const char* base = (const char*)src.data;
                if (ix >= 0 && iy >= 0 && ix + 1 < sw && iy + 1 < sh) {
                    const P* r0 = (const P*)(base + (size_t)iy * src.rowBytes) + ix;
                    const P* r1 = (const P*)(base + (size_t)(iy + 1) * src.rowBytes) + ix;
                    s = _mm_add_ps(_mm_add_ps(_mm_mul_ps(Premul(Traits<P>::Load(r0)), _mm_set1_ps(ws[0])),
                                              _mm_mul_ps(Premul(Traits<P>::Load(r0 + 1)), _mm_set1_ps(ws[1]))),
                                   _mm_add_ps(_mm_mul_ps(Premul(Traits<P>::Load(r1)), _mm_set1_ps(ws[2])),
                                              _mm_mul_ps(Premul(Traits<P>::Load(r1 + 1)), _mm_set1_ps(ws[3]))));
                } else {
                    s = _mm_setzero_ps();
                    for (int j = 0; j < 2; j++)
                        for (int i2 = 0; i2 < 2; i2++) {
                            int qx = ix + i2, qy = iy + j;
                            if (qx < 0 || qy < 0 || qx >= sw || qy >= sh) continue;
                            const P* p = (const P*)(base + (size_t)qy * src.rowBytes) + qx;
                            s = _mm_add_ps(s, _mm_mul_ps(Premul(Traits<P>::Load(p)), _mm_set1_ps(ws[j * 2 + i2])));
                        }
                }
                s = _mm_mul_ps(s, _mm_set1_ps(op));  // premultiplied source x opacity, 0..1
                if (_mm_movemask_ps(_mm_cmpneq_ps(s, _mm_setzero_ps())) == 0) continue;
                if (nFrag == capFrag) {  // grow (rare): keep the contents
                    sc.frags.resize(capFrag * 2);
                    frags = sc.frags.data();
                    capFrag = sc.frags.size();
                }
                Frag& fr = frags[nFrag];
                fr.c = s;
                fr.iz = (float)hw;
                fr.next = head[x];
                head[x] = (int)nFrag++;
            }
        }
        // resolve the row: per pixel, far to near by the pixel's own depth
        P* out = (P*)((char*)dst.data + (size_t)y * dst.rowBytes);
        for (int x = 0; x < w; x++) {
            __m128 acc = _mm_setzero_ps();
            int f = head[x];
            if (f >= 0 && frags[f].next < 0) {
                acc = frags[f].c;  // one fragment: nothing to order
            } else if (f >= 0) {
                // the list runs near to far in registration order (copies were taken far to near):
                // fill `order` from the back so it reads far to near
                size_t cnt = 0;
                for (int g = f; g >= 0; g = frags[g].next) cnt++;
                if (sc.order.size() < cnt) sc.order.resize(cnt * 2);
                const Frag** order = sc.order.data();
                size_t pos = cnt;
                for (int g = f; g >= 0; g = frags[g].next) order[--pos] = &frags[g];
                for (size_t k = 1; k < cnt; k++) {  // stable insertion sort, usually in order already
                    const Frag* t = order[k];
                    size_t j = k;
                    for (; j > 0 && order[j - 1]->iz > t->iz; j--) order[j] = order[j - 1];
                    order[j] = t;
                }
                for (size_t k = 0; k < cnt; k++) {
                    const __m128 c = order[k]->c;
                    const __m128 keep = _mm_sub_ps(_mm_set1_ps(1.f), _mm_shuffle_ps(c, c, 0));  // 1 - alpha
                    acc = _mm_add_ps(c, _mm_mul_ps(acc, keep));
                }
            }
            // back to straight alpha (transparent: colour 0)
            const float al = _mm_cvtss_f32(acc);
            const float k = al > 0.f ? 1.f / al : 0.f;
            Traits<P>::Store(out[x], _mm_mul_ps(acc, _mm_set_ps(k, k, k, 1.f)));
        }
    }
}

template <class P>
bool Run(const RasterImage& src, const RasterCopy* copies, int n, double dsX, double dsY, const RasterImage& dst,
         int threads, const std::function<bool()>& abort)
{
    std::vector<Box> boxes((size_t)std::max(n, 1));
    for (int i = 0; i < n; i++) {
        Box& b = boxes[i];
        b.hit = copies[i].opacity > 0 && CopyBox(src, copies[i], dsX, dsY, dst, &b.x0, &b.y0, &b.x1, &b.y1);
    }
    // Small chunks of rows handed out in order: copies often cover only part of the frame.
    const int kRows = 8;
    const int chunks = (dst.height + kRows - 1) / kRows;
    std::atomic<int> next{0};
    std::atomic<bool> stop{false};
    auto worker = [&](bool caller) {
        RowScratch sc;
        for (int k; !stop.load(std::memory_order_relaxed) && (k = next.fetch_add(1)) < chunks;) {
            // AE's abort callback is only asked on the calling (render) thread
            if (caller && abort && abort()) {
                stop = true;
                break;
            }
            DrawRows<P>(src, copies, boxes.data(), n, dsX, dsY, dst, k * kRows, std::min(dst.height, (k + 1) * kRows),
                        sc);
        }
    };
    int t = std::max(1, std::min(threads, chunks));
    std::vector<std::thread> pool;
    try {
        for (int i = 1; i < t; i++) pool.emplace_back(worker, false);
    } catch (...) {
        // no more threads: the ones already started and this one take all the chunks
    }
    worker(true);
    for (auto& th : pool) th.join();
    return !stop.load();
}

}  // namespace

bool RasterizeCopiesReference(const RasterImage& src, RasterCopy* copies, int n, double dsX, double dsY, RasterImage& dst,
                     int threads, const std::function<bool()>& abort)
{
    if (dst.width <= 0 || dst.height <= 0 || !dst.data) return true;
    if (!(dsX > 0) || !(dsY > 0) || src.bpc != dst.bpc || !src.data) n = 0;
    if (n > 0)
        std::stable_sort(copies, copies + n, [](const RasterCopy& a, const RasterCopy& b) { return a.depth > b.depth; });
    switch (dst.bpc) {
    case 8: return Run<P8>(src, copies, n, dsX, dsY, dst, threads, abort);
    case 16: return Run<P16>(src, copies, n, dsX, dsY, dst, threads, abort);
    case 32: return Run<PF>(src, copies, n, dsX, dsY, dst, threads, abort);
    default: return true;
    }
}

}  // namespace rnd
