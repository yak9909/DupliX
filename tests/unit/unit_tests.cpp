// randamu unit tests (no framework). Exit code = number of failed checks.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>

#include "core/fast_plane.h"
#include "core/raster.h"
#include "core/scatter.h"
#include "core/world.h"
#include "hook/flatten_site.h"
#include "hook/registry.h"
#include "hook/bee_abi.h"

static int g_failed = 0;
static int g_checks = 0;
static const char* g_test = "";

#define CHECK(cond)                                                                     \
    do {                                                                                \
        ++g_checks;                                                                     \
        if (!(cond)) {                                                                  \
            ++g_failed;                                                                 \
            std::printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_test, #cond);     \
        }                                                                               \
    } while (0)
#define CHECK_NEAR(a, b, eps)                                                                          \
    do {                                                                                               \
        ++g_checks;                                                                                    \
        double va_ = (a), vb_ = (b);                                                                   \
        if (!(std::fabs(va_ - vb_) <= (eps))) {                                                        \
            ++g_failed;                                                                                \
            std::printf("FAIL %s:%d [%s] %s=%.12g vs %s=%.12g\n", __FILE__, __LINE__, g_test, #a, va_, #b, vb_); \
        }                                                                                              \
    } while (0)
#define TEST(name) static void name(); struct name##_reg { name##_reg() { Register(#name, name); } } name##_inst; static void name()

typedef void (*TestFn)();
static struct { const char* name; TestFn fn; } g_tests[256];
static int g_numTests = 0;
static void Register(const char* n, TestFn f) { g_tests[g_numTests].name = n; g_tests[g_numTests].fn = f; ++g_numTests; }

// ---- helpers
static void Apply(const double m[16], double x, double y, double z, double o[3])
{
    for (int c = 0; c < 3; c++) o[c] = x * m[0 * 4 + c] + y * m[1 * 4 + c] + z * m[2 * 4 + c] + m[3 * 4 + c];
}
static void Mul(const double a[16], const double b[16], double o[16])
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
            o[r * 4 + c] = s;
        }
}
static rnd::ScatterParams Zero()
{
    rnd::ScatterParams p;
    p.count = 5;
    return p;
}

// ---- core tests
TEST(rand_deterministic)
{
    double a = rnd::Rand01(7, 3, 2), b = rnd::Rand01(7, 3, 2);
    CHECK(a == b);
    CHECK(a >= 0.0 && a < 1.0);
    CHECK(rnd::Rand01(7, 3, 2) != rnd::Rand01(8, 3, 2));
    CHECK(rnd::Rand01(7, 3, 2) != rnd::Rand01(7, 4, 2));
    CHECK(rnd::Rand01(7, 3, 2) != rnd::Rand01(7, 3, 1));
}

TEST(rand_stable_when_count_grows)
{
    rnd::ScatterParams p = Zero();
    p.pos[0] = 100; p.pos[1] = 50; p.pos[2] = 300; p.rotDeg[2] = 45; p.scaleMin = 50; p.scaleMax = 150;
    rnd::CopyXform a, b;
    p.count = 5;
    rnd::ComputeCopy(p, 3, 10, 20, &a);
    p.count = 50;
    rnd::ComputeCopy(p, 3, 10, 20, &b);
    for (int k = 0; k < 16; k++) CHECK(a.m4[k] == b.m4[k]);
    CHECK(a.opacity == b.opacity);
}

TEST(zero_ranges_identity)
{
    rnd::ScatterParams p = Zero();
    for (int i = 0; i < 5; i++) {
        rnd::CopyXform x;
        rnd::ComputeCopy(p, i, 33, 44, &x);
        for (int k = 0; k < 16; k++) CHECK_NEAR(x.m4[k], (k % 5 == 0) ? 1.0 : 0.0, 1e-12);
        CHECK_NEAR(x.opacity, 1.0, 1e-12);
    }
}

TEST(translate_only)
{
    rnd::ScatterParams p = Zero();
    p.pos[0] = 100; p.pos[1] = 50; p.pos[2] = 300;
    bool anyNonZero = false;
    for (int i = 0; i < 20; i++) {
        rnd::CopyXform x;
        rnd::ComputeCopy(p, i, 0, 0, &x);
        CHECK(std::fabs(x.m4[12]) <= 100 && std::fabs(x.m4[13]) <= 50 && std::fabs(x.m4[14]) <= 300);
        CHECK_NEAR(x.m4[15], 1.0, 1e-12);
        for (int k = 0; k < 12; k++) CHECK_NEAR(x.m4[k], (k % 5 == 0) ? 1.0 : 0.0, 1e-12);
        if (x.m4[12] != 0) anyNonZero = true;
    }
    CHECK(anyNonZero);
}

TEST(rotation_about_anchor)
{
    double m[16], o[3];
    rnd::detail::MakeM4(0, 0, 0, 0, 0, 90, 1, 50, 50, m);
    Apply(m, 50, 50, 0, o);
    CHECK_NEAR(o[0], 50, 1e-9); CHECK_NEAR(o[1], 50, 1e-9); CHECK_NEAR(o[2], 0, 1e-9);
    Apply(m, 100, 50, 0, o);
    CHECK_NEAR(o[0], 50, 1e-9); CHECK_NEAR(o[1], 100, 1e-9); CHECK_NEAR(o[2], 0, 1e-9);
}

