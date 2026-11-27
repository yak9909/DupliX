// DupliX: AE's classic 3D span drawing on the GPU (core/ae_span_impl.h, the same arithmetic as the CPU). Built
// into PTX by tools/build_ptx.ps1 with --fmad=false (no contraction: every float operation rounds like AE's SSE
// code) and loaded with the CUDA driver API (src/gpu/gpu_raster.cpp).
// One block = one row span of one surface. Thread 0 walks the span once, keeping (U, V, W) at every chunk start
// (the same additions AE makes, so the values are exact); then each thread draws whole chunks from its start.

#include "../core/ae_span_impl.h"

#define AE_MAX_CHUNKS 256

// The pixels of row span r: rows[8 r .. 8 r + 5] = y, x0, x1, the x of the packed segment holding it, the segment's
// byte offset in region (the target's touched row segments packed one after another), the span's surface (cores[]).
// Drawn by the whole block (thread 0 walks the span, then every thread draws chunks).
__device__ void DrawPackedSpan(const rnd::AeSpanCore* __restrict__ cores, const unsigned char* __restrict__ texData,
                               int texRowbytes, int texW, int texH, const float* __restrict__ rcp, unsigned char* region,
                               const int* __restrict__ rows, int r, double* sU, double* sV, double* sW)
{
    using namespace rnd::aeimpl;
    const rnd::AeSpanCore p = cores[rows[8 * r + 5]];
    const int y = rows[8 * r], x0 = rows[8 * r + 1], x1 = rows[8 * r + 2], segX = rows[8 * r + 3], segOff = rows[8 * r + 4];
    const int len = x1 - x0;
    if (len <= 0) return;
    // chunks of c pixels, one per thread: as many threads busy as the span allows (spans of small copies are short)
    int c = (len + AE_MAX_CHUNKS - 1) / AE_MAX_CHUNKS;
    const int cMin = (len + blockDim.x - 1) / blockDim.x;
    if (c < cMin) c = cMin;
    if (c < 1) c = 1;
    if (threadIdx.x == 0) {
        Samp s;
        InitRow(s, p, SpanStart(x0), SpanStart(y));
        for (int i = 0; i < len; i++) {
            if (i % c == 0) sU[i / c] = s.U, sV[i / c] = s.V, sW[i / c] = s.W;
            s.U = (double)s.dUx + s.U;
            s.V = (double)s.dVx + s.V;
            if (s.dWx != 0.0f) s.W = (double)s.dWx + s.W;
        }
    }
    __syncthreads();
    rnd::AeTex8 tex;
    tex.data = texData, tex.rowbytes = texRowbytes, tex.width = texW, tex.height = texH;
    const unsigned char op = OpacityByte(p.opacity);
    const int nch = (len + c - 1) / c;
    for (int k = threadIdx.x; k < nch; k += blockDim.x) {
        Samp s;
        InitRow(s, p, SpanStart(x0), SpanStart(y));
        s.U = sU[k], s.V = sV[k], s.W = sW[k];
        s.invW = s.persp ? 1.0 / s.W : 1.0;
        const int e = (k + 1) * c < len ? (k + 1) * c : len;
        unsigned char* row = region + segOff + 4 * (long long)(x0 - segX);
        for (int i = k * c; i < e; i++) {
            DrawPixel(p, s, tex, op, rcp, row + 4 * i);
            Step(s);
        }
    }
}

