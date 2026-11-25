#include "core/raster.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <emmintrin.h>
#include <thread>
#include <vector>

#include "core/tiles.h"

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

// One copy's contribution to one pixel.
struct alignas(16) Frag {
    __m128 c;   // premultiplied colour x opacity, 0..1 (lane 0 = alpha)
    float iz;   // 1 / camera z at this pixel (larger = nearer)
    int rank;   // the copy's far-to-near rank: equal depths put the higher rank on top
};

constexpr int kTile = 16;
constexpr int kTilePx = kTile * kTile;

// Per-thread working memory for one tile: kMaxFrags fragments per pixel, fragment k of pixel p at
// frags[k * kTilePx + p] (most pixels hold few fragments: their first ones stay together in the cache).
struct TileScratch {
    std::unique_ptr<Frag[]> frags{new Frag[(size_t)kTilePx * kMaxFrags]};  // not initialised (written before read)
    int count[kTilePx];
    float opaqueIz[kTilePx];  // the nearest fully opaque fragment so far (0: none): anything behind it is hidden
    __m128 back[kTilePx];     // fragments evicted past kMaxFrags, composited under one another
};

// Scratch blocks reused across calls (each is 1.5 MB): taken by a worker for one call, then returned.
std::mutex g_scratchMutex;
std::vector<std::unique_ptr<TileScratch>> g_scratchFree;
std::unique_ptr<TileScratch> TakeScratch()
{
    {
        std::lock_guard<std::mutex> lk(g_scratchMutex);
        if (!g_scratchFree.empty()) {
            auto s = std::move(g_scratchFree.back());
            g_scratchFree.pop_back();
            return s;
        }
    }
    return std::unique_ptr<TileScratch>(new TileScratch);
}
void ReturnScratch(std::unique_ptr<TileScratch> s)
{
    std::lock_guard<std::mutex> lk(g_scratchMutex);
    if (g_scratchFree.size() < 64) g_scratchFree.push_back(std::move(s));
}

inline __m128 Over(__m128 front, __m128 behind)
{
    return _mm_add_ps(front, _mm_mul_ps(behind, _mm_sub_ps(_mm_set1_ps(1.f), _mm_shuffle_ps(front, front, 0))));
}

