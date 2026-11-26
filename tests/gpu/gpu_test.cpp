// randamu GPU tests (no AE): the CUDA rasterizer against the CPU one, in a CUDA context of our own.
// Build: pwsh tools/build.ps1 -Target gpu  ->  build\gpu\gpu_test.exe. Exit code = number of failed checks.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

#include "core/ae_span.h"
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
                   int dh, int ox, int oy, double ds, double shift, double* gpuMs, int sox = 0, int soy = 0)
{
    std::vector<ARGB> cpu((size_t)dw * dh);
    rnd::RasterImage s = {(void*)src.data(), sw, sh, (long)(sw * sizeof(ARGB)), 32, sox, soy};
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
    rnd::GpuImage gs = {dsrc, sw, sh, (long)(sw * sizeof(BGRA)), sox, soy};
    rnd::GpuImage gd = {ddst, dw, dh, (long)(dw * sizeof(BGRA)), ox, oy, shift, 0};
    auto t0 = std::chrono::steady_clock::now();
    // device memory through an allocator like AE's GPU memory suite: every block allocated is freed
    int allocs = 0, frees = 0;
    rnd::GpuAllocator al;
    al.alloc = [&](size_t bytes) {
        allocs++;
        return rnd::cu::Alloc(bytes);
    };
    al.free = [&](void* p) {
        frees++;
        rnd::cu::Free(p);
    };
    bool ok = rnd::GpuRasterize(ctx, nullptr, gs, gd, copies.data(), (int)copies.size(), ds, ds, {}, &al) == rnd::kGpuOk;
    CHECK(allocs > 0 && allocs == frees);
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

// gpu_ae_span: AE's span drawing on the GPU against captures of AE (RANDAMU_SPAN_CAP: directories separated by ';',
// written by RANDAMU_A3D_DRAW=3): every captured pixel must be AE's.
static std::vector<unsigned char> ReadAll(const std::string& path)
{
    std::vector<unsigned char> b;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") || !f) return b;
    fseek(f, 0, SEEK_END);
    b.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    if (!b.empty() && fread(b.data(), 1, b.size(), f) != b.size()) b.clear();
    fclose(f);
    return b;
}
template <class T> static T At(const std::vector<unsigned char>& b, size_t off)
{
    T v{};
    if (off + sizeof(T) <= b.size()) std::memcpy(&v, b.data() + off, sizeof(T));
    return v;
}
static void GpuSpanCapture(const std::string& dir)
{
    const auto st = ReadAll(dir + "\\state.bin"), pa = ReadAll(dir + "\\params.bin"), sm = ReadAll(dir + "\\samples.bin");
    const auto th = ReadAll(dir + "\\tex_hdr.bin"), tx = ReadAll(dir + "\\tex.bin"), rows = ReadAll(dir + "\\rows.bin");
    CHECK(!st.empty() && !pa.empty() && sm.size() >= 72 && !tx.empty() && !rows.empty());
    if (st.empty() || pa.empty() || sm.size() < 72 || tx.empty() || rows.empty()) return;
    static rnd::AeRcpTable rcp;
    rnd::AeFillRcpTable(&rcp);
    rnd::AeSpanParams p;
    const int si = At<int>(st, 0x30);
    for (int k = 0; k < 9; k++) p.m[k] = At<double>(sm, 72 * (size_t)si + 8 * k);
    p.persp = st[0x98] != 0, p.a0 = At<double>(st, 0xA0), p.quality = st[0xA8];
    p.opacity = At<double>(pa, 0x20), p.colorMode = pa[0x5B];
    // the captured rows into a frame (before), the expected pixels (after)
    struct R { int y, x0, x1; size_t o; };
    std::vector<R> rs;
    int W = 0, H = 0;
    for (size_t o = 0; o + 20 <= rows.size();) {
        const int y = At<int>(rows, o + 4), x0 = At<int>(rows, o + 8), x1 = At<int>(rows, o + 12), n = At<int>(rows, o + 16);
        o += 20;
        if (n > 0) rs.push_back({y, x0, x0 + n / 4, o}), W = std::max(W, x0 + n / 4), H = std::max(H, y + 1);
        o += 2 * (size_t)std::max(n, 0);
    }
    const int rb = W * 4;
    std::vector<unsigned char> frame((size_t)rb * H, 0);
    std::vector<int> tri;
    for (const R& r : rs) {
        std::memcpy(&frame[(size_t)r.y * rb + 4 * r.x0], &rows[r.o], 4 * (size_t)(r.x1 - r.x0));
        tri.push_back(r.y), tri.push_back(r.x0), tri.push_back(r.x1);
    }
    rnd::GpuAeSpanJob j;
    j.core = rnd::AeCoreOf(p);
    j.tex.data = tx.data(), j.tex.rowbytes = At<int>(th, 40), j.tex.width = At<int>(th, 44), j.tex.height = At<int>(th, 48);
    static unsigned long long key = 1;
    j.texKey = key++;
    j.rcp = rcp.r, j.target = frame.data(), j.targetRowbytes = rb, j.rows = tri.data(), j.nRows = (int)rs.size();
    double ms = 0;
    const bool ok = rnd::GpuAeSpans(j, &ms);
    CHECK(ok);
    long long pixels = 0, bad = 0;
    for (const R& r : rs)
        for (int x = r.x0; x < r.x1; x++) {
            pixels++;
            const unsigned char* g = &frame[(size_t)r.y * rb + 4 * x];
            const unsigned char* a = &rows[r.o + 4 * (size_t)(r.x1 - r.x0) + 4 * (size_t)(x - r.x0)];
            if (std::memcmp(g, a, 4)) {
                if (bad < 5) std::printf("  gpu span (%d, %d): gpu %d %d %d %d AE %d %d %d %d\n", x, r.y, g[0], g[1], g[2], g[3], a[0], a[1], a[2], a[3]);
                bad++;
            }
        }
    std::printf("gpu_ae_span %s: %lld pixels, %lld differ, %.2f ms\n", dir.c_str(), pixels, bad, ms);
    CHECK(pixels > 0 && bad == 0);
}

// gpu_ae_batch: a group's copies drawn together on the GPU (GpuAeSpansBatch: overlapping copies in their order)
// against the CPU reference (AeDrawSpan8, AE's arithmetic, one copy after another): every pixel equal. Many small
// copies piled up (like Test C: about 180 spans per row), rotated, minified (area sampling) and in perspective.
static void GpuAeBatch(int n, unsigned seed)
{
    static rnd::AeRcpTable rcp;
    rnd::AeFillRcpTable(&rcp);
    unsigned s = seed;
    auto r01 = [&] { return (s = s * 1103515245u + 12345u, (s >> 8) / 16777216.0); };
    const int W = 1920, H = 1080, tw = 217, th = 216;
    std::vector<unsigned char> tex((size_t)tw * th * 4);
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            unsigned char* p = &tex[((size_t)y * tw + x) * 4];
            const double d = std::hypot(x - tw / 2.0, y - th / 2.0) / (tw / 2.0);
            p[0] = d > 1 ? 0 : (unsigned char)(255 * (1 - d * d * 0.7));  // A (ARGB), clear corners
            p[1] = (unsigned char)(x * 255 / tw), p[2] = (unsigned char)(y * 255 / th), p[3] = (unsigned char)((x ^ y) & 255);
        }
    std::vector<unsigned char> frame((size_t)W * H * 4);
    for (auto& b : frame) b = (unsigned char)(r01() * 256);
    struct J {
        rnd::AeSpanParams p;
        std::vector<int> rows;
    };
    std::vector<J> js((size_t)n);
    for (J& j : js) {
        const double S = 8 + r01() * 70, cx = 300 + r01() * 1300, cy = 100 + r01() * 880, a = r01() * 6.283;
        const double k = tw / S * (0.8 + r01() * 0.4);  // texture px per screen px (> 1: minified)
        const double c = std::cos(a) * k, sn = std::sin(a) * k;
        const bool persp = r01() < 0.5;
        const double wx = persp ? (r01() - 0.5) * 2e-3 : 0, wy = persp ? (r01() - 0.5) * 2e-3 : 0, w0 = 1 - wx * cx - wy * cy;
        // screen (x, y) -> (U, V, W): U = (y m3 + x m0) + m6, ...
        j.p.m[0] = c, j.p.m[3] = sn, j.p.m[6] = tw / 2.0 - c * cx - sn * cy;
        j.p.m[1] = -sn, j.p.m[4] = c, j.p.m[7] = th / 2.0 + sn * cx - c * cy;
        j.p.m[2] = wx, j.p.m[5] = wy, j.p.m[8] = w0;
        j.p.persp = persp, j.p.a0 = 0, j.p.quality = r01() < 0.9 ? 1 : 0;
        j.p.opacity = r01() < 0.3 ? 0.2 + r01() * 0.8 : 1.0, j.p.colorMode = 1, j.p.rcp = &rcp;
        const int y0 = std::max(0, (int)(cy - S * 0.75)), y1 = std::min(H, (int)(cy + S * 0.75));
        for (int y = y0; y < y1; y++) {
            const int x0 = std::max(0, (int)(cx - S * 0.75 + (r01() - 0.5) * 3)), x1 = std::min(W, (int)(cx + S * 0.75));
            if (x1 > x0) j.rows.insert(j.rows.end(), {y, x0, x1});
        }
    }
    rnd::AeTex8 t;
    t.data = tex.data(), t.rowbytes = tw * 4, t.width = tw, t.height = th;
    std::vector<unsigned char> cpu = frame, gpu = frame;
    for (const J& j : js)
        for (size_t r = 0; r + 2 < j.rows.size(); r += 3)
            rnd::AeDrawSpan8(j.p, t, j.rows[r + 1], j.rows[r + 2], j.rows[r], &cpu[(size_t)j.rows[r] * W * 4]);
    std::vector<rnd::GpuAeSpanJob> gj((size_t)n);
    for (int q = 0; q < n; q++) {
        gj[q].core = rnd::AeCoreOf(js[q].p), gj[q].tex = t, gj[q].texKey = 77, gj[q].rcp = rcp.r;
        gj[q].target = gpu.data(), gj[q].targetRowbytes = W * 4, gj[q].rows = js[q].rows.data(), gj[q].nRows = (int)js[q].rows.size() / 3;
    }
    double ms = 0, best = 1e9;
    bool ok = true;
    for (int rep = 0; rep < 3 && ok; rep++) {
        std::vector<unsigned char> g = frame;
        for (auto& x : gj) x.target = g.data();
        ok = rnd::GpuAeSpansBatch(gj.data(), n, &ms);
        best = std::min(best, ms);
        if (rep == 0) gpu = g;
    }
    CHECK(ok);
    long long bad = 0, touched = 0;
    for (size_t i = 0; i < frame.size(); i += 4) {
        touched += std::memcmp(&cpu[i], &frame[i], 4) != 0;
        if (std::memcmp(&cpu[i], &gpu[i], 4)) {
            if (bad < 5) std::printf("  gpu_ae_batch px %zu: gpu %d %d %d %d cpu %d %d %d %d\n", i / 4, gpu[i], gpu[i + 1], gpu[i + 2], gpu[i + 3], cpu[i], cpu[i + 1], cpu[i + 2], cpu[i + 3]);
            bad++;
        }
    }
    std::printf("gpu_ae_batch: %d copies, %lld pixels changed, %lld differ, %.1f ms (best of 3)\n", n, touched, bad, best);
    CHECK(touched > 0 && bad == 0);
}