TEST(rotation_order_zyx)
{
    const double d2r = 3.14159265358979323846 / 180.0;
    double cx = std::cos(20 * d2r), sx = std::sin(20 * d2r);
    double cy = std::cos(30 * d2r), sy = std::sin(30 * d2r);
    double cz = std::cos(40 * d2r), sz = std::sin(40 * d2r);
    const double Rx[16] = {1, 0, 0, 0, 0, cx, sx, 0, 0, -sx, cx, 0, 0, 0, 0, 1};
    const double Ry[16] = {cy, 0, -sy, 0, 0, 1, 0, 0, sy, 0, cy, 0, 0, 0, 0, 1};
    const double Rz[16] = {cz, sz, 0, 0, -sz, cz, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    double zy[16], zyx[16], m[16];
    Mul(Rz, Ry, zy);
    Mul(zy, Rx, zyx);
    rnd::detail::MakeM4(0, 0, 0, 20, 30, 40, 1, 0, 0, m);
    for (int k = 0; k < 16; k++) CHECK_NEAR(m[k], zyx[k], 1e-12);
}

TEST(scale_about_anchor)
{
    double m[16], o[3];
    rnd::detail::MakeM4(0, 0, 0, 0, 0, 0, 2, 50, 50, m);
    Apply(m, 60, 50, 0, o);
    CHECK_NEAR(o[0], 70, 1e-9); CHECK_NEAR(o[1], 50, 1e-9); CHECK_NEAR(o[2], 0, 1e-9);
    Apply(m, 0, 0, 10, o);
    CHECK_NEAR(o[0], -50, 1e-9); CHECK_NEAR(o[1], -50, 1e-9); CHECK_NEAR(o[2], 20, 1e-9);
}

TEST(translation_after_rotation)
{
    double m[16], o[3];
    rnd::detail::MakeM4(10, 20, 30, 0, 0, 90, 1, 50, 50, m);
    Apply(m, 50, 50, 0, o);  // the anchor ends at A + t
    CHECK_NEAR(o[0], 60, 1e-9); CHECK_NEAR(o[1], 70, 1e-9); CHECK_NEAR(o[2], 30, 1e-9);
}

TEST(keep_first)
{
    rnd::ScatterParams p = Zero();
    p.pos[0] = 100; p.rotDeg[1] = 60; p.scaleMin = 50; p.scaleMax = 200; p.opacityMin = 10; p.opacityMax = 90;
    p.keepFirst = true;
    rnd::CopyXform a0, a1, b1;
    rnd::ComputeCopy(p, 0, 5, 5, &a0);
    for (int k = 0; k < 16; k++) CHECK_NEAR(a0.m4[k], (k % 5 == 0) ? 1.0 : 0.0, 1e-12);
    CHECK_NEAR(a0.opacity, 1.0, 1e-12);
    rnd::ComputeCopy(p, 1, 5, 5, &a1);
    p.keepFirst = false;
    rnd::ComputeCopy(p, 1, 5, 5, &b1);
    for (int k = 0; k < 16; k++) CHECK(a1.m4[k] == b1.m4[k]);
    CHECK(a1.opacity == b1.opacity);
}

TEST(opacity_range)
{
    rnd::ScatterParams p = Zero();
    p.opacityMin = 30; p.opacityMax = 30;
    rnd::CopyXform x;
    rnd::ComputeCopy(p, 2, 0, 0, &x);
    CHECK_NEAR(x.opacity, 0.30, 1e-12);
    p.opacityMin = 20; p.opacityMax = 60;
    for (int i = 0; i < 20; i++) {
        rnd::ComputeCopy(p, i, 0, 0, &x);
        CHECK(x.opacity >= 0.20 - 1e-12 && x.opacity <= 0.60 + 1e-12);
    }
}

TEST(scale_range)
{
    rnd::ScatterParams p = Zero();
    p.scaleMin = 50; p.scaleMax = 200;
    for (int i = 0; i < 20; i++) {
        rnd::CopyXform x;
        rnd::ComputeCopy(p, i, 0, 0, &x);
        CHECK(x.m4[0] >= 0.5 - 1e-12 && x.m4[0] <= 2.0 + 1e-12);
        CHECK_NEAR(x.m4[0], x.m4[5], 1e-12);
        CHECK_NEAR(x.m4[0], x.m4[10], 1e-12);
    }
}

TEST(count_zero)
{
    rnd::ScatterParams p = Zero();
    p.count = 0;
    rnd::CopyXform x;
    rnd::ComputeCopy(p, 0, 0, 0, &x);  // must not crash; the caller handles 0 copies
    CHECK_NEAR(x.opacity, 1.0, 1e-12);
}

TEST(drawcopy_matches_compute)
{
    rnd::ScatterParams p = Zero();
    p.seed = 77; p.pos[0] = 120; p.pos[1] = 80; p.pos[2] = 400; p.rotDeg[0] = 30; p.rotDeg[1] = 60; p.rotDeg[2] = 90;
    p.scaleMin = 40; p.scaleMax = 180; p.opacityMin = 25; p.opacityMax = 95;
    for (int i = 0; i < 10; i++) {
        rnd::CopyParams c = rnd::DrawCopy(p, i);
        double m[16];
        rnd::detail::MakeM4(c.t[0], c.t[1], c.t[2], c.r[0], c.r[1], c.r[2], c.s / 100.0, 12, 34, m);
        rnd::CopyXform x;
        rnd::ComputeCopy(p, i, 12, 34, &x);
        for (int k = 0; k < 16; k++) CHECK(m[k] == x.m4[k]);
        CHECK(c.o / 100.0 == x.opacity);
    }
    p.keepFirst = true;
    rnd::CopyParams c0 = rnd::DrawCopy(p, 0);
    CHECK(c0.t[0] == 0 && c0.t[1] == 0 && c0.t[2] == 0 && c0.r[0] == 0 && c0.r[1] == 0 && c0.r[2] == 0);
    CHECK(c0.s == 100 && c0.o == 100);
}

// ---- world transform (plan 2026-11-25 Task 2)
static void CheckM4Near(const double a[16], const double b[16], double eps)
{
    for (int i = 0; i < 16; i++) CHECK_NEAR(a[i], b[i], eps);
}
// A layer-to-world like AE builds it: T(-A) S Rz Ry Rx O(rz,ry,rx) T(P) [* parent]
static void LayerXform(double ax, double ay, const double s[3], const double r[3], const double o[3], const double p[3],
                       double out[16])
{
    rnd::WorldXform w;
    for (int k = 0; k < 3; k++) w.scale[k] = s[k], w.rotDeg[k] = r[k];
    double m[16];
    rnd::WorldM4(w, ax, ay, m);  // T(-A) S R T(A)
    rnd::WorldXform wo;
    for (int k = 0; k < 3; k++) wo.rotDeg[k] = o[k];
    double mo[16];
    rnd::WorldM4(wo, 0, 0, mo);  // orientation about the origin
    // m = T(-A) S R T(A); want T(-A) S R O T(P): m * T(-A) * O * T(P)
    double tA[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -ax, -ay, 0, 1};
    double tP[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, p[0], p[1], p[2], 1};
    double a[16], b[16];
    Mul(m, tA, a);
    Mul(a, mo, b);
    Mul(b, tP, out);
}

TEST(world_default_identity)
{
    rnd::WorldXform w;
    double m[16];
    rnd::WorldM4(w, 288.5, 340, m);
    const double I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    CheckM4Near(m, I, 1e-12);
}

TEST(world_roundtrip)
{
    const double rots[2][3] = {{25, -40, 70}, {10, 90, -30}};  // the second is gimbal lock
    for (auto& r : rots) {
        rnd::WorldXform w;
        w.pos[0] = 12, w.pos[1] = -30, w.pos[2] = 400;
        w.scale[0] = 80, w.scale[1] = 120, w.scale[2] = 150;
        for (int k = 0; k < 3; k++) w.rotDeg[k] = r[k];
        double m[16], m2[16];
        rnd::WorldM4(w, 288.5, 340, m);
        rnd::WorldXform d;
        CHECK(rnd::DecomposeWorld(m, 288.5, 340, &d));
        rnd::WorldM4(d, 288.5, 340, m2);
        CheckM4Near(m, m2, 1e-9);
    }
}

TEST(world_anchor_fixed)
{
    rnd::WorldXform w;
    w.rotDeg[0] = 20, w.rotDeg[1] = 35, w.rotDeg[2] = -15;
    w.scale[0] = w.scale[1] = w.scale[2] = 120;
    double m[16], o[3];
    rnd::WorldM4(w, 100, 50, m);
    Apply(m, 100, 50, 0, o);
    CHECK_NEAR(o[0], 100, 1e-9);
    CHECK_NEAR(o[1], 50, 1e-9);
    CHECK_NEAR(o[2], 0, 1e-9);
}

TEST(world_matches_copy_convention)
{
    // same rotation order and signs as the copies (detail::MakeM4, checked against AE by run_ref)
    rnd::WorldXform w;
    w.pos[0] = 5, w.pos[1] = -7, w.pos[2] = 30;
    w.rotDeg[0] = 20, w.rotDeg[1] = 35, w.rotDeg[2] = -15;
    w.scale[0] = w.scale[1] = w.scale[2] = 120;
    double m[16], c[16];
    rnd::WorldM4(w, 100, 50, m);
    rnd::detail::MakeM4(5, -7, 30, 20, 35, -15, 1.2, 100, 50, c);
    CheckM4Near(m, c, 1e-12);
}

TEST(transfer_keeps_world_placement)
{
  // exact cases only: a world rotation followed by a non-uniform layer scale is a shear (see the next test)
  for (int scenario = 0; scenario < 2; scenario++) {
    const double ax = 288.5, ay = 340, cw = 1920, ch = 1080;
    const double s0[3] = {90, 110, 100}, s1[3] = {110, 110, 110};
    const double* s = scenario == 0 ? s0 : s1;
    const double r[3] = {15, -25, 40}, o[3] = {10, 20, 30}, p[3] = {700, 300, -200};
    double layer[16], parent[16], l2w[16];
    LayerXform(ax, ay, s, r, o, p, layer);
    const double ps[3] = {100, 100, 100}, pr[3] = {0, 26, 0}, po[3] = {0, 0, 0}, pp[3] = {960, 540, 0};
    LayerXform(0, 0, ps, pr, po, pp, parent);
    Mul(layer, parent, l2w);
    rnd::WorldXform cur;
    cur.pos[0] = 30, cur.pos[1] = 0, cur.pos[2] = -50;
    cur.rotDeg[2] = scenario == 0 ? 0 : 12;
    cur.scale[0] = cur.scale[1] = cur.scale[2] = 80;
    rnd::WorldXform out;
    CHECK(rnd::TransferToWorld(cur, l2w, ax, ay, cw, ch, &out));
    double gOld[16], gNew[16], V[16], before[16], after[16];
    rnd::WorldM4(cur, ax, ay, gOld);
    rnd::WorldM4(out, ax, ay, gNew);
    rnd::InitialLayerToWorld(ax, ay, cw, ch, V);
    Mul(gOld, l2w, before);
    Mul(gNew, V, after);
    const double pts[3][2] = {{0, 0}, {577, 680}, {100, 50}};
    for (auto& q : pts) {
        double a[3], b[3];
        Apply(before, q[0], q[1], 0, a);
        Apply(after, q[0], q[1], 0, b);
        for (int k = 0; k < 3; k++) CHECK_NEAR(a[k], b[k], 1e-6);
    }
  }
}

TEST(transfer_initial_layer_is_identity)
{
    // a layer already in the initial state: the world transform is unchanged
    const double ax = 40, ay = 30, cw = 400, ch = 300;
    double V[16];
    rnd::InitialLayerToWorld(ax, ay, cw, ch, V);
    double o[3];
    Apply(V, ax, ay, 0, o);
    CHECK_NEAR(o[0], 200, 1e-12);
    CHECK_NEAR(o[1], 150, 1e-12);
    CHECK_NEAR(o[2], 0, 1e-12);
    rnd::WorldXform cur, out;
    cur.rotDeg[1] = 33;
    CHECK(rnd::TransferToWorld(cur, V, ax, ay, cw, ch, &out));
    for (int k = 0; k < 3; k++) {
        CHECK_NEAR(out.pos[k], cur.pos[k], 1e-9);
        CHECK_NEAR(out.rotDeg[k], cur.rotDeg[k], 1e-9);
        CHECK_NEAR(out.scale[k], cur.scale[k], 1e-9);
    }
}

TEST(transfer_shear_is_approximate)
{
    const double ax = 40, ay = 30;
    const double s1[3] = {100, 100, 100}, r1[3] = {0, 0, 30}, z[3] = {0, 0, 0}, p1[3] = {10, 20, 0};
    const double s2[3] = {200, 100, 100}, p2[3] = {200, 150, 0};
    double child[16], parent[16], l2w[16];
    LayerXform(ax, ay, s1, r1, z, p1, child);
    LayerXform(0, 0, s2, z, z, p2, parent);
    Mul(child, parent, l2w);
    rnd::WorldXform cur, out;
    CHECK(rnd::TransferToWorld(cur, l2w, ax, ay, 400, 300, &out));
}

// ---- fast mode: plane and per-copy projections
static void LookAtCamera(double px, double py, double pz, double tx, double ty, double tz, double camToWorld[16])
{
    double f[3] = {tx - px, ty - py, tz - pz};
    double fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (double& v : f) v /= fl;
    const double d0[3] = {0, 1, 0};  // AE: y down, camera looks along +z
    double r[3] = {d0[1] * f[2] - d0[2] * f[1], d0[2] * f[0] - d0[0] * f[2], d0[0] * f[1] - d0[1] * f[0]};
    double rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (double& v : r) v /= rl;
    double d[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    const double m[16] = {r[0], r[1], r[2], 0, d[0], d[1], d[2], 0, f[0], f[1], f[2], 0, px, py, pz, 1};
    std::memcpy(camToWorld, m, sizeof(m));
}
static rnd::CameraView CamMoved()
{
    rnd::CameraView c;
    LookAtCamera(700, 300, -1500, 1100, 600, 200, c.camToWorld);
    c.dist = 2666.6667;
    c.compW = 1920;
    c.compH = 1080;
    return c;
}
static rnd::CameraView CamDefault()
{
    rnd::CameraView c;
    LookAtCamera(960, 540, -2666.6667, 960, 540, 0, c.camToWorld);
    c.dist = 2666.6667;
    c.compW = 1920;
    c.compH = 1080;
    return c;
}
// A 577x680 layer, anchor at its centre, rotated, scaled 80%, at (1000, 500, 300).
static void LayerXform(double W[16])
{
    rnd::detail::MakeM4(1000 - 288.5, 500 - 340, 300, 20, -35, 15, 0.8, 288.5, 340, W);
}
// layer px (x, y, z) -> camera space
static void ToCam(const rnd::CameraView& cam, const double chain[16], double x, double y, double z, double c[3])
{
    double w[3], Cinv[16];
    Apply(chain, x, y, z, w);
    rnd::Invert4(cam.camToWorld, Cinv);
    Apply(Cinv, w[0], w[1], w[2], c);
}
static void ProjectCam(const rnd::CameraView& cam, const double c[3], double* sx, double* sy)
{
    *sx = c[0] * cam.dist / c[2] + cam.compW / 2;
    *sy = c[1] * cam.dist / c[2] + cam.compH / 2;
}
static void ApplyH(const double h[9], double x, double y, double* u, double* v)
{
    double a = x * h[0] + y * h[3] + h[6], b = x * h[1] + y * h[4] + h[7], w = x * h[2] + y * h[5] + h[8];
    *u = a / w;
    *v = b / w;
}

TEST(invert4_roundtrip)
{
    double W[16], Wi[16], I[16];
    LayerXform(W);
    CHECK(rnd::Invert4(W, Wi));
    rnd::Mul4(W, Wi, I);
    for (int k = 0; k < 16; k++) CHECK_NEAR(I[k], (k % 5 == 0) ? 1.0 : 0.0, 1e-9);
}

TEST(fast_plane_maps_pixels_1to1)
{
    rnd::CameraView cams[2] = {CamMoved(), CamDefault()};
    double W[16];
    LayerXform(W);
    for (const auto& cam : cams) {
        double M[16], MW[16];
        CHECK(rnd::PlaneM4(cam, W, 1800.0, M));
        rnd::Mul4(M, W, MW);
        const double pts[4][2] = {{0, 0}, {1919, 0}, {123.5, 777.25}, {1919, 1079}};
        for (const auto& p : pts) {
            double c[3], sx, sy;
            ToCam(cam, MW, p[0], p[1], 0, c);
            ProjectCam(cam, c, &sx, &sy);
            CHECK_NEAR(sx, p[0], 1e-6);
            CHECK_NEAR(sy, p[1], 1e-6);
        }
    }
}

TEST(fast_plane_depth_is_anchor)
{
    rnd::CameraView cams[2] = {CamMoved(), CamDefault()};
    double W[16];
    LayerXform(W);
    for (const auto& cam : cams) {
        double d = rnd::AnchorDepth(cam, W, 288.5, 340);
        double a[3];
        ToCam(cam, W, 288.5, 340, 0, a);
        CHECK_NEAR(d, a[2], 1e-9);
        double M[16], MW[16];
        CHECK(rnd::PlaneM4(cam, W, d, M));
        rnd::Mul4(M, W, MW);
        const double pts[3][2] = {{0, 0}, {960, 540}, {1500, 100}};
        for (const auto& p : pts) {
            double c[3];
            ToCam(cam, MW, p[0], p[1], 0, c);
            CHECK_NEAR(c[2], d, 1e-6);
        }
    }
}

static const double kSrcRect[4] = {0, 0, 577, 680};  // left, top, right, bottom (layer px)

TEST(copy_homography_plane_is_identity)
{
    // A copy placed exactly on the screen plane maps screen px to the same layer px.
    rnd::CameraView cam = CamMoved();
    double W[16], M[16], H[9];
    LayerXform(W);
    CHECK(rnd::PlaneM4(cam, W, 1800.0, M));
    const double big[4] = {0, 0, 1920, 1080};
    CHECK(rnd::CopyHomography(cam, W, M, big, H));
    const double pts[3][2] = {{0, 0}, {800.5, 300.25}, {1919, 1079}};
    for (const auto& p : pts) {
        double u, v;
        ApplyH(H, p[0], p[1], &u, &v);
        CHECK_NEAR(u, p[0], 1e-6);
        CHECK_NEAR(v, p[1], 1e-6);
    }
}

TEST(copy_homography_inverts_projection)
{
    rnd::CameraView cams[2] = {CamMoved(), CamDefault()};
    double W[16], C[16], CW[16];
    LayerXform(W);
    rnd::detail::MakeM4(40, -70, 250, 10, 30, -50, 1.3, 288.5, 340, C);
    rnd::Mul4(C, W, CW);
    for (const auto& cam : cams) {
        double H[9];
        CHECK(rnd::CopyHomography(cam, W, C, kSrcRect, H));
        const double pts[4][2] = {{0, 0}, {576, 0}, {100.5, 600.25}, {576, 679}};
        for (const auto& p : pts) {
            double c[3], sx, sy, u, v;
            ToCam(cam, CW, p[0], p[1], 0, c);
            ProjectCam(cam, c, &sx, &sy);
            ApplyH(H, sx, sy, &u, &v);
            CHECK_NEAR(u, p[0], 1e-6);
            CHECK_NEAR(v, p[1], 1e-6);
        }
    }
}

TEST(copy_homography_behind_camera)
{
    // Default camera at z = -2666.67, a 577x680 layer facing it, turned 80 deg about Y around its anchor:
    // its left/right edges are about +-284 px apart in depth.
    rnd::CameraView cam = CamDefault();
    double W[16], H[9];
    rnd::detail::MakeM4(960 - 288.5, 540 - 340, 0, 0, 0, 0, 1, 288.5, 340, W);
    double near[16], far[16];
    rnd::detail::MakeM4(0, 0, -2666.6667 + 200, 0, 80, 0, 1, 288.5, 340, near);  // anchor 200 px ahead
    rnd::detail::MakeM4(0, 0, -2666.6667 + 400, 0, 80, 0, 1, 288.5, 340, far);   // anchor 400 px ahead
    CHECK(!rnd::CopyHomography(cam, W, near, kSrcRect, H));
    CHECK(rnd::CopyHomography(cam, W, far, kSrcRect, H));
}

TEST(copy_depth_is_center_z)
{
    rnd::CameraView cam = CamMoved();
    double W[16], C[16], CW[16], c[3];
    LayerXform(W);
    rnd::detail::MakeM4(40, -70, 250, 10, 30, -50, 1.3, 288.5, 340, C);
    rnd::Mul4(C, W, CW);
    ToCam(cam, CW, 288.5, 340, 0, c);
    CHECK_NEAR(rnd::CopyDepth(cam, W, C, 288.5, 340), c[2], 1e-9);
}

// ---- fast mode: CPU rasterizer
struct Px8 { unsigned char a, r, g, b; };
struct Px16 { unsigned short a, r, g, b; };
struct PxF { float a, r, g, b; };

// A premultiplied 8 bpc test image with distinct values (alpha >= colour channels).
static std::vector<Px8> MakeSrc8(int w, int h)
{
    std::vector<Px8> v((size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            Px8& p = v[(size_t)y * w + x];
            p.a = (unsigned char)(150 + (x * 7 + y * 3) % 106);
            p.r = (unsigned char)((x * 37 + y * 11) % (p.a + 1));
            p.g = (unsigned char)((x * 5 + y * 29) % (p.a + 1));
            p.b = (unsigned char)((x * 13 + y * 17) % (p.a + 1));
        }
    return v;
}
template <class P>
static rnd::RasterImage Img(std::vector<P>& v, int w, int h, int bpc, int ox = 0, int oy = 0)
{
    rnd::RasterImage r;
    r.data = v.data();
    r.width = w;
    r.height = h;
    r.rowBytes = (long)(w * sizeof(P));
    r.bpc = bpc;
    r.originX = ox;
    r.originY = oy;
    return r;
}
static rnd::RasterCopy Copy(double opacity, double depth, double sx = 1, double tx = 0, double ty = 0)
{
    // screen -> src: u = sx * x + tx, v = sx * y + ty
    rnd::RasterCopy c = {{sx, 0, 0, 0, sx, 0, tx, ty, 1}, opacity, depth};
    return c;
}
static bool SamePx(const Px8& p, const Px8& q) { return p.a == q.a && p.r == q.r && p.g == q.g && p.b == q.b; }

TEST(raster_identity_copies_source)
{
    const int w = 9, h = 7;
    auto src = MakeSrc8(w, h);
    std::vector<Px8> dst((size_t)w * h, Px8{9, 9, 9, 9});
    rnd::RasterImage s = Img(src, w, h, 8), d = Img(dst, w, h, 8);
    rnd::RasterCopy c = Copy(1.0, 100);
    rnd::RasterizeCopies(s, &c, 1, 1.0, 1.0, d, 1);
    for (size_t i = 0; i < dst.size(); i++) CHECK(SamePx(dst[i], src[i]));
}

TEST(raster_origins)
{
    // src buffer starts at layer px (-2, -1); dst buffer starts at screen px (5, 4).
    const int w = 12, h = 10;
    auto src = MakeSrc8(w, h);
    std::vector<Px8> dst(4 * 3);
    rnd::RasterImage s = Img(src, w, h, 8, -2, -1), d = Img(dst, 4, 3, 8, 5, 4);
    rnd::RasterCopy c = Copy(1.0, 100);
    rnd::RasterizeCopies(s, &c, 1, 1.0, 1.0, d, 1);
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 4; x++) CHECK(SamePx(dst[(size_t)y * 4 + x], src[(size_t)(y + 5) * w + (x + 7)]));
}

TEST(raster_screen_shift)
{
    // fast mode on a 2D layer: dst buffer px (i, j) is layer px, screen px = layer px + shift
    const int w = 30, h = 20;
    auto src = MakeSrc8(w, h);
    std::vector<Px8> dst(8 * 6);
    rnd::RasterImage s = Img(src, w, h, 8), d = Img(dst, 8, 6, 8);
    d.screenShiftX = 10;
    d.screenShiftY = 5;
    rnd::RasterCopy c = Copy(1.0, 100);
    rnd::RasterizeCopies(s, &c, 1, 1.0, 1.0, d, 1);
    for (int y = 0; y < 6; y++)
        for (int x = 0; x < 8; x++) CHECK(SamePx(dst[(size_t)y * 8 + x], src[(size_t)(y + 5) * w + (x + 10)]));
}

TEST(raster_opacity_half)
{
    const int w = 6, h = 5;
    auto src = MakeSrc8(w, h);
    std::vector<Px8> dst((size_t)w * h);
    rnd::RasterImage s = Img(src, w, h, 8), d = Img(dst, w, h, 8);
    rnd::RasterCopy c = Copy(0.5, 100);
    rnd::RasterizeCopies(s, &c, 1, 1.0, 1.0, d, 1);
    // AE worlds are straight alpha: half opacity halves alpha, the colour stays
    for (size_t i = 0; i < dst.size(); i++) {
        CHECK_NEAR(dst[i].a, src[i].a / 2.0, 1.0);
        CHECK_NEAR(dst[i].r, src[i].r, 1.0);
        CHECK_NEAR(dst[i].g, src[i].g, 1.0);
        CHECK_NEAR(dst[i].b, src[i].b, 1.0);
    }
}

TEST(raster_straight_alpha_edge)
{
    // A transparent pixel keeps a bright colour (straight alpha): it must not leak into the result.
    std::vector<Px8> src = {Px8{0, 255, 255, 255}, Px8{255, 0, 0, 0}};
    std::vector<Px8> dst(2);
    rnd::RasterImage s = Img(src, 2, 1, 8), d = Img(dst, 2, 1, 8);
    rnd::RasterCopy c = Copy(1.0, 100, 1, 0.5, 0);  // samples halfway between the two pixels
    rnd::RasterizeCopies(s, &c, 1, 1.0, 1.0, d, 1);
    CHECK_NEAR(dst[0].a, 127.5, 1.0);
    CHECK(dst[0].r == 0 && dst[0].g == 0 && dst[0].b == 0);
    // over a straight-alpha background: 50% white over opaque black = mid grey, opaque
    std::vector<Px8> w = {Px8{128, 255, 255, 255}}, out(1);
    rnd::RasterImage sw = Img(w, 1, 1, 8), dw = Img(out, 1, 1, 8);
    rnd::RasterCopy two[2] = {Copy(1.0, 100), Copy(1.0, 200)};
    std::vector<Px8> blk = {Px8{255, 0, 0, 0}};
    (void)blk;
    rnd::RasterizeCopies(sw, two, 2, 1.0, 1.0, dw, 1);  // the same 50% white twice: alpha 1-(0.5)^2
    CHECK_NEAR(out[0].a, 255 * (1 - (1 - 128 / 255.0) * (1 - 128 / 255.0)), 1.0);
    CHECK(out[0].r == 255 && out[0].g == 255 && out[0].b == 255);
}

TEST(raster_back_to_front)
{
    // Opaque source; the near copy (depth 100) is listed first, the far one (depth 200) is shifted 1 px.
    const int w = 8, h = 6;
    auto src = MakeSrc8(w, h);
    for (auto& p : src) p.a = 255;
    std::vector<Px8> dst((size_t)w * h);
    rnd::RasterImage s = Img(src, w, h, 8), d = Img(dst, w, h, 8);
    rnd::RasterCopy c[2] = {Copy(1.0, 100), Copy(1.0, 200, 1, 1, 0)};
    rnd::RasterizeCopies(s, c, 2, 1.0, 1.0, d, 1);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w - 1; x++) CHECK(SamePx(dst[(size_t)y * w + x], src[(size_t)y * w + x]));
}

TEST(raster_downsample_half)
{
    // At 1/2 resolution AE hands both the input and the output at half size.
    const int w = 8, h = 6;
    auto src = MakeSrc8(w, h);
    std::vector<Px8> dst((size_t)w * h);
    rnd::RasterImage s = Img(src, w, h, 8), d = Img(dst, w, h, 8);
    rnd::RasterCopy c = Copy(1.0, 100);
    rnd::RasterizeCopies(s, &c, 1, 0.5, 0.5, d, 1);
    for (size_t i = 0; i < dst.size(); i++) CHECK(SamePx(dst[i], src[i]));
    // A copy at half size (src = 2 * screen): dst (x, y) samples src index (2x + 0.5, 2y + 0.5).
    std::vector<Px8> dst2((size_t)(w / 2) * (h / 2));
    rnd::RasterImage d2 = Img(dst2, w / 2, h / 2, 8);
    rnd::RasterCopy c2 = Copy(1.0, 100, 2.0);
    rnd::RasterizeCopies(s, &c2, 1, 0.5, 0.5, d2, 1);
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w / 2; x++) {
            const Px8* q[4] = {&src[(size_t)(2 * y) * w + 2 * x], &src[(size_t)(2 * y) * w + 2 * x + 1],
                               &src[(size_t)(2 * y + 1) * w + 2 * x], &src[(size_t)(2 * y + 1) * w + 2 * x + 1]};
            const Px8& o = dst2[(size_t)y * (w / 2) + x];
            double sa = q[0]->a + q[1]->a + q[2]->a + q[3]->a;
            double sr = q[0]->r * q[0]->a + q[1]->r * q[1]->a + q[2]->r * q[2]->a + q[3]->r * q[3]->a;
            CHECK_NEAR(o.a, sa / 4.0, 1.0);
            CHECK_NEAR(o.r, sr / sa, 1.0);  // straight alpha: colours are averaged weighted by alpha
        }
}