// ---- the tiled drawing (11/26): many spans of a row in parallel, still AE's order at every pixel.
// Spans: sp[8 r .. 8 r + 7] = y, x0, x1, segment x, segment offset, surface, the index of its first kept state, 0.
// AeSpanWalk (a thread per span) walks the span with AE's additions and keeps the state (U, V, W) at every pixel
// x0 < x < x1 with x % AE_TILE == 0. AeSpanTiles (a block per target row, a thread per AE_TILE pixels of it) draws
// its pixels: every span of the row in order, from the state kept at the tile's start (or the span's start).
#define AE_TILE 8
#define AE_BITS_WORDS 4096  // at most 16 KB of shared memory per row block
extern "C" __global__ void AeSpanWalk(const rnd::AeSpanCore* __restrict__ cores, const int* __restrict__ sp, int n,
                                      double* __restrict__ states)
{
    using namespace rnd::aeimpl;
    const int r = blockIdx.x * blockDim.x + threadIdx.x;
    if (r >= n) return;
    const int* e = sp + 8 * r;
    const int y = e[0], x0 = e[1], x1 = e[2];
    double* out = states + 3 * (long long)e[6];
    Samp s;
    InitRow(s, cores[e[5]], SpanStart(x0), SpanStart(y));
    for (int x = x0; x < x1; x++) {
        if (x > x0 && x % AE_TILE == 0) out[0] = s.U, out[1] = s.V, out[2] = s.W, out += 3;
        s.U = (double)s.dUx + s.U;
        s.V = (double)s.dVx + s.V;
        if (s.dWx != 0.0f) s.W = (double)s.dWx + s.W;
    }
}
// span e's state at its pixel xs (x0 <= xs < x1; xs = x0 or a multiple of AE_TILE)
__device__ void StateAt(const rnd::AeSpanCore& p, const int* e, int xs, const double* __restrict__ states,
                        rnd::aeimpl::Samp& s)
{
    using namespace rnd::aeimpl;
    const int x0 = e[1];
    InitRow(s, p, SpanStart(x0), SpanStart(e[0]));
    if (xs > x0) {  // the kept state at the tile's start
        const double* k = states + 3 * ((long long)e[6] + (xs / AE_TILE - (x0 / AE_TILE + 1)));
        s.U = k[0], s.V = k[1], s.W = k[2];
        s.invW = s.persp ? 1.0 / s.W : 1.0;
    }
}
// cull (exact): a pixel whose sample covers the target (XferAlpha 0xFF: the transfer replaces the pixel) hides every
// span drawn before it there. The spans are first walked from the last, sampling only the pixels that look opaque
// (op 0xFF and the nearest texel's alpha 0xFF: a guess, confirmed by the sample itself); the last covering span's
// sample is kept, and the forward drawing starts after it. The result is the forward drawing's, pixel for pixel.
extern "C" __global__ void AeSpanTiles(const rnd::AeSpanCore* __restrict__ cores, const unsigned char* __restrict__ texData,
                                       int texRowbytes, int texW, int texH, const float* __restrict__ rcp,
                                       unsigned char* region, const int* __restrict__ sp, const int* __restrict__ gStart,
                                       const int* __restrict__ gSpan, int nGroups, const double* __restrict__ states, int cull,
                                       int bitsWords)
{
    using namespace rnd::aeimpl;
    const int g = blockIdx.x;
    if (g >= nGroups) return;
    const int a = gStart[g], b = gStart[g + 1];
    const int t0 = gSpan[2 * g] / AE_TILE, t1 = (gSpan[2 * g + 1] - 1) / AE_TILE;  // the row's tiles
    rnd::AeTex8 tex;
    tex.data = texData, tex.rowbytes = texRowbytes, tex.width = texW, tex.height = texH;
    // the spans touching each tile, as a bit set per tile in shared memory (bit k: span a + k): a tile visits only
    // those, in AE's order. A row with more spans than the set holds: every span, each checked.
    extern __shared__ unsigned bits[];  // bitsWords words (the launch's dynamic shared memory)
    const int nW = (b - a + 31) / 32, nT = t1 - t0 + 1;
    const bool useBits = (long long)nW * nT <= bitsWords;
    if (useBits) {
        for (int i = threadIdx.x; i < nW * nT; i += blockDim.x) bits[i] = 0;
        __syncthreads();
        for (int r = a + (int)threadIdx.x; r < b; r += blockDim.x) {
            const int* e = sp + 8 * r;
            if (e[2] <= e[1]) continue;
            const int ta = e[1] / AE_TILE > t0 ? e[1] / AE_TILE : t0, tb = (e[2] - 1) / AE_TILE;
            for (int tt = ta; tt <= tb; tt++) atomicOr(&bits[(tt - t0) * nW + (r - a) / 32], 1u << ((r - a) & 31));
        }
        __syncthreads();
    }
    for (int t = t0 + (int)threadIdx.x; t <= t1; t += blockDim.x) {
        const int tx0 = t * AE_TILE, tx1 = tx0 + AE_TILE;
        const unsigned* tb = bits + (t - t0) * nW;
        // the spans touching the tile, in order (f(r) returns false to stop)
        auto forward = [&](auto&& f) {
            if (useBits) {
                for (int w = 0; w < nW; w++)
                    for (unsigned m = tb[w]; m; m &= m - 1)
                        if (!f(a + w * 32 + __ffs(m) - 1)) return;
            } else {
                for (int r = a; r < b; r++) {
                    const int* e = sp + 8 * r;
                    if (e[2] <= tx0 || e[1] >= tx1) continue;
                    if (!f(r)) return;
                }
            }
        };
        auto backward = [&](auto&& f) {
            if (useBits) {
                for (int w = nW - 1; w >= 0; w--)
                    for (unsigned m = tb[w]; m;) {
                        const int bit = 31 - __clz(m);
                        m &= ~(1u << bit);
                        if (!f(a + w * 32 + bit)) return;
                    }
            } else {
                for (int r = b - 1; r >= a; r--) {
                    const int* e = sp + 8 * r;
                    if (e[2] <= tx0 || e[1] >= tx1) continue;
                    if (!f(r)) return;
                }
            }
        };
        int from[AE_TILE];  // per pixel: the first span to draw (after the last covering one)
        unsigned char top[AE_TILE][4];  // the covering span's sample
        unsigned covered = 0;
        for (int k = 0; k < AE_TILE; k++) from[k] = a;
        if (cull & 1) {
            backward([&](int r) {
                const int* e = sp + 8 * r;
                const rnd::AeSpanCore p = cores[e[5]];
                const unsigned char op = OpacityByte(p.opacity);
                if (op != 0xFF) return true;
                const int xs = e[1] > tx0 ? e[1] : tx0, xe = e[2] < tx1 ? e[2] : tx1;
                Samp s;
                StateAt(p, e, xs, states, s);
                for (int x = xs; x < xe; x++) {
                    const int k = x - tx0;
                    if (!(covered >> k & 1)) {
                        // the guess: the nearest texel's alpha
                        const float iw = (float)s.invW;
                        const int u = (int)floorf((float)s.U * iw), v = (int)floorf((float)s.V * iw);
                        if (u >= 0 && v >= 0 && u < texW && v < texH && texData[(long long)v * texRowbytes + 4 * u] == 0xFF) {
                            unsigned char px[4];
                            SamplePixel(p, s, tex, px);
                            if (XferAlpha(px, op) == 0xFF) {
                                covered |= 1u << k, from[k] = r + 1;
                                top[k][0] = px[0], top[k][1] = px[1], top[k][2] = px[2], top[k][3] = px[3];
                            }
                        }
                    }
                    Step(s);
                }
                return covered != (1u << AE_TILE) - 1;
            });
            // the covered pixels: the covering sample replaces the target (XferInFront8 with alpha 0xFF)
            if (covered)
                forward([&](int r) {
                    const int* e = sp + 8 * r;
                    const int xs = e[1] > tx0 ? e[1] : tx0, xe = e[2] < tx1 ? e[2] : tx1;
                    unsigned char* row = region + e[4] + 4 * (long long)(xs - e[3]);
                    for (int x = xs; x < xe; x++)
                        if (covered >> (x - tx0) & 1) {
                            const int k = x - tx0;
                            unsigned char* d = row + 4 * (x - xs);
                            d[0] = top[k][0], d[1] = top[k][1], d[2] = top[k][2], d[3] = top[k][3];
                        }
                    return true;
                });
        }
        forward([&](int r) {
            const int* e = sp + 8 * r;
            const int xs = e[1] > tx0 ? e[1] : tx0, xe = e[2] < tx1 ? e[2] : tx1;
            bool any = false;
            for (int x = xs; x < xe && !any; x++) any = r >= from[x - tx0];
            if (!any) return true;
            const rnd::AeSpanCore p = cores[e[5]];
            Samp s;
            StateAt(p, e, xs, states, s);
            const unsigned char op = OpacityByte(p.opacity);
            unsigned char* row = region + e[4] + 4 * (long long)(xs - e[3]);
            for (int x = xs; x < xe; x++, row += 4) {
                if (r >= from[x - tx0] && !(cull & 2)) DrawPixel(p, s, tex, op, rcp, row);
                Step(s);
            }
            return true;
        });
    }
}