// Draws one tile. Copies come nearest first; per pixel the fragments are kept (at most kMaxFrags), those behind
// an opaque fragment dropped, then composited far to near by the depth AT THAT PIXEL (copies seen at an angle
// overlap in an order no per-copy depth gives; the user's orbiting camera, 11/24), equal depths by rank.
template <class P>
void DrawTile(const RasterImage& src, const PreparedCopies& pc, int tile, double dsX, double dsY,
              const RasterImage& dst, TileScratch& sc)
{
    const TileLists& tl = pc.tiles;
    const int tx = tile % tl.tilesX, ty = tile / tl.tilesX;
    const int px0 = tx * kTile, py0 = ty * kTile;
    const int pw = std::min(kTile, dst.width - px0), ph = std::min(kTile, dst.height - py0);
    if (tl.start[tile] == tl.start[tile + 1]) {  // no copy here: transparent
        for (int yy = 0; yy < ph; yy++)
            std::memset((char*)dst.data + (size_t)(py0 + yy) * dst.rowBytes + (size_t)px0 * sizeof(P), 0,
                        (size_t)pw * sizeof(P));
        return;
    }
    for (int i = 0; i < kTilePx; i++) sc.count[i] = 0, sc.opaqueIz[i] = 0.f, sc.back[i] = _mm_setzero_ps();
    const float inv = 1.f / Traits<P>::kMax;
    const __m128 invv = _mm_set1_ps(inv), ones = _mm_set1_ps(1.f);
    auto Premul = [invv, ones](__m128 v) {
        __m128 m = _mm_move_ss(_mm_mul_ps(_mm_shuffle_ps(v, v, 0), invv), ones);  // (1, a, a, a) / max
        return _mm_mul_ps(v, m);
    };
    const int sw = src.width, sh = src.height;
    const char* base = (const char*)src.data;
    const double stepX = 1.0 / dsX;
    Frag* frags = sc.frags.get();
    float tileMinOpaque = 0.f;  // min of opaqueIz over the tile's pixels (0 while any pixel has none)
    int opaquePx = 0;
    for (int j = tl.start[tile]; j < tl.start[tile + 1]; j++) {
        const int ci = tl.index[j];
        if (opaquePx == pw * ph && pc.nearIz[ci] < tileMinOpaque) continue;  // behind every pixel's opaque front
        const RasterCopy& cp = pc.copies[ci];
        const int rank = pc.rank[ci];
        const double* H = cp.screenToSrc;
        const float op = (float)cp.opacity * inv;
        bool newOpaque = false;
        for (int yy = 0; yy < ph; yy++) {
            const double sy = (py0 + yy + dst.originY + 0.5) / dsY + dst.screenShiftY;
            const double sx0 = (px0 + dst.originX + 0.5) / dsX + dst.screenShiftX;
            double ha = sx0 * H[0] + sy * H[3] + H[6], hb = sx0 * H[1] + sy * H[4] + H[7], hw = sx0 * H[2] + sy * H[5] + H[8];
            const double da = H[0] * stepX, db = H[1] * stepX, dw = H[2] * stepX;
            for (int xx = 0; xx < pw; xx++, ha += da, hb += db, hw += dw) {
                if (!(hw > 0)) continue;
                const int p = yy * kTile + xx;
                const float iz = (float)hw;
                if (iz < sc.opaqueIz[p]) continue;  // hidden behind this pixel's opaque fragment
                const double iw = 1.0 / hw;
                const double bx = ha * iw * dsX - 0.5 - src.originX, by = hb * iw * dsY - 0.5 - src.originY;
                if (!(bx > -1 && by > -1 && bx < sw && by < sh)) continue;
                const int ix = (int)(bx + 1.0) - 1, iy = (int)(by + 1.0) - 1;  // floor; bx, by > -1
                const float fx = (float)(bx - ix), fy = (float)(by - iy);
                const float ws[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
                __m128 s;
                if (ix >= 0 && iy >= 0 && ix + 1 < sw && iy + 1 < sh) {
                    const P* r0 = (const P*)(base + (size_t)iy * src.rowBytes) + ix;
                    const P* r1 = (const P*)(base + (size_t)(iy + 1) * src.rowBytes) + ix;
                    s = _mm_add_ps(_mm_add_ps(_mm_mul_ps(Premul(Traits<P>::Load(r0)), _mm_set1_ps(ws[0])),
                                              _mm_mul_ps(Premul(Traits<P>::Load(r0 + 1)), _mm_set1_ps(ws[1]))),
                                   _mm_add_ps(_mm_mul_ps(Premul(Traits<P>::Load(r1)), _mm_set1_ps(ws[2])),
                                              _mm_mul_ps(Premul(Traits<P>::Load(r1 + 1)), _mm_set1_ps(ws[3]))));
                } else {
                    s = _mm_setzero_ps();
                    for (int q = 0; q < 2; q++)
                        for (int i2 = 0; i2 < 2; i2++) {
                            int qx = ix + i2, qy = iy + q;
                            if (qx < 0 || qy < 0 || qx >= sw || qy >= sh) continue;
                            const P* pp = (const P*)(base + (size_t)qy * src.rowBytes) + qx;
                            s = _mm_add_ps(s, _mm_mul_ps(Premul(Traits<P>::Load(pp)), _mm_set1_ps(ws[q * 2 + i2])));
                        }
                }
                s = _mm_mul_ps(s, _mm_set1_ps(op));  // premultiplied source x opacity, 0..1
                if (_mm_movemask_ps(_mm_cmpneq_ps(s, _mm_setzero_ps())) == 0) continue;
                Frag* fp = frags + p;  // this pixel's fragment k is fp[k * kTilePx]
                int& n = sc.count[p];
                const bool opaque = _mm_cvtss_f32(s) >= kOpaqueAlpha;
                if (opaque || n == kMaxFrags) {
                    // drop what is now hidden (behind an opaque fragment, this one included)
                    const float cut = std::max(sc.opaqueIz[p], opaque ? iz : 0.f);
                    int m = 0;
                    for (int k = 0; k < n; k++)
                        if (fp[(k) * kTilePx].iz >= cut) fp[(m++) * kTilePx] = fp[(k) * kTilePx];
                    n = m;
                }
                if (n == kMaxFrags) {  // still full: the farthest (lowest rank on a tie) goes under the back
                    int far = 0;
                    for (int k = 1; k < n; k++)
                        if (fp[(k) * kTilePx].iz < fp[(far) * kTilePx].iz || (fp[(k) * kTilePx].iz == fp[(far) * kTilePx].iz && fp[(k) * kTilePx].rank < fp[(far) * kTilePx].rank)) far = k;
                    sc.back[p] = Over(sc.back[p], fp[(far) * kTilePx].c);
                    fp[(far) * kTilePx] = fp[(--n) * kTilePx];
                }
                fp[(n) * kTilePx].c = s;
                fp[(n) * kTilePx].iz = iz;
                fp[(n) * kTilePx].rank = rank;
                n++;
                if (opaque && iz > sc.opaqueIz[p]) {
                    if (sc.opaqueIz[p] == 0.f) opaquePx++;
                    sc.opaqueIz[p] = iz;
                    newOpaque = true;
                }
            }
        }
        if (newOpaque && opaquePx == pw * ph) {
            tileMinOpaque = sc.opaqueIz[0];
            for (int yy = 0; yy < ph; yy++)
                for (int xx = 0; xx < pw; xx++) tileMinOpaque = std::min(tileMinOpaque, sc.opaqueIz[yy * kTile + xx]);
        }
    }
    // resolve: per pixel, far to near (insertion sort; the lists are short)
    for (int yy = 0; yy < ph; yy++) {
        P* out = (P*)((char*)dst.data + (size_t)(py0 + yy) * dst.rowBytes) + px0;
        for (int xx = 0; xx < pw; xx++) {
            const int p = yy * kTile + xx;
            Frag* fp = frags + p;  // this pixel's fragment k is fp[k * kTilePx]
            int n = sc.count[p];
            for (int k = 1; k < n; k++) {
                Frag t = fp[(k) * kTilePx];
                int j = k;
                for (; j > 0 && (fp[(j - 1) * kTilePx].iz > t.iz || (fp[(j - 1) * kTilePx].iz == t.iz && fp[(j - 1) * kTilePx].rank > t.rank)); j--)
                    fp[(j) * kTilePx] = fp[(j - 1) * kTilePx];
                fp[(j) * kTilePx] = t;
            }
            __m128 acc = sc.back[p];
            const float cut = sc.opaqueIz[p];
            for (int k = 0; k < n; k++)
                if (fp[(k) * kTilePx].iz >= cut) acc = Over(fp[(k) * kTilePx].c, acc);
            // back to straight alpha (transparent: colour 0)
            const float al = _mm_cvtss_f32(acc);
            const float kk = al > 0.f ? 1.f / al : 0.f;
            Traits<P>::Store(out[xx], _mm_mul_ps(acc, _mm_set_ps(kk, kk, kk, 1.f)));
        }
    }
}

template <class P>
bool Run(const RasterImage& src, const PreparedCopies& pc, double dsX, double dsY, const RasterImage& dst,
         int threads, const std::function<bool()>& abort)
{
    const int tiles = pc.tiles.tilesX * pc.tiles.tilesY;
    std::atomic<int> next{0};
    std::atomic<bool> stop{false};
    auto worker = [&](bool caller) {
        std::unique_ptr<TileScratch> scp = TakeScratch();
        TileScratch& sc = *scp;
        for (int k; !stop.load(std::memory_order_relaxed) && (k = next.fetch_add(1)) < tiles;) {
            // AE's abort callback is only asked on the calling (render) thread
            if (caller && abort && (k & 15) == 0 && abort()) {
                stop = true;
                break;
            }
            DrawTile<P>(src, pc, k, dsX, dsY, dst, sc);
        }
        ReturnScratch(std::move(scp));
    };
    int t = std::max(1, std::min(threads, tiles));
    std::vector<std::thread> pool;
    try {
        for (int i = 1; i < t; i++) pool.emplace_back(worker, false);
    } catch (...) {
        // no more threads: the ones already started and this one take all the tiles
    }
    worker(true);
    for (auto& th : pool) th.join();
    return !stop.load();
}

}  // namespace

bool RasterizeCopies(const RasterImage& src, RasterCopy* copies, int n, double dsX, double dsY, RasterImage& dst,
                     int threads, const std::function<bool()>& abort)
{
    if (dst.width <= 0 || dst.height <= 0 || !dst.data) return true;
    if (!(dsX > 0) || !(dsY > 0) || src.bpc != dst.bpc || !src.data) n = 0;
    PreparedCopies pc;
    PrepareCopies(src, copies, n, dsX, dsY, dst, kTile, &pc);
    switch (dst.bpc) {
    case 8: return Run<P8>(src, pc, dsX, dsY, dst, threads, abort);
    case 16: return Run<P16>(src, pc, dsX, dsY, dst, threads, abort);
    case 32: return Run<PF>(src, pc, dsX, dsY, dst, threads, abort);
    default: return true;
    }
}

}  // namespace rnd