static void RandomCopies(rnd::RasterCopy* c, int n)
{
    for (int i = 0; i < n; i++) {
        double a = 0.3 * i, s = 0.8 + 0.05 * i;
        rnd::RasterCopy k = {{s * std::cos(a), s * std::sin(a), 0.0002 * i, -s * std::sin(a), s * std::cos(a), 0.0001,
                              -40.0 * i, 25.0 * i, 1},
                             0.4 + 0.05 * i,
                             100.0 + 37 * ((i * 7) % 5)};
        c[i] = k;
    }
}

TEST(raster_threads_same_result)
{
    const int w = 60, h = 50;
    auto src = MakeSrc8(w, h);
    std::vector<Px8> d1(200 * 150), d8(200 * 150);
    rnd::RasterImage s = Img(src, w, h, 8), a = Img(d1, 200, 150, 8, -20, -10), b = Img(d8, 200, 150, 8, -20, -10);
    rnd::RasterCopy c[12];
    RandomCopies(c, 12);
    rnd::RasterizeCopies(s, c, 12, 1.0, 1.0, a, 1);
    RandomCopies(c, 12);
    rnd::RasterizeCopies(s, c, 12, 1.0, 1.0, b, 8);
    CHECK(std::memcmp(d1.data(), d8.data(), d1.size() * sizeof(Px8)) == 0);
    bool any = false;
    for (auto& p : d1) any |= p.a != 0;
    CHECK(any);
}