// The same drawing with a thread per pixel (11/27): the tiles' 8 pixels in 8 threads. A pixel's state is the kept
// state at its tile's start (or the span's start) stepped to the pixel with the same additions, so every pixel is
// the tiled drawing's, bit for bit. Faster where few spans are long and sampling is costly (a large texture shrunk:
// AE averages every texel under the pixel), the tiles' thread drew 8 such pixels one after another.
// span e's state at its pixel x (x0 <= x < x1), from the kept state at or before x
__device__ void StateAtPixel(const rnd::AeSpanCore& p, const int* e, int x, const double* __restrict__ states,
                             rnd::aeimpl::Samp& s)
{
    const int t = x / AE_TILE * AE_TILE, xs = e[1] > t ? e[1] : t;
    StateAt(p, e, xs, states, s);
    if (x == xs) return;
    for (int k = xs; k < x; k++) {
        s.U = (double)s.dUx + s.U;
        s.V = (double)s.dVx + s.V;
        if (s.dWx != 0.0f) s.W = (double)s.dWx + s.W;
    }
    if (s.dWx != 0.0f && s.persp) s.invW = 1.0 / s.W;
}
extern "C" __global__ void AeSpanPixels(const rnd::AeSpanCore* __restrict__ cores, const unsigned char* __restrict__ texData,
                                        int texRowbytes, int texW, int texH, const float* __restrict__ rcp,
                                        unsigned char* region, const int* __restrict__ sp, const int* __restrict__ gStart,
                                        const int* __restrict__ gSpan, int nGroups, const double* __restrict__ states, int cull,
                                        int bitsWords)
{
    using namespace rnd::aeimpl;
    const int g = blockIdx.x;
    if (g >= nGroups) return;
    const int a = gStart[g], b = gStart[g + 1];
    const int t0 = gSpan[2 * g] / AE_TILE, t1 = (gSpan[2 * g + 1] - 1) / AE_TILE;
    rnd::AeTex8 tex;
    tex.data = texData, tex.rowbytes = texRowbytes, tex.width = texW, tex.height = texH;
    extern __shared__ unsigned bits[];
    const int nW = (b - a + 31) / 32, nT = t1 - t0 + 1;
    const bool useBits = (long long)nW * nT <= bitsWords;
    if (useBits) {
        for (int i = threadIdx.x; i < nW * nT; i += blockDim.x) bits[i] = 0;
        __syncthreads();
        for (int r = a + (int)threadIdx.x; r < b; r += blockDim.x) {
            const int* e = sp + 8 * r;
            if (e[2] <= e[1]) continue;
            const int ta = e[1] / AE_TILE > t0 ? e[1] / AE_TILE : t0, tb = (e[2] - 1) / AE_TILE;
            for (int tt = ta; tt <= tb; tt++) atomicOr(&bits[(tt - t0) * nW + (r - a) / 32], 1u << ((r - a) & 31));
        }
        __syncthreads();
    }
    for (int x = gSpan[2 * g] + (int)threadIdx.x; x < gSpan[2 * g + 1]; x += blockDim.x) {
        const unsigned* tb = bits + (x / AE_TILE - t0) * nW;
        // the spans holding pixel x, in order (f(r) returns false to stop)
        auto forward = [&](auto&& f) {
            if (useBits) {
                for (int w = 0; w < nW; w++)
                    for (unsigned m = tb[w]; m; m &= m - 1) {
                        const int r = a + w * 32 + __ffs(m) - 1;
                        const int* e = sp + 8 * r;
                        if (e[1] <= x && x < e[2] && !f(r)) return;
                    }
            } else {
                for (int r = a; r < b; r++) {
                    const int* e = sp + 8 * r;
                    if (e[1] <= x && x < e[2] && !f(r)) return;
                }
            }
        };
        auto backward = [&](auto&& f) {
            if (useBits) {
                for (int w = nW - 1; w >= 0; w--)
                    for (unsigned m = tb[w]; m;) {
                        const int bit = 31 - __clz(m);
                        m &= ~(1u << bit);
                        const int r = a + w * 32 + bit;
                        const int* e = sp + 8 * r;
                        if (e[1] <= x && x < e[2] && !f(r)) return;
                    }
            } else {
                for (int r = b - 1; r >= a; r--) {
                    const int* e = sp + 8 * r;
                    if (e[1] <= x && x < e[2] && !f(r)) return;
                }
            }
        };
        int from = a;  // the first span to draw (after the last covering one)
        if (cull & 1) {
            backward([&](int r) {
                const int* e = sp + 8 * r;
                const rnd::AeSpanCore p = cores[e[5]];
                const unsigned char op = OpacityByte(p.opacity);
                if (op != 0xFF) return true;
                Samp s;
                StateAtPixel(p, e, x, states, s);
                const float iw = (float)s.invW;
                const int u = (int)floorf((float)s.U * iw), v = (int)floorf((float)s.V * iw);
                if (u >= 0 && v >= 0 && u < texW && v < texH && texData[(long long)v * texRowbytes + 4 * u] == 0xFF) {
                    unsigned char px[4];
                    SamplePixel(p, s, tex, px);
                    if (XferAlpha(px, op) == 0xFF) {
                        // the covering sample replaces the target (XferInFront8 with alpha 0xFF)
                        unsigned char* d = region + e[4] + 4 * (long long)(x - e[3]);
                        d[0] = px[0], d[1] = px[1], d[2] = px[2], d[3] = px[3];
                        from = r + 1;
                        return false;
                    }
                }
                return true;
            });
        }
        forward([&](int r) {
            if (r < from) return true;
            const int* e = sp + 8 * r;
            const rnd::AeSpanCore p = cores[e[5]];
            Samp s;
            StateAtPixel(p, e, x, states, s);
            if (!(cull & 2)) DrawPixel(p, s, tex, OpacityByte(p.opacity), rcp, region + e[4] + 4 * (long long)(x - e[3]));
            return true;
        });
    }
}

