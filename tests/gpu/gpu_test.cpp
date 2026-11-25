// randamu GPU tests (no AE): the CUDA rasterizer against the CPU one, in a CUDA context of our own.
// Build: pwsh tools/build.ps1 -Target gpu  ->  build\gpu\gpu_test.exe. Exit code = number of failed checks.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/fast_plane.h"
#include "core/raster.h"
#include "core/scatter.h"
#include "core/world.h"
#include "gpu/gpu_raster.h"

static int g_failed = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                        \
    } while (0)

struct ARGB { float a, r, g, b; };  // the CPU's 32 bpc pixel
struct BGRA { float b, g, r, a; };  // AE's GPU pixel

// The same camera matrix as the unit tests' LookAtCamera (AE: y down, the camera looks along +z).
static void LookAt(double px, double py, double pz, double tx, double ty, double tz, double m[16])
{
    double f[3] = {tx - px, ty - py, tz - pz};
    double fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (double& v : f) v /= fl;
    const double d0[3] = {0, 1, 0};
    double r[3] = {d0[1] * f[2] - d0[2] * f[1], d0[2] * f[0] - d0[0] * f[2], d0[0] * f[1] - d0[1] * f[0]};
    double rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (double& v : r) v /= rl;
    double d[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    const double o[16] = {r[0], r[1], r[2], 0, d[0], d[1], d[2], 0, f[0], f[1], f[2], 0, px, py, pz, 1};
    for (int i = 0; i < 16; i++) m[i] = o[i];
}

// The fast mode's copies of a w x h layer (default camera of a 1920x1080 comp, virtual layer at the centre).
static std::vector<rnd::RasterCopy> Scene(int seed, int n, int w, int h, double posRange, double rot, double opMin)
{
    rnd::CameraView cam;
    LookAt(960, 540, -2666.6667, 960, 540, 0, cam.camToWorld);
    cam.dist = 2666.6667;
    cam.compW = 1920;
    cam.compH = 1080;
    rnd::ScatterParams p;
    p.count = n;
    p.seed = seed;
    p.pos[0] = p.pos[1] = posRange;
    p.pos[2] = 2 * posRange;
    p.rotDeg[0] = p.rotDeg[1] = p.rotDeg[2] = rot;
    p.scaleMin = 60, p.scaleMax = 140, p.opacityMin = opMin, p.opacityMax = 100;
    double W[16];
    rnd::InitialLayerToWorld(w / 2.0, h / 2.0, 1920, 1080, W);
    const double rect[4] = {0, 0, (double)w, (double)h};
    std::vector<rnd::RasterCopy> v;
    for (int i = 0; i < n; i++) {
        rnd::CopyXform x;
        rnd::ComputeCopy(p, i, w / 2.0, h / 2.0, &x);
        rnd::RasterCopy c;
        if (!rnd::CopyHomography(cam, W, x.m4, rect, c.screenToSrc)) continue;
        c.opacity = x.opacity;
        c.depth = rnd::CopyDepth(cam, W, x.m4, w / 2.0, h / 2.0);
        v.push_back(c);
    }
    return v;
}

// A cut-out like the user's PNGs: an opaque ellipse with a soft edge and a pattern; transparent around it.
static std::vector<ARGB> Source(int w, int h)
{
    std::vector<ARGB> s((size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            double u = (x - w / 2.0) / (w * 0.45), v = (y - h / 2.0) / (h * 0.45), d = u * u + v * v;
            float a = d > 1.1 ? 0.f : d < 0.9 ? 1.f : (float)((1.1 - d) / 0.2);
            s[(size_t)y * w + x] = {a, (float)((x * 37 % 97) / 96.0), (float)((y * 11 % 53) / 52.0), (float)(((x + y) % 7) / 6.0)};
        }
    return s;
}

struct Compare {
    float maxDiff = 0, sumDiff = 0;
    size_t n = 0;
    size_t drawn = 0;  // CPU pixels with alpha: the scene is not empty
    size_t over2 = 0;  // pixels differing by more than 2 (8-bit units)
    size_t px = 0;
    float Frac() const { return px ? (float)over2 / px : 0.f; }
    float Mean() const { return n ? sumDiff / n : 0.f; }
};

// Draws `copies` with both rasterizers into a dw x dh buffer at origin (ox, oy), downsample ds; returns the
// difference in 8-bit units (the GPU output converted to ARGB).
static Compare Run(void* ctx, const std::vector<ARGB>& src, int sw, int sh, std::vector<rnd::RasterCopy> copies, int dw,
                   int dh, int ox, int oy, double ds, double shift, double* gpuMs)
{
    std::vector<ARGB> cpu((size_t)dw * dh);
    rnd::RasterImage s = {(void*)src.data(), sw, sh, (long)(sw * sizeof(ARGB)), 32, 0, 0};
    rnd::RasterImage d = {cpu.data(), dw, dh, (long)(dw * sizeof(ARGB)), 32, ox, oy};
    d.screenShiftX = shift;
    auto c1 = copies;
    rnd::RasterizeCopies(s, c1.data(), (int)c1.size(), ds, ds, d, 8);

    std::vector<BGRA> sb(src.size()), out((size_t)dw * dh);
    for (size_t i = 0; i < src.size(); i++) sb[i] = {src[i].b, src[i].g, src[i].r, src[i].a};
    void* dsrc = rnd::cu::Alloc(sb.size() * sizeof(BGRA));
    void* ddst = rnd::cu::Alloc(out.size() * sizeof(BGRA));
    Compare r;
    if (!dsrc || !ddst || !rnd::cu::Upload(dsrc, sb.data(), sb.size() * sizeof(BGRA))) {
        CHECK(!"gpu alloc/upload");
        return r;
    }
    rnd::GpuImage gs = {dsrc, sw, sh, (long)(sw * sizeof(BGRA)), 0, 0};
    rnd::GpuImage gd = {ddst, dw, dh, (long)(dw * sizeof(BGRA)), ox, oy, shift, 0};
    auto t0 = std::chrono::steady_clock::now();
    bool ok = rnd::GpuRasterize(ctx, nullptr, gs, gd, copies.data(), (int)copies.size(), ds, ds);
    if (gpuMs) *gpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ok);
    CHECK(rnd::cu::Download(out.data(), ddst, out.size() * sizeof(BGRA)));
    rnd::cu::Free(dsrc);
    rnd::cu::Free(ddst);
    for (size_t i = 0; i < out.size(); i++) {
        const float g[4] = {out[i].a, out[i].r, out[i].g, out[i].b}, c[4] = {cpu[i].a, cpu[i].r, cpu[i].g, cpu[i].b};
        r.drawn += c[0] > 0;
        r.px++;
        float pixMax = 0;
        // colour only matters where there is alpha (straight alpha: compare colour x alpha)
        for (int k = 0; k < 4; k++) {
            float dv = k == 0 ? std::fabs(g[0] - c[0]) : std::fabs(g[k] * g[0] - c[k] * c[0]);
            r.maxDiff = std::max(r.maxDiff, dv * 255.f);
            pixMax = std::max(pixMax, dv * 255.f);
            r.sumDiff += dv * 255.f;
            r.n++;
        }
        r.over2 += pixMax > 2.f;
    }
    return r;
}

int main()
{
    if (!rnd::GpuRasterAvailable() || !rnd::cu::Init()) {
        std::printf("FAIL no CUDA driver\n");
        return 1;
    }
    void* ctx = rnd::cu::CreateContext();
    CHECK(ctx != nullptr);
    if (!ctx) return 1;
    const int sw = 120, sh = 90;
    auto src = Source(sw, sh);

    // gpu_matches_cpu: 20 scenes (1..200 copies, oblique, semi-transparent, ds 1 and 0.5, a screen shift)
    const int counts[5] = {1, 7, 30, 90, 200};
    float worstMax = 0, worstMean = 0, worstFrac = 0;
    for (int sc = 0; sc < 20; sc++) {
        auto copies = Scene(100 + sc, counts[sc % 5], sw, sh, 500 + 100 * (sc % 3), sc % 2 ? 80 : 20, sc % 4 ? 30 : 100);
        const double ds = sc % 3 == 2 ? 0.5 : 1.0;
        Compare c = Run(ctx, src, sw, sh, copies, (int)(1920 * ds), (int)(1080 * ds), 0, 0, ds, sc * 3.25, nullptr);
        CHECK(c.drawn > 0);
        worstMax = std::max(worstMax, c.maxDiff);
        worstMean = std::max(worstMean, c.Mean());
        worstFrac = std::max(worstFrac, c.Frac());
    }
    // The GPU evaluates depth in float: nearly coplanar copies may swap at a few pixels (both orders are right).
    std::printf("gpu_matches_cpu: max %.3f mean %.4f >2: %.4f%% (8-bit units)\n", worstMax, worstMean, 100 * worstFrac);
    CHECK(worstMean <= 0.1f && worstFrac <= 0.001f);

    // gpu_overflow_64: 300 semi-transparent copies stacked on the same pixels
    {
        auto copies = Scene(5, 300, sw, sh, 2, 2, 30);
        Compare c = Run(ctx, src, sw, sh, copies, 320, 180, 800, 450, 1.0, 0, nullptr);
        std::printf("gpu_overflow_64: max %.3f mean %.4f >2: %.4f%%\n", c.maxDiff, c.Mean(), 100 * c.Frac());
        CHECK(c.Mean() <= 0.1f && c.Frac() <= 0.01f);
    }
    // gpu_no_copies: all transparent
    {
        Compare c = Run(ctx, src, sw, sh, {}, 64, 64, 0, 0, 1.0, 0, nullptr);
        CHECK(c.maxDiff == 0.f);
    }
    // gpu_many_copies + speed: 1920x1080, a 577x680 cut-out, 10 / 100 / 1000 / 10000 copies
    {
        auto big = Source(577, 680);
        for (int n : {10, 100, 1000, 10000}) {
            auto copies = Scene(7, n, 577, 680, 1500, 60, 100);
            double ms = 0, best = 1e9;
            Compare c;
            for (int rep = 0; rep < 2; rep++) {
                c = Run(ctx, big, 577, 680, copies, 1920, 1080, 0, 0, 1.0, 0, &ms);
                best = std::min(best, ms);
            }
            std::printf("gpu speed: 1920x1080, %d copies of 577x680: %.1f ms (incl. upload of the copy lists), max diff %.3f, "
                        "mean %.4f >2: %.4f%% drawn px %zu\n", n, best, c.maxDiff, c.Mean(), 100 * c.Frac(), c.drawn);
            CHECK(c.Mean() <= 0.1f && c.Frac() <= 0.001f);
        }
    }
    rnd::cu::DestroyContext(ctx);
    std::printf(g_failed ? "GPU FAILURES: %d\n" : "GPU ALL PASS\n", g_failed);
    return g_failed;
}