TEST(raster_behind_camera)
{
    // Copies behind the camera are dropped by the caller (CopyHomography false): with none left, all clear.
    std::vector<Px8> src = MakeSrc8(4, 4), dst(16, Px8{7, 7, 7, 7});
    rnd::RasterImage s = Img(src, 4, 4, 8), d = Img(dst, 4, 4, 8);
    rnd::RasterizeCopies(s, nullptr, 0, 1.0, 1.0, d, 4);
    for (auto& p : dst) CHECK(p.a == 0 && p.r == 0 && p.g == 0 && p.b == 0);
}

TEST(raster_per_pixel_depth)
{
    // The user's report: parallel copies seen at an angle (camera orbiting). Where two copies overlap, the one
    // nearer at THAT pixel must be in front, whatever their centres' depths say. Real geometry: a 100x100
    // layer turned 70 deg about Y; copy B is shifted along the layer's x and towards the camera along its normal.
    rnd::CameraView cam = CamDefault();
    double W[16], A[16], B[16];
    rnd::detail::MakeM4(960 - 50, 540 - 50, 0, 0, 70, 0, 1, 50, 50, W);
    rnd::detail::MakeM4(0, 0, 0, 0, 0, 0, 1, 50, 50, A);
    rnd::detail::MakeM4(60, 0, -30, 0, 0, 0, 1, 50, 50, B);
    const double rect[4] = {0, 0, 100, 100};
    rnd::RasterCopy c[2];
    CHECK(rnd::CopyHomography(cam, W, A, rect, c[0].screenToSrc));
    CHECK(rnd::CopyHomography(cam, W, B, rect, c[1].screenToSrc));
    c[0].opacity = c[1].opacity = 1.0;
    // a linear ramp: red = 2 * (u - 0.5) at source x u, opaque (bilinear reproduces it exactly)
    std::vector<Px8> src(100 * 100);
    for (int y = 0; y < 100; y++)
        for (int x = 0; x < 100; x++) src[(size_t)y * 100 + x] = Px8{255, (unsigned char)(2 * x), 0, 0};
    const int ox = 860, oy = 440, w = 200, h = 200;
    // Give each copy the "centre depth" that puts the WRONG one in front, in both orders.
    for (int flip = 0; flip < 2; flip++) {
        std::vector<Px8> dst((size_t)w * h);
        rnd::RasterImage s = Img(src, 100, 100, 8), d = Img(dst, w, h, 8, ox, oy);
        rnd::RasterCopy k[2] = {c[0], c[1]};
        k[0].depth = flip ? 1.0 : 2.0;
        k[1].depth = flip ? 2.0 : 1.0;
        rnd::RasterizeCopies(s, k, 2, 1.0, 1.0, d, 2);
        int checked = 0, wrong = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                double sx = x + ox + 0.5, sy = y + oy + 0.5, u[2], v[2], iz[2];
                for (int i = 0; i < 2; i++) {
                    const double* H = c[i].screenToSrc;
                    iz[i] = sx * H[2] + sy * H[5] + H[8];  // = 1 / camera z
                    ApplyH(H, sx, sy, &u[i], &v[i]);
                }
                bool inside = true;
                for (int i = 0; i < 2; i++) inside = inside && u[i] > 2 && u[i] < 98 && v[i] > 2 && v[i] < 98;
                if (!inside || std::fabs(u[0] - u[1]) < 5) continue;
                int nearer = iz[0] > iz[1] ? 0 : 1;
                ++checked;
                if (std::fabs(dst[(size_t)y * w + x].r - 2 * (u[nearer] - 0.5)) > 1.5) ++wrong;
            }
        CHECK(checked > 100);
        CHECK(wrong == 0);
        if (wrong) std::printf("  per-pixel depth: flip=%d %d of %d overlap px show the farther copy\n", flip, wrong, checked);
    }
}