// ---- the fragments of spans into AE's M buffer (sub_1800477F0, core/ae_span_impl.h's MbufDepth): a block per span
// (sp as AeSpanWalk's, e[4] = the span's first pixel in col / depth), a thread per pixel from the kept states
extern "C" __global__ void AeMbufSpans(const rnd::AeSpanCore* __restrict__ cores, const unsigned char* __restrict__ texData,
                                       int texRowbytes, int texW, int texH, unsigned char* __restrict__ col,
                                       float* __restrict__ depth, const int* __restrict__ sp, int n,
                                       const double* __restrict__ states)
{
    using namespace rnd::aeimpl;
    const int r = blockIdx.x;
    if (r >= n) return;
    const int* e = sp + 8 * r;
    const int x0 = e[1], len = e[2] - e[1];
    const rnd::AeSpanCore p = cores[e[5]];
    rnd::AeTex8 tex;
    tex.data = texData, tex.rowbytes = texRowbytes, tex.width = texW, tex.height = texH;
    for (int i = threadIdx.x; i < len; i += blockDim.x) {
        Samp s;
        StateAtPixel(p, e, x0 + i, states, s);
        const float off = MbufOff(s, p.zoff);  // (dWx, dWy: InitRow's, the same at every pixel)
        const long long o = (long long)e[4] + i;
        SamplePixel(p, s, tex, col + 4 * o);
        depth[o] = MbufDepth(s, off, MbufPairFirst(i, len));
    }
}

