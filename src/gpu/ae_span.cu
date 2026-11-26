// randamu: AE's classic 3D span drawing on the GPU (core/ae_span_impl.h, the same arithmetic as the CPU). Built
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
extern "C" __global__ void AeSpanTiles(const rnd::AeSpanCore* __restrict__ cores, const unsigned char* __restrict__ texData,
                                       int texRowbytes, int texW, int texH, const float* __restrict__ rcp,
                                       unsigned char* region, const int* __restrict__ sp, const int* __restrict__ gStart,
                                       const int* __restrict__ gSpan, int nGroups, const double* __restrict__ states)
{
    using namespace rnd::aeimpl;
    const int g = blockIdx.x;
    if (g >= nGroups) return;
    const int a = gStart[g], b = gStart[g + 1];
    const int t0 = gSpan[2 * g] / AE_TILE, t1 = (gSpan[2 * g + 1] - 1) / AE_TILE;  // the row's tiles
    rnd::AeTex8 tex;
    tex.data = texData, tex.rowbytes = texRowbytes, tex.width = texW, tex.height = texH;
    for (int t = t0 + (int)threadIdx.x; t <= t1; t += blockDim.x) {
        const int tx0 = t * AE_TILE, tx1 = tx0 + AE_TILE;
        for (int r = a; r < b; r++) {
            const int* e = sp + 8 * r;
            const int x0 = e[1], x1 = e[2];
            if (x1 <= tx0 || x0 >= tx1) continue;
            const rnd::AeSpanCore p = cores[e[5]];
            Samp s;
            InitRow(s, p, SpanStart(x0), SpanStart(e[0]));
            const int xs = x0 > tx0 ? x0 : tx0;
            if (xs > x0) {  // the kept state at the tile's start
                const double* k = states + 3 * ((long long)e[6] + (xs / AE_TILE - (x0 / AE_TILE + 1)));
                s.U = k[0], s.V = k[1], s.W = k[2];
                s.invW = s.persp ? 1.0 / s.W : 1.0;
            }
            const unsigned char op = OpacityByte(p.opacity);
            unsigned char* row = region + e[4] + 4 * (long long)(xs - e[3]);
            const int xe = x1 < tx1 ? x1 : tx1;
            for (int x = xs; x < xe; x++, row += 4) {
                DrawPixel(p, s, tex, op, rcp, row);
                Step(s);
            }
        }
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