TEST(raster_abort_stops)
{
    // AE's abort (the user scrubs or cancels): the drawing stops and reports it; without abort it completes.
    std::vector<Px8> src = MakeSrc8(40, 40), dst((size_t)200 * 200);
    rnd::RasterImage s = Img(src, 40, 40, 8), d = Img(dst, 200, 200, 8);
    rnd::RasterCopy c = Copy(1.0, 100, 0.2);
    int asked = 0;
    auto stop = [&]() { ++asked; return true; };
    CHECK(!rnd::RasterizeCopies(s, &c, 1, 1.0, 1.0, d, 4, stop));
    CHECK(asked >= 1);
    auto never = []() { return false; };
    CHECK(rnd::RasterizeCopies(s, &c, 1, 1.0, 1.0, d, 4, never));
    CHECK(rnd::RasterizeCopies(s, &c, 1, 1.0, 1.0, d, 4));
}

TEST(raster_identity_16bpc)
{
    const int w = 7, h = 5;
    auto s8 = MakeSrc8(w, h);
    std::vector<Px16> src((size_t)w * h), dst((size_t)w * h);
    for (size_t i = 0; i < src.size(); i++)
        src[i] = Px16{(unsigned short)(s8[i].a * 128), (unsigned short)(s8[i].r * 128), (unsigned short)(s8[i].g * 128),
                      (unsigned short)(s8[i].b * 128)};
    src[0].a = 32768;
    rnd::RasterImage s = Img(src, w, h, 16), d = Img(dst, w, h, 16);
    rnd::RasterCopy c = Copy(1.0, 100);
    rnd::RasterizeCopies(s, &c, 1, 1.0, 1.0, d, 2);
    for (size_t i = 0; i < src.size(); i++)
        CHECK(dst[i].a == src[i].a && dst[i].r == src[i].r && dst[i].g == src[i].g && dst[i].b == src[i].b);
}

