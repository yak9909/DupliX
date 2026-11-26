// randamu: AE's classic 3D span drawing on the GPU (core/ae_span_impl.h, the same arithmetic as the CPU). Built
// into PTX by tools/build_ptx.ps1 with --fmad=false (no contraction: every float operation rounds like AE's SSE
// code) and loaded with the CUDA driver API (src/gpu/gpu_raster.cpp).
// One block = one row span of one surface. Thread 0 walks the span once, keeping (U, V, W) at every chunk start
// (the same additions AE makes, so the values are exact); then each thread draws whole chunks from its start.

#include "../core/ae_span_impl.h"

#define AE_MAX_CHUNKS 256

// The pixels of row r: rows[6 r .. 6 r + 5] = y, x0, x1, the x of the packed segment holding it, the segment's byte
// offset in region (the target's touched row segments packed one after another), the row's surface (cores[]).
// Rows of one launch never touch the same pixel (the caller launches overlapping surfaces in order).
extern "C" __global__ void AeSpanRowsPacked(const rnd::AeSpanCore* __restrict__ cores, const unsigned char* __restrict__ texData,
                                            int texRowbytes, int texW, int texH, const float* __restrict__ rcp,
                                            unsigned char* region, const int* __restrict__ rows, int n)
{
    using namespace rnd::aeimpl;
    const int r = blockIdx.x;
    if (r >= n) return;
    const rnd::AeSpanCore p = cores[rows[6 * r + 5]];
    const int y = rows[6 * r], x0 = rows[6 * r + 1], x1 = rows[6 * r + 2], segX = rows[6 * r + 3], segOff = rows[6 * r + 4];
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
        unsigned char* row = region + segOff + 4 * (long long)(x0 - segX);
        for (int i = k * c; i < e; i++) {
            DrawPixel(p, s, tex, op, rcp, row + 4 * i);
            Step(s);
        }
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
