// randamu: the fast mode's rasterizer on the GPU. Built into PTX by tools/build_ptx.ps1 (nvcc -ptx) and loaded
// at run time with the CUDA driver API (src/gpu/gpu_raster.cpp). The same drawing as src/core/raster.cpp
// (DrawTile): copies nearest first, per pixel at most MAX_FRAGS fragments, those behind an opaque fragment
// dropped, past the limit the farthest goes under a back accumulator, then far to near by the depth at the
// pixel (equal depths by rank), bilinear sampling, straight alpha in and out.
// One block = one 16x16 tile, one thread = one pixel.

#define MAX_FRAGS 64
#define OPAQUE_ALPHA (1.f - 1.f / 1024.f)

// Must match GCopy in gpu_raster.cpp.
struct GCopy {
    float h[9];     // dst buffer px (x + 0.5 folded in) -> src buffer px, homogeneous; h[2], h[5], h[8]: 1 / z
    float opacity;  // 0..1
    float nearIz;   // unused here (the CPU's tile skip)
    int rank;       // far-to-near rank: equal depths put the higher rank on top
};

__device__ __forceinline__ float4 Premul(float4 p)  // BGRA straight -> premultiplied (w = alpha)
{
    return make_float4(p.x * p.w, p.y * p.w, p.z * p.w, p.w);
}
__device__ __forceinline__ float4 Over(float4 f, float4 b)
{
    float k = 1.f - f.w;
    return make_float4(f.x + b.x * k, f.y + b.y * k, f.z + b.z * k, f.w + b.w * k);
}

extern "C" __global__ void FastRaster(const float4* __restrict__ src, int srcPitch, int srcW, int srcH,
                                      float4* __restrict__ dst, int dstPitch, int dstW, int dstH,
                                      const GCopy* __restrict__ copies, const int* __restrict__ tileStart,
                                      const int* __restrict__ tileIndex, int tilesX)
{
    const int tile = blockIdx.y * tilesX + blockIdx.x;
    const int x = blockIdx.x * 16 + threadIdx.x, y = blockIdx.y * 16 + threadIdx.y;
    if (x >= dstW || y >= dstH) return;
    float4 fc[MAX_FRAGS];
    float fz[MAX_FRAGS];
    int fr[MAX_FRAGS];
    int n = 0;
    float opaqueIz = 0.f;
    float4 back = make_float4(0.f, 0.f, 0.f, 0.f);
    const float px = (float)x, py = (float)y;
    const int j1 = tileStart[tile + 1];
    for (int j = tileStart[tile]; j < j1; j++) {
        const GCopy& c = copies[tileIndex[j]];
        const float hw = px * c.h[2] + py * c.h[5] + c.h[8];
        if (!(hw > 0.f) || hw < opaqueIz) continue;
        const float iw = 1.f / hw;
        const float bx = (px * c.h[0] + py * c.h[3] + c.h[6]) * iw, by = (px * c.h[1] + py * c.h[4] + c.h[7]) * iw;
        if (!(bx > -1.f && by > -1.f && bx < (float)srcW && by < (float)srcH)) continue;
        const int ix = (int)(bx + 1.f) - 1, iy = (int)(by + 1.f) - 1;
        const float fx = bx - ix, fy = by - iy;
        const float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy, w11 = fx * fy;
        float4 s = make_float4(0.f, 0.f, 0.f, 0.f);
        const float ws[4] = {w00, w10, w01, w11};
        for (int q = 0; q < 4; q++) {
            const int qx = ix + (q & 1), qy = iy + (q >> 1);
            if (qx < 0 || qy < 0 || qx >= srcW || qy >= srcH) continue;
            const float4 p = Premul(src[qy * srcPitch + qx]);
            s.x += p.x * ws[q], s.y += p.y * ws[q], s.z += p.z * ws[q], s.w += p.w * ws[q];
        }
        s.x *= c.opacity, s.y *= c.opacity, s.z *= c.opacity, s.w *= c.opacity;
        if (s.x == 0.f && s.y == 0.f && s.z == 0.f && s.w == 0.f) continue;
        const bool opaque = s.w >= OPAQUE_ALPHA;
        if (opaque || n == MAX_FRAGS) {
            const float cut = fmaxf(opaqueIz, opaque ? hw : 0.f);
            int m = 0;
            for (int k = 0; k < n; k++)
                if (fz[k] >= cut) fc[m] = fc[k], fz[m] = fz[k], fr[m] = fr[k], m++;
            n = m;
        }
        if (n == MAX_FRAGS) {
            int far = 0;
            for (int k = 1; k < n; k++)
                if (fz[k] < fz[far] || (fz[k] == fz[far] && fr[k] < fr[far])) far = k;
            back = Over(back, fc[far]);
            n--;
            fc[far] = fc[n], fz[far] = fz[n], fr[far] = fr[n];
        }
        fc[n] = s, fz[n] = hw, fr[n] = c.rank;
        n++;
        if (opaque && hw > opaqueIz) opaqueIz = hw;
    }
    for (int k = 1; k < n; k++) {
        const float4 tc = fc[k];
        const float tz = fz[k];
        const int tr = fr[k];
        int j = k;
        for (; j > 0 && (fz[j - 1] > tz || (fz[j - 1] == tz && fr[j - 1] > tr)); j--)
            fc[j] = fc[j - 1], fz[j] = fz[j - 1], fr[j] = fr[j - 1];
        fc[j] = tc, fz[j] = tz, fr[j] = tr;
    }
    float4 acc = back;
    for (int k = 0; k < n; k++)
        if (fz[k] >= opaqueIz) acc = Over(fc[k], acc);
    const float inv = acc.w > 0.f ? 1.f / acc.w : 0.f;
    dst[y * dstPitch + x] = make_float4(acc.x * inv, acc.y * inv, acc.z * inv, acc.w);
}