TEST(raster_identity_32bpc)
{
    const int w = 7, h = 5;
    auto s8 = MakeSrc8(w, h);
    std::vector<PxF> src((size_t)w * h), dst((size_t)w * h);
    for (size_t i = 0; i < src.size(); i++)
        src[i] = PxF{s8[i].a / 255.f, s8[i].r / 255.f * 1.5f, s8[i].g / 255.f, s8[i].b / 255.f};  // overbright ok
    rnd::RasterImage s = Img(src, w, h, 32), d = Img(dst, w, h, 32);
    rnd::RasterCopy c = Copy(1.0, 100);
    rnd::RasterizeCopies(s, &c, 1, 1.0, 1.0, d, 2);
    for (size_t i = 0; i < src.size(); i++) {
        CHECK_NEAR(dst[i].a, src[i].a, 1e-6);
        CHECK_NEAR(dst[i].r, src[i].r, 1e-6);
        CHECK_NEAR(dst[i].g, src[i].g, 1e-6);
        CHECK_NEAR(dst[i].b, src[i].b, 1e-6);
    }
}

TEST(raster_speed_report)
{
    std::vector<Px8> src = MakeSrc8(577, 680), dst((size_t)1920 * 1080);
    rnd::RasterImage s = Img(src, 577, 680, 8), d = Img(dst, 1920, 1080, 8);
    rnd::RasterCopy c[10];
    for (int i = 0; i < 10; i++) c[i] = Copy(0.9, 100.0 + i, 1.0 + 0.03 * i, -130.0 * i, -40.0 * i);
    const int counts[2] = {1, (int)std::thread::hardware_concurrency()};
    for (int threads : counts) {
        double best = 1e9;
        for (int rep = 0; rep < 3; rep++) {
            auto t0 = std::chrono::steady_clock::now();
            rnd::RasterizeCopies(s, c, 10, 1.0, 1.0, d, threads);
            best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        std::printf("raster speed: 1920x1080, 10 copies of 577x680, %d threads: %.2f ms (best of 3)\n", threads, best);
        CHECK(best >= 0);
    }
    auto t0 = std::chrono::steady_clock::now();
    rnd::RasterizeCopies(s, c, 0, 1.0, 1.0, d, counts[1]);
    std::printf("raster speed: 1920x1080, 0 copies (clear only), %d threads: %.2f ms\n", counts[1],
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

// ---- hook tests
// The AE 2025 code shape after `call [rax+IsLayerType]` in the flatten check (see flatten_site.cpp).
static void MakeFlattenSite(unsigned char* b, uintptr_t* betaFn)
{
    std::memset(b, 0x90, 0x200);
    const int rel = 0x100;
    const unsigned char head[] = {0x84, 0xC0, 0x75, 0x10, 0xE8};
    std::memcpy(b, head, sizeof(head));
    std::memcpy(b + 5, &rel, 4);
    *betaFn = (uintptr_t)(b + 9 + rel);
    const unsigned char mid[] = {0x84, 0xC0, 0x74, 0x20, 0x48, 0x8B, 0x03, 0x48, 0x8B, 0xCB, 0xFF, 0x90};
    std::memcpy(b + 9, mid, sizeof(mid));
    const unsigned char tail1[] = {0x48, 0x8B, 0x10, 0x48, 0x8B, 0xC8, 0xFF, 0x92};
    std::memcpy(b + 25, tail1, sizeof(tail1));
    const unsigned char tail2[] = {0x85, 0xC0, 0x75, 0x30, 0x48, 0x8B};
    std::memcpy(b + 37, tail2, sizeof(tail2));
    b[47] = 0xFF; b[48] = 0x90;
    const unsigned char tail3[] = {0x84, 0xC0, 0x75, 0x40, 0x48, 0x8B, 0xCB, 0xE8};
    std::memcpy(b + 53, tail3, sizeof(tail3));
}

TEST(flatten_site_match)
{
    static unsigned char b[0x200];
    uintptr_t beta = 0;
    MakeFlattenSite(b, &beta);
    CHECK(rnd::MatchFlattenCheckSite(b, beta));
    CHECK(!rnd::MatchFlattenCheckSite(b, beta + 1));  // call goes elsewhere
}

TEST(flatten_site_audio_rejected)
{
    static unsigned char b[0x200];
    uintptr_t beta = 0;
    MakeFlattenSite(b, &beta);
    b[25] = 0x33;  // same prefix, different code after the effects-count call (audio-synth check)
    CHECK(!rnd::MatchFlattenCheckSite(b, beta));
    MakeFlattenSite(b, &beta);
    b[60] = 0x90;  // no call to BEE_GetSelectedTrackMatte
    CHECK(!rnd::MatchFlattenCheckSite(b, beta));
}

static rnd::ParamSnapshot Snap(int count)
{
    rnd::ParamSnapshot s;
    s.p.count = count;
    s.valid = true;
    return s;
}

TEST(registry_two_comps_same_layer_id)
{
    rnd::RegRegister({10, 48}, Snap(5));
    rnd::ParamSnapshot s;
    CHECK(!rnd::RegLookup({11, 48}, &s));
    CHECK(rnd::RegLookup({10, 48}, &s));
    CHECK(s.p.count == 5);
    rnd::RegUnregister({10, 48});
}

TEST(registry_unregister)
{
    rnd::RegRegister({20, 7}, Snap(3));
    CHECK(rnd::RegContains({20, 7}));
    rnd::RegUnregister({20, 7});
    rnd::ParamSnapshot s;
    CHECK(!rnd::RegLookup({20, 7}, &s));
    CHECK(!rnd::RegContains({20, 7}));
    rnd::RegRegister({20, 7}, Snap(4));  // re-register after unregister
    CHECK(rnd::RegLookup({20, 7}, &s) && s.p.count == 4);
    rnd::RegUnregister({20, 7});
}

TEST(registry_update_snapshot)
{
    rnd::RegRegister({30, 1}, Snap(5));
    rnd::RegRegister({30, 1}, Snap(9));
    rnd::ParamSnapshot s;
    CHECK(rnd::RegLookup({30, 1}, &s) && s.p.count == 9);
    rnd::RegUnregister({30, 1});
}

TEST(vec_reserve_small_and_aligned)
{
    rnd::bee::RawVec v = {nullptr, nullptr, nullptr};
    CHECK(rnd::bee::VecReserve(&v, 128, 5));  // 640 bytes: plain malloc
    CHECK(v.first && v.last == v.first && v.end - v.first == 640);
    v.last = v.first + 256;
    CHECK(rnd::bee::VecReserve(&v, 128, 3));  // fits: reuse, emptied
    CHECK(v.last == v.first && v.end - v.first == 640);
    CHECK(rnd::bee::VecReserve(&v, 128, 100));  // 12800 bytes: 32-byte aligned, raw pointer before it
    CHECK(((uintptr_t)v.first & 31) == 0);
    CHECK(((void**)v.first)[-1] != nullptr);
    CHECK((char*)v.first - (char*)((void**)v.first)[-1] >= 8);
    CHECK(v.last == v.first && v.end - v.first == 12800);
    CHECK(rnd::bee::VecReserve(&v, 128, 200));  // grow again, frees the aligned block correctly
    CHECK(v.end - v.first == 25600);
}

int main()
{
    for (int t = 0; t < g_numTests; t++) {
        g_test = g_tests[t].name;
        g_tests[t].fn();
    }
    if (g_failed == 0)
        std::printf("ALL PASS (%d tests, %d checks)\n", g_numTests, g_checks);
    else
        std::printf("%d FAILED of %d checks\n", g_failed, g_checks);
    return g_failed;
}