// gpu_ae_replay (RANDAMU_BATCH_REPLAY=<file>, written by the plugin with RANDAMU_BATCH_DUMP): a real group's batch
// from AE drawn on the GPU and by the CPU reference (AE's arithmetic, in order), every pixel compared, and timed
static void GpuAeReplay(const std::string& path)
{
    const auto b = ReadAll(path);
    if (b.size() < 16) {
        std::printf("FAIL gpu_ae_replay: %s not read\n", path.c_str());
        g_failed++;
        return;
    }
    size_t o = 0;
    const int n = At<int>(b, 0), tw = At<int>(b, 4), th = At<int>(b, 8), trb = At<int>(b, 12);
    o = 16;
    std::vector<unsigned char> tex(b.begin() + o, b.begin() + o + (size_t)trb * th);
    o += (size_t)trb * th;
    float rcp[256];
    std::memcpy(rcp, b.data() + o, sizeof(rcp));
    o += sizeof(rcp);
    std::vector<rnd::AeSpanCore> cores((size_t)n);
    std::vector<std::vector<int>> rows((size_t)n);
    int W = 0, H = 0;
    for (int q = 0; q < n; q++) {
        std::memcpy(&cores[q], b.data() + o, sizeof(rnd::AeSpanCore));
        o += sizeof(rnd::AeSpanCore);
        const int nr = At<int>(b, o);
        o += 4;
        rows[q].resize(3 * (size_t)nr);
        std::memcpy(rows[q].data(), b.data() + o, 12 * (size_t)nr);
        o += 12 * (size_t)nr;
        for (int r = 0; r < nr; r++) W = std::max(W, rows[q][3 * r + 2]), H = std::max(H, rows[q][3 * r] + 1);
    }
    rnd::AeTex8 t;
    t.data = tex.data(), t.rowbytes = trb, t.width = tw, t.height = th;
    std::vector<unsigned char> frame((size_t)W * H * 4);
    unsigned s = 5;
    for (auto& v : frame) v = (unsigned char)((s = s * 1103515245u + 12345u) >> 24);
    std::vector<unsigned char> cpu = frame;
    const auto c0 = std::chrono::steady_clock::now();
    for (int q = 0; q < n; q++) {
        rnd::aeimpl::Samp st;
        const unsigned char op = rnd::aeimpl::OpacityByte(cores[q].opacity);
        for (size_t r = 0; r + 2 < rows[q].size(); r += 3) {
            const int y = rows[q][r], x0 = rows[q][r + 1], x1 = rows[q][r + 2];
            rnd::aeimpl::InitRow(st, cores[q], rnd::aeimpl::SpanStart(x0), rnd::aeimpl::SpanStart(y));
            unsigned char* row = &cpu[(size_t)y * W * 4];
            for (int x = x0; x < x1; x++) {
                rnd::aeimpl::DrawPixel(cores[q], st, t, op, rcp, row + 4 * x);
                rnd::aeimpl::Step(st);
            }
        }
    }
    const double cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
    std::vector<rnd::GpuAeSpanJob> gj((size_t)n);
    double best = 1e9, ms = 0;
    std::vector<unsigned char> gpu;
    bool ok = true;
    for (int rep = 0; rep < 3 && ok; rep++) {
        std::vector<unsigned char> g = frame;
        for (int q = 0; q < n; q++) {
            gj[q].core = cores[q], gj[q].tex = t, gj[q].texKey = 99, gj[q].rcp = rcp, gj[q].target = g.data();
            gj[q].targetRowbytes = W * 4, gj[q].rows = rows[q].data(), gj[q].nRows = (int)rows[q].size() / 3;
        }
        ok = rnd::GpuAeSpansBatch(gj.data(), n, &ms);
        best = std::min(best, ms);
        if (rep == 0) gpu = g;
    }
    CHECK(ok);
    long long bad = 0;
    for (size_t i = 0; i < cpu.size(); i += 4)
        if (std::memcmp(&cpu[i], &gpu[i], 4)) bad++;
    std::printf("gpu_ae_replay %s: %d jobs, %lld pixels differ, gpu %.1f ms (best of 3), cpu %.1f ms\n", path.c_str(), n, bad,
                best, cpuMs);
    CHECK(bad == 0);
}

