#include "hook/merge_texture.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

#include "core/fast_plane.h"
#include "core/merge_geom.h"
#include "core/raster.h"
#include "effect/randamu.h"
#include "gpu/gpu_raster.h"
#include "hook/bee_hooks.h"

namespace rnd {

AEGP_WorldH RenderGroupTexture(AEGP_WorldH layerTex, const double texM3[9], const MergePlan& plan, const MergeLayerPlan& lp,
                               const MergeCluster& c, int dsfX, int dsfY, double outM3[9])
{
    SPBasicSuite* sp = HookPica();
    AEGP_WorldSuite3* ws = nullptr;
    if (!sp || sp->AcquireSuite(kAEGPWorldSuite, kAEGPWorldSuiteVersion3, (const void**)&ws) || !ws) return nullptr;
    struct Release {
        SPBasicSuite* sp;
        ~Release() { sp->ReleaseSuite(kAEGPWorldSuite, kAEGPWorldSuiteVersion3); }
    } release{sp};

    AEGP_WorldType type = AEGP_WorldType_NONE;
    A_long w = 0, h = 0;
    A_u_long rb = 0;
    void* base = nullptr;
    if (ws->AEGP_GetType(layerTex, &type) || ws->AEGP_GetSize(layerTex, &w, &h) || ws->AEGP_GetRowBytes(layerTex, &rb) ||
        w <= 0 || h <= 0)
        return nullptr;
    int bpc = 0;
    switch (type) {
    case AEGP_WorldType_8: {
        PF_Pixel8* p = nullptr;
        if (!ws->AEGP_GetBaseAddr8(layerTex, &p)) base = p, bpc = 8;
        break;
    }
    case AEGP_WorldType_16: {
        PF_Pixel16* p = nullptr;
        if (!ws->AEGP_GetBaseAddr16(layerTex, &p)) base = p, bpc = 16;
        break;
    }
    case AEGP_WorldType_32: {
        PF_PixelFloat* p = nullptr;
        if (!ws->AEGP_GetBaseAddr32(layerTex, &p)) base = p, bpc = 32;
        break;
    }
    default: break;
    }
    if (!base) return nullptr;

    // the texture's layer rect. Its layer px are the render's downsampled px (at 1/2 resolution AE gives a 60x50
    // texture of a 120x100 layer with an identity matrix): the downsample factor is that rect over the layer's own
    // (full-resolution) rect, and the group is drawn at that resolution.
    double texRect[4], inv[9];
    TextureLayerRect(texM3, (int)w, (int)h, texRect);
    const double* full = lp.in.srcRect;
    if (!Invert3(texM3, inv) || !(texRect[2] > texRect[0]) || !(texRect[3] > texRect[1]) || !(full[2] > full[0]) ||
        !(full[3] > full[1]))
        return nullptr;
    const bool known = dsfX >= 1 && dsfY >= 1;
    const double dsX = known ? 1.0 / dsfX : (texRect[2] - texRect[0]) / (full[2] - full[0]);
    const double dsY = known ? 1.0 / dsfY : (texRect[3] - texRect[1]) / (full[3] - full[1]);
    double gr[4];  // the group's screen rect on whole render pixels (as the Rects hook placed the plane)
    AlignRectToDownsample(c.rect, known ? dsfX : 1, known ? dsfY : 1, gr);
    // the layer rect the group's world spans, in the texture's (downsampled) layer px: the layer's own rect at
    // the render's scale (AE rounds the layer texture up, so texRect may be a little larger)
    double spanRect[4] = {texRect[0], texRect[1], texRect[2], texRect[3]};
    if (known) spanRect[0] = full[0] * dsX, spanRect[1] = full[1] * dsY, spanRect[2] = full[2] * dsX, spanRect[3] = full[3] * dsY;
    const long dw = std::lround((gr[2] - gr[0]) * dsX), dh = std::lround((gr[3] - gr[1]) * dsY);
    {
        static LogLimit logged;
        if (logged.Take(6))
            RLog("merge: group texture from %dx%d bpc %d m3=[%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f] texRect=(%.1f,%.1f,%.1f,%.1f) "
                 "ds=(%.4f,%.4f)%s -> %ldx%ld",
                 (int)w, (int)h, bpc, texM3[0], texM3[1], texM3[2], texM3[3], texM3[4], texM3[5], texM3[6], texM3[7], texM3[8],
                 texRect[0], texRect[1], texRect[2], texRect[3], dsX, dsY, known ? " (render's)" : " (estimated)", dw, dh);
    }
    if (dw <= 0 || dh <= 0) return nullptr;

    AEGP_WorldH out = nullptr;
    A_u_long orb = 0;
    void* obase = nullptr;
    if (ws->AEGP_New(HookPluginId(), type, (A_long)dw, (A_long)dh, &out) || !out) return nullptr;
    bool ok = !ws->AEGP_GetRowBytes(out, &orb);
    if (ok) {
        if (bpc == 8) {
            PF_Pixel8* p = nullptr;
            ok = !ws->AEGP_GetBaseAddr8(out, &p), obase = p;
        } else if (bpc == 16) {
            PF_Pixel16* p = nullptr;
            ok = !ws->AEGP_GetBaseAddr16(out, &p), obase = p;
        } else {
            PF_PixelFloat* p = nullptr;
            ok = !ws->AEGP_GetBaseAddr32(out, &p), obase = p;
        }
    }
    if (!ok || !obase) {
        ws->AEGP_Dispose(out);
        return nullptr;
    }

    // screen px -> texture px: the copy's homography to full-resolution layer px, scaled to the texture's
    // (downsampled) layer px, then the texture matrix's inverse, scaled so that RasterizeCopies' source mapping
    // (u * ds - 0.5 - origin) lands on texture px
    const double Sd[9] = {dsX, 0, 0, 0, dsY, 0, 0, 0, 1}, D[9] = {1 / dsX, 0, 0, 0, 1 / dsY, 0, 0, 0, 1};
    double t1[9], invD[9];
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) t1[r * 3 + k] = Sd[r * 3] * inv[k] + Sd[r * 3 + 1] * inv[3 + k] + Sd[r * 3 + 2] * inv[6 + k];
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) invD[r * 3 + k] = t1[r * 3] * D[k] + t1[r * 3 + 1] * D[3 + k] + t1[r * 3 + 2] * D[6 + k];
    double G[16];
    WorldM4(lp.in.world, lp.in.anchorX, lp.in.anchorY, G);
    const double cx = (full[0] + full[2]) / 2, cy = (full[1] + full[3]) / 2;
    std::vector<RasterCopy> copies;
    copies.reserve(c.copies.size());
    for (int i : c.copies) {
        CopyXform x;
        ComputeCopy(lp.in.p, i, lp.in.anchorX, lp.in.anchorY, &x);
        double m[16], H[9];
        Mul4(x.m4, G, m);
        if (!CopyHomography(plan.cam, lp.in.layerToWorld, m, full, H)) continue;
        RasterCopy rc;
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 3; k++)
                rc.screenToSrc[r * 3 + k] = H[r * 3] * invD[k] + H[r * 3 + 1] * invD[3 + k] + H[r * 3 + 2] * invD[6 + k];
        rc.opacity = x.opacity;
        rc.depth = CopyDepth(plan.cam, lp.in.layerToWorld, m, cx, cy);
        copies.push_back(rc);
    }
    RasterImage src = {base, (int)w, (int)h, (long)rb, bpc, 0, 0};
    RasterImage dst = {obase, (int)dw, (int)dh, (long)orb, bpc, (int)std::lround(gr[0] * dsX),
                       (int)std::lround(gr[1] * dsY)};
    unsigned hw = std::thread::hardware_concurrency();
    LARGE_INTEGER q0, q1, qf;
    QueryPerformanceCounter(&q0);
    // on the GPU when allowed (AE's GPU acceleration is CUDA); any failure draws on the CPU
    bool onGpu = false;
    if (MergeGpuAllowed()) {
        GpuRasterStatus st = GpuRasterizeHost(src, copies.data(), (int)copies.size(), dsX, dsY, dst);
        onGpu = st == kGpuOk;
    }
    if (!onGpu) RasterizeCopies(src, copies.data(), (int)copies.size(), dsX, dsY, dst, hw ? (int)hw : 4);
    QueryPerformanceCounter(&q1);
    {
        static LogLimit timed;
        if (timed.Take(64)) {
            QueryPerformanceFrequency(&qf);
            RLog("merge: group draw %ldx%ld copies %d on the %s in %.1f ms", dw, dh, (int)copies.size(),
                 onGpu ? "GPU" : "CPU", (q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart);
        }
    }
    GroupTextureM3(spanRect, (int)dw, (int)dh, outM3);
    return out;
}

}  // namespace rnd
