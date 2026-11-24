#include "core/raster.h"

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
            double sx = a / w * dsX - dst.originX - 0.5, sy = b / w * dsY - dst.originY - 0.5;
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

// Draws rows [rowBegin, rowEnd) into acc (premultiplied floats, 0..1, rows of dst.width * 4).
template <class P>
void DrawRows(const RasterImage& src, const RasterCopy* copies, const Box* boxes, int n, double dsX, double dsY,
              const RasterImage& dst, int rowBegin, int rowEnd, float* acc)
{
    const int w = dst.width;
    std::fill(acc, acc + (size_t)(rowEnd - rowBegin) * w * 4, 0.f);
    const float inv = 1.f / Traits<P>::kMax;
    const int sw = src.width, sh = src.height;
    for (int i = 0; i < n; i++) {
        const Box& b = boxes[i];
        if (!b.hit || b.y1 < rowBegin || b.y0 >= rowEnd) continue;
        const RasterCopy& c = copies[i];
        const int y0 = std::max(b.y0, rowBegin), y1 = std::min(b.y1, rowEnd - 1);
        const double* H = c.screenToSrc;
        const float op = (float)c.opacity * inv;
        const double stepX = 1.0 / dsX;
        for (int y = y0; y <= y1; y++) {
            const double sy = (y + dst.originY + 0.5) / dsY;
            const double sx0 = (b.x0 + dst.originX + 0.5) / dsX;
            // homogeneous source position along the row: linear in x
            double ha = sx0 * H[0] + sy * H[3] + H[6], hb = sx0 * H[1] + sy * H[4] + H[7], hw = sx0 * H[2] + sy * H[5] + H[8];
            const double da = H[0] * stepX, db = H[1] * stepX, dw = H[2] * stepX;
            float* row = acc + ((size_t)(y - rowBegin) * w) * 4;
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
                    s = _mm_add_ps(_mm_add_ps(_mm_mul_ps(Traits<P>::Load(r0), _mm_set1_ps(ws[0])),
                                              _mm_mul_ps(Traits<P>::Load(r0 + 1), _mm_set1_ps(ws[1]))),
                                   _mm_add_ps(_mm_mul_ps(Traits<P>::Load(r1), _mm_set1_ps(ws[2])),
                                              _mm_mul_ps(Traits<P>::Load(r1 + 1), _mm_set1_ps(ws[3]))));
                } else {
                    s = _mm_setzero_ps();
                    for (int j = 0; j < 2; j++)
                        for (int i2 = 0; i2 < 2; i2++) {
                            int qx = ix + i2, qy = iy + j;
                            if (qx < 0 || qy < 0 || qx >= sw || qy >= sh) continue;
                            const P* p = (const P*)(base + (size_t)qy * src.rowBytes) + qx;
                            s = _mm_add_ps(s, _mm_mul_ps(Traits<P>::Load(p), _mm_set1_ps(ws[j * 2 + i2])));
                        }
                }
                s = _mm_mul_ps(s, _mm_set1_ps(op));  // premultiplied source x opacity, 0..1
                if (_mm_movemask_ps(_mm_cmpneq_ps(s, _mm_setzero_ps())) == 0) continue;
                float* d = row + (size_t)x * 4;
                const __m128 keep = _mm_sub_ps(_mm_set1_ps(1.f), _mm_shuffle_ps(s, s, 0));  // 1 - alpha
                _mm_storeu_ps(d, _mm_add_ps(s, _mm_mul_ps(_mm_loadu_ps(d), keep)));
            }
        }
    }
    for (int y = rowBegin; y < rowEnd; y++) {
        P* out = (P*)((char*)dst.data + (size_t)y * dst.rowBytes);
        const float* a = acc + ((size_t)(y - rowBegin) * w) * 4;
        for (int x = 0; x < w; x++) Traits<P>::Store(out[x], _mm_loadu_ps(a + (size_t)x * 4));
    }
}

template <class P>
void Run(const RasterImage& src, const RasterCopy* copies, int n, double dsX, double dsY, const RasterImage& dst,
         int threads)
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
    auto worker = [&]() {
        std::vector<float> acc((size_t)kRows * dst.width * 4);
        for (int k; (k = next.fetch_add(1)) < chunks;)
            DrawRows<P>(src, copies, boxes.data(), n, dsX, dsY, dst, k * kRows, std::min(dst.height, (k + 1) * kRows),
                        acc.data());
    };
    int t = std::max(1, std::min(threads, chunks));
    std::vector<std::thread> pool;
    try {
        for (int i = 1; i < t; i++) pool.emplace_back(worker);
    } catch (...) {
        // no more threads: the ones already started and this one take all the chunks
    }
    worker();
    for (auto& th : pool) th.join();
}

}  // namespace

void RasterizeCopies(const RasterImage& src, RasterCopy* copies, int n, double dsX, double dsY, RasterImage& dst,
                     int threads)
{
    if (dst.width <= 0 || dst.height <= 0 || !dst.data) return;
    if (!(dsX > 0) || !(dsY > 0) || src.bpc != dst.bpc || !src.data) n = 0;
    if (n > 0)
        std::stable_sort(copies, copies + n, [](const RasterCopy& a, const RasterCopy& b) { return a.depth > b.depth; });
    switch (dst.bpc) {
    case 8: Run<P8>(src, copies, n, dsX, dsY, dst, threads); break;
    case 16: Run<P16>(src, copies, n, dsX, dsY, dst, threads); break;
    case 32: Run<PF>(src, copies, n, dsX, dsY, dst, threads); break;
    default: break;
    }
}

}  // namespace rnd