int main()
{
    {
        char rp[MAX_PATH] = {0};
        if (GetEnvironmentVariableA("RANDAMU_BATCH_REPLAY", rp, sizeof(rp)) > 0) GpuAeReplay(rp);
    }
    rnd::GpuRasterSetLog([](const char* f, ...) {
        va_list a;
        va_start(a, f);
        std::printf("  [gpu] ");
        std::vprintf(f, a);
        std::printf("\n");
        va_end(a);
    });
    GpuAeBatch(300, 1);
    GpuAeBatch(2500, 2);
    {
        char env[4096] = {0};
        if (GetEnvironmentVariableA("RANDAMU_SPAN_CAP", env, sizeof(env)) > 0) {
            std::string all = env;
            size_t a = 0;
            while (a <= all.size()) {
                const size_t b = all.find(';', a);
                const std::string dir = all.substr(a, b == std::string::npos ? std::string::npos : b - a);
                if (!dir.empty()) GpuSpanCapture(dir);
                if (b == std::string::npos) break;
                a = b + 1;
            }
        }
    }
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
    // gpu_origin_half: half resolution with a negative / nonzero source origin (an effect before that grows the
    // layer) and a negative destination origin
    {
        auto copies = Scene(21, 60, sw, sh, 600, 50, 40);
        Compare c = Run(ctx, src, sw, sh, copies, 960, 540, -37, 11, 0.5, 5.5, nullptr, -8, 5);
        std::printf("gpu_origin_half: max %.3f mean %.4f >2: %.4f%% drawn %zu\n", c.maxDiff, c.Mean(), 100 * c.Frac(), c.drawn);
        CHECK(c.drawn > 0 && c.Mean() <= 0.1f && c.Frac() <= 0.001f);
    }
    // gpu_overflow_order: 70 faint copies on one pixel, nearest first; the GPU keeps the CPU's order past 64
    {
        const int n = 70;
        std::vector<ARGB> s(n);
        for (int i = 0; i < n; i++) s[i] = i % 2 ? ARGB{1, 0, 0, 1} : ARGB{1, 1, 0, 0};
        std::vector<rnd::RasterCopy> c;
        for (int i = 0; i < n; i++) {
            const double w = 1.0 + 0.01 * i;
            c.push_back({{w, 0, 0, 0, w, 0, i * w, 0, w}, i == 0 ? 1.0 : 0.05, 1.0 / w});
        }
        Compare r = Run(ctx, s, n, 1, c, 1, 1, 0, 0, 1.0, 0, nullptr);
        std::printf("gpu_overflow_order: max %.4f (8-bit units)\n", r.maxDiff);
        CHECK(r.maxDiff < 0.01f);
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
    // gpu_fallback_cpu: the kernel fails (forced) -> kGpuFailed, the GPU is marked unusable for later frames, and
    // the CPU fallback on the same device buffers draws exactly what the CPU rasterizer draws.
    {
        auto copies = Scene(11, 40, sw, sh, 600, 40, 30);
        const int dw = 640, dh = 360;
        std::vector<ARGB> cpu((size_t)dw * dh);
        rnd::RasterImage s = {(void*)src.data(), sw, sh, (long)(sw * sizeof(ARGB)), 32, 0, 0};
        rnd::RasterImage d = {cpu.data(), dw, dh, (long)(dw * sizeof(ARGB)), 32, -20, 10};
        auto c1 = copies;
        rnd::RasterizeCopies(s, c1.data(), (int)c1.size(), 1.0, 1.0, d, 4);
        std::vector<BGRA> sb(src.size()), out((size_t)dw * dh);
        for (size_t i = 0; i < src.size(); i++) sb[i] = {src[i].b, src[i].g, src[i].r, src[i].a};
        void* dsrc = rnd::cu::Alloc(sb.size() * sizeof(BGRA));
        void* ddst = rnd::cu::Alloc(out.size() * sizeof(BGRA));
        CHECK(dsrc && ddst && rnd::cu::Upload(dsrc, sb.data(), sb.size() * sizeof(BGRA)));
        rnd::GpuImage gs = {dsrc, sw, sh, (long)(sw * sizeof(BGRA)), 0, 0};
        rnd::GpuImage gd = {ddst, dw, dh, (long)(dw * sizeof(BGRA)), -20, 10};
        CHECK(rnd::GpuRasterUsable());
        rnd::GpuRasterTestFail(true);
        CHECK(rnd::GpuRasterize(ctx, nullptr, gs, gd, copies.data(), (int)copies.size(), 1.0, 1.0) == rnd::kGpuFailed);
        rnd::GpuRasterTestFail(false);
        CHECK(!rnd::GpuRasterUsable());
        CHECK(rnd::GpuRasterizeOnCpu(ctx, nullptr, gs, gd, copies.data(), (int)copies.size(), 1.0, 1.0) == rnd::kGpuOk);
        CHECK(rnd::cu::Download(out.data(), ddst, out.size() * sizeof(BGRA)));
        float m = 0;
        size_t drawn = 0;
        for (size_t i = 0; i < out.size(); i++) {
            m = std::max({m, std::fabs(out[i].a - cpu[i].a), std::fabs(out[i].r - cpu[i].r), std::fabs(out[i].g - cpu[i].g),
                          std::fabs(out[i].b - cpu[i].b)});
            drawn += cpu[i].a > 0;
        }
        std::printf("gpu_fallback_cpu: max diff %g, drawn px %zu\n", m, drawn);
        CHECK(drawn > 0 && m == 0.f);
        rnd::GpuRasterTestReset();
        CHECK(rnd::GpuRasterUsable());
        rnd::cu::Free(dsrc);
        rnd::cu::Free(ddst);
    }
    // gpu_bands_abort: a 1920x1080 frame is drawn in several launches (one short launch each, no driver timeout),
    // and the abort callback asked between them stops the drawing.
    {
        auto big = Source(577, 680);
        auto copies = Scene(7, 200, 577, 680, 1500, 60, 100);
        std::vector<BGRA> sb(big.size());
        for (size_t i = 0; i < big.size(); i++) sb[i] = {big[i].b, big[i].g, big[i].r, big[i].a};
        void* dsrc = rnd::cu::Alloc(sb.size() * sizeof(BGRA));
        void* ddst = rnd::cu::Alloc((size_t)1920 * 1080 * sizeof(BGRA));
        CHECK(dsrc && ddst && rnd::cu::Upload(dsrc, sb.data(), sb.size() * sizeof(BGRA)));
        rnd::GpuImage gs = {dsrc, 577, 680, (long)(577 * sizeof(BGRA)), 0, 0};
        rnd::GpuImage gd = {ddst, 1920, 1080, (long)(1920 * sizeof(BGRA)), 0, 0};
        int asked = 0;
        CHECK(rnd::GpuRasterize(ctx, nullptr, gs, gd, copies.data(), (int)copies.size(), 1.0, 1.0, [&] {
                  asked++;
                  return false;
              }) == rnd::kGpuOk);
        std::printf("gpu_bands_abort: launches %d, abort asked %d\n", rnd::GpuRasterLastLaunches(), asked);
        CHECK(rnd::GpuRasterLastLaunches() > 1 && asked >= 1);
        CHECK(rnd::GpuRasterize(ctx, nullptr, gs, gd, copies.data(), (int)copies.size(), 1.0, 1.0, [] { return true; }) ==
              rnd::kGpuAborted);
        CHECK(rnd::GpuRasterUsable());  // an abort is not a failure
        rnd::cu::Free(dsrc);
        rnd::cu::Free(ddst);
    }
    // gpu_prepare_forget: the module is loaded at device setup and dropped at device setdown (AE may destroy the
    // context and a new one may reuse its handle: a cached kernel of the old one must not be used).
    {
        CHECK(rnd::GpuRasterPrepare(ctx));
        CHECK(rnd::GpuRasterCachedContexts() == 1);
        rnd::GpuRasterForget(ctx);
        CHECK(rnd::GpuRasterCachedContexts() == 0);
        CHECK(rnd::GpuRasterPrepare(ctx));
    }
    rnd::cu::DestroyContext(ctx);
    std::printf(g_failed ? "GPU FAILURES: %d\n" : "GPU ALL PASS\n", g_failed);
    return g_failed;
}