// One block = one target row: its spans gStart[g] .. gStart[g + 1] - 1, in AE's drawing order, one after another
// (spans of one row may overlap; rows never touch each other's pixels). One launch draws every surface of a group.
extern "C" __global__ void AeSpanRowsOrdered(const rnd::AeSpanCore* __restrict__ cores, const unsigned char* __restrict__ texData,
                                             int texRowbytes, int texW, int texH, const float* __restrict__ rcp,
                                             unsigned char* region, const int* __restrict__ rows,
                                             const int* __restrict__ gStart, int nGroups)
{
    const int g = blockIdx.x;
    if (g >= nGroups) return;
    __shared__ double sU[AE_MAX_CHUNKS], sV[AE_MAX_CHUNKS], sW[AE_MAX_CHUNKS];
    for (int r = gStart[g]; r < gStart[g + 1]; r++) {
        DrawPackedSpan(cores, texData, texRowbytes, texW, texH, rcp, region, rows, r, sU, sV, sW);
        __syncthreads();  // this span's pixels and the shared chunk starts are done before the next span
    }
}

extern "C" __global__ void AeSpanRows(rnd::AeSpanCore p, const unsigned char* __restrict__ texData, int texRowbytes,
                                      int texW, int texH, const float* __restrict__ rcp, unsigned char* region,
                                      int pitch, int ox, int oy, const int* __restrict__ rows, int n)
{
    using namespace rnd::aeimpl;
    const int r = blockIdx.x;
    if (r >= n) return;
    const int y = rows[3 * r], x0 = rows[3 * r + 1], x1 = rows[3 * r + 2];
    const int len = x1 - x0;
    if (len <= 0) return;
    __shared__ double sU[AE_MAX_CHUNKS], sV[AE_MAX_CHUNKS], sW[AE_MAX_CHUNKS];
    int c = (len + AE_MAX_CHUNKS - 1) / AE_MAX_CHUNKS;
    if (c < 16) c = 16;
    if (threadIdx.x == 0) {
        Samp s;
        InitRow(s, p, SpanStart(x0), SpanStart(y));
        for (int i = 0; i < len; i++) {
            if (i % c == 0) sU[i / c] = s.U, sV[i / c] = s.V, sW[i / c] = s.W;
            s.U = (double)s.dUx + s.U;
            s.V = (double)s.dVx + s.V;
            if (s.dWx != 0.0f) s.W = (double)s.dWx + s.W;
        }
    }
    __syncthreads();
    rnd::AeTex8 tex;
    tex.data = texData, tex.rowbytes = texRowbytes, tex.width = texW, tex.height = texH;
    const unsigned char op = OpacityByte(p.opacity);
    const int nch = (len + c - 1) / c;
    for (int k = threadIdx.x; k < nch; k += blockDim.x) {
        Samp s;
        InitRow(s, p, SpanStart(x0), SpanStart(y));
        s.U = sU[k], s.V = sV[k], s.W = sW[k];
        s.invW = s.persp ? 1.0 / s.W : 1.0;
        const int e = (k + 1) * c < len ? (k + 1) * c : len;
        unsigned char* row = region + (long long)(y - oy) * pitch + 4 * (long long)(x0 - ox);
        for (int i = k * c; i < e; i++) {
            DrawPixel(p, s, tex, op, rcp, row + 4 * i);
            Step(s);
        }
    }
}
