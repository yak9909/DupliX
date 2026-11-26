// randamu unit tests (no framework). Exit code = number of failed checks.
#define NOMINMAX
#include <windows.h>
#undef near  // windows.h's old pointer keywords; the tests use these names
#undef far

#include <crtdbg.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <new>
#include <thread>
#include <string>
#include <vector>

#include <array>

#include "core/ae_pair.h"
#include "core/ae_span.h"
#include "core/fast_plane.h"
#include "core/merge_geom.h"
#include "core/merge_plan.h"
#include "effect/randamu_params.h"
#include "core/raster.h"
#include "core/raster_ref.h"
#include "core/tiles.h"
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

// ---- tiled rasterizer (plan 2026-11-25-fast-mode-gpu Task 2)
// Copies of a srcW x srcH layer scattered like the fast mode does (default camera, the virtual layer at the comp
// centre), sorted far to near. Copies behind the camera are dropped as the effect does.
static std::vector<rnd::RasterCopy> SceneCopies(int seed, int n, int srcW, int srcH, double posRange, double rotRange,
                                                double opMin)
{
    rnd::CameraView cam = CamDefault();
    rnd::ScatterParams p;
    p.count = n;
    p.seed = seed;
    p.pos[0] = p.pos[1] = posRange;
    p.pos[2] = posRange * 2;
    p.rotDeg[0] = p.rotDeg[1] = p.rotDeg[2] = rotRange;
    p.scaleMin = 60;
    p.scaleMax = 140;
    p.opacityMin = opMin;
    p.opacityMax = 100;
    double W[16];
    rnd::InitialLayerToWorld(srcW / 2.0, srcH / 2.0, 1920, 1080, W);
    const double rect[4] = {0, 0, (double)srcW, (double)srcH};
    std::vector<rnd::RasterCopy> v;
    for (int i = 0; i < n; i++) {
        rnd::CopyXform x;
        rnd::ComputeCopy(p, i, srcW / 2.0, srcH / 2.0, &x);
        rnd::RasterCopy c;
        if (!rnd::CopyHomography(cam, W, x.m4, rect, c.screenToSrc)) continue;
        c.opacity = x.opacity;
        c.depth = rnd::CopyDepth(cam, W, x.m4, srcW / 2.0, srcH / 2.0);
        v.push_back(c);
    }
    std::stable_sort(v.begin(), v.end(), [](const rnd::RasterCopy& a, const rnd::RasterCopy& b) { return a.depth > b.depth; });
    return v;
}

TEST(tiles_cover_boxes)
{
    std::vector<Px8> src = MakeSrc8(10, 10), dst((size_t)64 * 48);
    rnd::RasterImage s = Img(src, 10, 10, 8), d = Img(dst, 64, 48, 8);
    rnd::RasterCopy c[3] = {Copy(1, 3, 1, 0, 0),         // screen (0..10, 0..10): tile 0 only
                            Copy(1, 2, 1, -40, -20),     // screen (40..50, 20..30): tile (2, 1) and (3, 1)
                            Copy(1, 1, 1, 500, 500)};    // off screen: nowhere
    rnd::TileLists t;
    rnd::BuildTiles(s, c, 3, 1.0, 1.0, d, 16, &t);
    CHECK(t.tilesX == 4 && t.tilesY == 3);
    auto has = [&](int tx, int ty, int i) {
        int k = ty * t.tilesX + tx;
        for (int j = t.start[k]; j < t.start[k + 1]; j++)
            if (t.index[j] == i) return true;
        return false;
    };
    CHECK(has(0, 0, 0) && !has(1, 0, 0) && !has(0, 1, 0));
    CHECK(has(2, 1, 1) && has(3, 1, 1) && !has(1, 1, 1));
    int total = t.start[t.tilesX * t.tilesY];
    int with2 = 0;
    for (int k = 0; k < total; k++) with2 += t.index[k] == 2;
    CHECK(with2 == 0);
}

TEST(merge_block_reasons)
{
    rnd::LayerFlagsForMerge f;
    CHECK(rnd::MergeBlockReasons(f) == rnd::MB_NONE);
    auto one = [](void (*set)(rnd::LayerFlagsForMerge&)) {
        rnd::LayerFlagsForMerge g;
        set(g);
        return rnd::MergeBlockReasons(g);
    };
    CHECK(one([](rnd::LayerFlagsForMerge& g) { g.compMotionBlur = true; }) == rnd::MB_NONE);  // the layer's switch off
    CHECK(one([](rnd::LayerFlagsForMerge& g) { g.compMotionBlur = g.layerMotionBlur = true; }) == rnd::MB_MOTION_BLUR);
    CHECK(one([](rnd::LayerFlagsForMerge& g) { g.dof = true; }) == rnd::MB_DOF);
    CHECK(one([](rnd::LayerFlagsForMerge& g) { g.acceptsLights = true; }) == rnd::MB_NONE);  // no light
    CHECK(one([](rnd::LayerFlagsForMerge& g) { g.acceptsLights = g.anyLight = true; }) == rnd::MB_LIGHTS);
    CHECK(one([](rnd::LayerFlagsForMerge& g) { g.castsShadows = g.anyShadowLight = true; }) == rnd::MB_SHADOWS);
    CHECK(one([](rnd::LayerFlagsForMerge& g) { g.normalBlend = false; }) == rnd::MB_BLEND);
    CHECK(one([](rnd::LayerFlagsForMerge& g) { g.fullOpacity = false; }) == rnd::MB_OPACITY);
    CHECK(one([](rnd::LayerFlagsForMerge& g) { g.trackMatte = true; }) == rnd::MB_MATTE);
    CHECK(one([](rnd::LayerFlagsForMerge& g) { g.otherEffects = true; }) == rnd::MB_OTHER_EFFECTS);
    CHECK(one([](rnd::LayerFlagsForMerge& g) { g.solid = true; }) == rnd::MB_SOLID);
}

// AE's default camera of a comp without one: the comp's corners and centre on z = 0 land on the screen's.
TEST(merge_default_camera)
{
    rnd::CameraView cam;
    rnd::DefaultCamera(1920, 1080, 2666.67, &cam);
    double I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    CHECK(std::fabs(rnd::AnchorDepth(cam, I, 960, 540) - 2666.67) < 1e-6);
    // a plane at depth 2666.67 that shows layer px (x, y) at comp px (x, y) is the identity there
    double M[16];
    CHECK(rnd::PlaneM4(cam, I, 2666.67, M));
    for (int k = 0; k < 16; k++) CHECK(std::fabs(M[k] - I[k]) < 1e-6);
    CHECK(cam.compW == 1920 && cam.compH == 1080 && cam.dist == 2666.67);
}

// A camera looking at the comp from the front-left, slightly rotated (a two-node camera's look).
static rnd::CameraView MergeTestCamera()
{
    rnd::CameraView cam;
    double R[16], T[16];
    const double a = 12 * 3.14159265358979 / 180;  // yaw
    const double Ry[16] = {std::cos(a), 0, -std::sin(a), 0, 0, 1, 0, 0, std::sin(a), 0, std::cos(a), 0, 0, 0, 0, 1};
    std::memcpy(R, Ry, sizeof(R));
    const double Tt[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 120, 140, -700, 1};
    std::memcpy(T, Tt, sizeof(T));
    rnd::Mul4(R, T, cam.camToWorld);
    cam.dist = 800;
    cam.compW = 400;
    cam.compH = 300;
    return cam;
}
static rnd::MergeLayerInput MergeTestLayer(int count, unsigned block = 0)
{
    rnd::MergeLayerInput l;
    l.compItemId = 5;
    l.layerId = 7;
    l.p.count = count;
    l.p.seed = 3;
    l.p.pos[0] = 140, l.p.pos[1] = 90, l.p.pos[2] = 250;
    l.p.rotDeg[2] = 90;
    l.p.merge = true;
    l.anchorX = 60, l.anchorY = 50;
    rnd::InitialLayerToWorld(60, 50, 400, 300, l.layerToWorld);
    const double r[4] = {0, 0, 120, 100};
    std::memcpy(l.srcRect, r, sizeof(r));
    l.block = block;
    return l;
}
static void ProjectWorld(const rnd::CameraView& cam, const double p[3], double* sx, double* sy)
{
    double inv[16];
    rnd::Invert4(cam.camToWorld, inv);
    double c[3];
    for (int k = 0; k < 3; k++) c[k] = p[0] * inv[k] + p[1] * inv[4 + k] + p[2] * inv[8 + k] + inv[12 + k];
    *sx = c[0] * cam.dist / c[2] + cam.compW / 2;
    *sy = c[1] * cam.dist / c[2] + cam.compH / 2;
}

// The group plane shows the texture rect exactly over the screen rect (any depth, any layer transform).
TEST(group_plane_maps_rect)
{
    rnd::CameraView cam = MergeTestCamera();
    rnd::MergeLayerInput l = MergeTestLayer(1);
    const double tex[4] = {0, 0, 120, 100}, rect[4] = {48, 24, 312, 216};
    for (double d : {300.0, 900.0, 2000.0}) {
        double M[16], toW[16];
        CHECK(rnd::GroupPlaneM4(cam, l.layerToWorld, d, tex, rect, M));
        rnd::Mul4(M, l.layerToWorld, toW);
        const double cx[4] = {tex[0], tex[2], tex[2], tex[0]}, cy[4] = {tex[1], tex[1], tex[3], tex[3]};
        const double ex[4] = {rect[0], rect[2], rect[2], rect[0]}, ey[4] = {rect[1], rect[1], rect[3], rect[3]};
        for (int k = 0; k < 4; k++) {
            double w[3];
            for (int j = 0; j < 3; j++) w[j] = cx[k] * toW[j] + cy[k] * toW[4 + j] + toW[12 + j];
            double sx, sy;
            ProjectWorld(cam, w, &sx, &sy);
            CHECK(std::fabs(sx - ex[k]) < 1e-6 && std::fabs(sy - ey[k]) < 1e-6);
        }
        // and at camera-space depth d
        rnd::ScreenQuad q = rnd::ProjectRect(cam, toW, tex);
        CHECK(q.valid && std::fabs(q.zMin - d) < 1e-6 && std::fabs(q.zMax - d) < 1e-6);
    }
}

TEST(group_texture_m3)
{
    const double tex[4] = {-10, 5, 110, 105};
    double m3[9], r[4];
    rnd::GroupTextureM3(tex, 240, 200, m3);
    rnd::TextureLayerRect(m3, 240, 200, r);
    for (int k = 0; k < 4; k++) CHECK(std::fabs(r[k] - tex[k]) < 1e-9);
}

// A group rect (24 px aligned) grown to whole pixels of a 1/5 render; 1/1 and unknown (0) leave it as it is.
TEST(align_rect_to_downsample)
{
    const double in[4] = {24, 48, 408, 312};
    double r[4];
    rnd::AlignRectToDownsample(in, 5, 3, r);
    CHECK(r[0] == 20 && r[1] == 48 && r[2] == 410 && r[3] == 312);
    rnd::AlignRectToDownsample(in, 1, 0, r);
    for (int k = 0; k < 4; k++) CHECK(r[k] == in[k]);
    const double neg[4] = {-24, -48, 0, 24};
    rnd::AlignRectToDownsample(neg, 5, 7, r);
    CHECK(r[0] == -25 && r[1] == -49 && r[2] == 0 && r[3] == 28);
}

// Many render threads asking for the same frame: the plan is built once and shared; new inputs rebuild it.
TEST(plan_cache_one_build)
{
    rnd::MergePlanCache cache;
    std::atomic<int> builds{0};
    auto build = [&] {
        builds++;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        return rnd::MergePlan();
    };
    std::vector<std::shared_ptr<const rnd::MergePlan>> got(8);
    std::vector<std::thread> th;
    for (int i = 0; i < 8; i++) th.emplace_back([&, i] { got[i] = cache.Get(42, 1, build); });
    for (auto& t : th) t.join();
    CHECK(builds == 1);
    for (int i = 1; i < 8; i++) CHECK(got[i] && got[i] == got[0]);
    CHECK(cache.Latest(42) == got[0]);
    auto again = cache.Get(42, 2, build);
    CHECK(builds == 2 && again && again != got[0] && cache.Latest(42) == again);
    CHECK(!cache.Latest(43));
}

TEST(single_group_plan)
{
    rnd::MergeSceneInput in;
    in.cam = MergeTestCamera();
    in.layers.push_back(MergeTestLayer(6));
    rnd::MergePlan p = rnd::BuildSingleGroupPlan(in);
    const rnd::MergeLayerPlan& lp = p.layers.at(rnd::MergeLayerKey(5, 7));
    CHECK(lp.subs.size() == 1 && lp.clusters.size() == 1 && lp.subs[0].cluster == 0 && lp.subs[0].copy < 0);
    CHECK(lp.clusters[0].copies.size() == 6);
    double zMin = 1e300;
    for (int i = 0; i < 6; i++) {
        double toW[16], op;
        rnd::CopyToWorld(lp.in, i, toW, &op);
        rnd::ScreenQuad q = rnd::ProjectRect(in.cam, toW, lp.in.srcRect);
        zMin = std::min(zMin, q.zMin);
        for (int k = 0; k < 4; k++) {  // every copy's visible part inside the group's rect (clipped to the comp)
            const double x = std::min(std::max(q.x[k], 0.0), 400.0), y = std::min(std::max(q.y[k], 0.0), 300.0);
            CHECK(x >= lp.clusters[0].rect[0] && x <= lp.clusters[0].rect[2] && y >= lp.clusters[0].rect[1] &&
                  y <= lp.clusters[0].rect[3]);
        }
    }
    CHECK(std::fabs(lp.clusters[0].depth - zMin) < 1e-6);
    for (int k = 0; k < 4; k++) CHECK(std::fmod(lp.clusters[0].rect[k], rnd::kMergeAlign) == 0);
    // a blocked layer: every copy on its own, in order
    in.layers[0] = MergeTestLayer(6, rnd::MB_LIGHTS);
    p = rnd::BuildSingleGroupPlan(in);
    const rnd::MergeLayerPlan& lb = p.layers.at(rnd::MergeLayerKey(5, 7));
    CHECK(lb.subs.size() == 6 && lb.clusters.empty());
    for (int i = 0; i < 6; i++) CHECK(lb.subs[i].copy == i && lb.subs[i].cluster < 0);
}

// ---- BuildMergePlan (Task 5). World = comp px; the default camera of a 400x300 comp at distance 800 sits at
// (200, 150, -800) looking down +z: world z w is camera-space z w + 800.
static rnd::CameraView PlanCam()
{
    rnd::CameraView c;
    rnd::DefaultCamera(400, 300, 800, &c);
    return c;
}
// layer px (0..120, 0..100) -> a w x h rectangle centred at (cx, cy, z), turned rotY degrees about its vertical axis
static void RectToWorld(double cx, double cy, double z, double w, double h, double rotY, double out[16])
{
    const double a = rotY * 3.14159265358979 / 180, c = std::cos(a), s = std::sin(a);
    const double S[16] = {w / 120, 0, 0, 0, 0, h / 100, 0, 0, 0, 0, 1, 0, -w / 2, -h / 2, 0, 1};
    const double R[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, cx, cy, z, 1};
    rnd::Mul4(S, R, out);
}
struct PlanCopy {
    double cx, cy, z, size, rotY;
};
static rnd::MergeLayerInput PlanLayer(int layerId, const std::vector<PlanCopy>& copies, unsigned block = 0)
{
    rnd::MergeLayerInput l;
    l.compItemId = 1;
    l.layerId = layerId;
    l.p.count = (int)copies.size();
    l.p.merge = true;
    const double r[4] = {0, 0, 120, 100}, I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(l.srcRect, r, sizeof(r));
    std::memcpy(l.layerToWorld, I, sizeof(I));
    for (const PlanCopy& c : copies) {
        double m[16];
        RectToWorld(c.cx, c.cy, c.z, c.size, c.size, c.rotY, m);
        l.explicitCopies.insert(l.explicitCopies.end(), m, m + 16);
    }
    l.block = block;
    return l;
}
static rnd::ScenePlane PlanWall(double cx, double cy, double z, double w, double h, double rotY, bool unbounded = false)
{
    rnd::ScenePlane p;
    RectToWorld(cx, cy, z, w, h, rotY, p.toWorld);
    const double r[4] = {0, 0, 120, 100};
    std::memcpy(p.rect, r, sizeof(r));
    p.unbounded = unbounded;
    return p;
}
static int Individuals(const rnd::MergeLayerPlan& lp)
{
    int n = 0;
    for (auto& s : lp.subs) n += s.copy >= 0;
    return n;
}
static int GroupOf(const rnd::MergeLayerPlan& lp, int copy)
{
    for (size_t g = 0; g < lp.clusters.size(); g++)
        for (int c : lp.clusters[g].copies)
            if (c == copy) return (int)g;
    return -1;
}

TEST(merge_free_one_group)
{
    rnd::MergeSceneInput in;
    in.cam = PlanCam();
    std::vector<PlanCopy> cs;
    for (int i = 0; i < 50; i++) cs.push_back({60.0 + (i * 37) % 280, 40.0 + (i * 53) % 220, 200.0 + (i * 71) % 600, 40, (double)(i * 13 % 70)});
    in.layers.push_back(PlanLayer(7, cs));
    rnd::MergePlan p = rnd::BuildMergePlan(in);
    const auto& lp = p.layers.at(rnd::MergeLayerKey(1, 7));
    CHECK(lp.clusters.size() == 1 && lp.subs.size() == 1 && lp.clusters[0].copies.size() == 50);
}

TEST(merge_wall_splits)
{
    // a wall covering the screen at world z 500 (camera z 1300); 20 copies in front, 20 behind
    rnd::MergeSceneInput in;
    in.cam = PlanCam();
    in.planes.push_back(PlanWall(200, 150, 500, 2000, 2000, 0));
    std::vector<PlanCopy> cs;
    for (int i = 0; i < 40; i++) cs.push_back({80.0 + (i * 37) % 240, 60.0 + (i * 53) % 180, i < 20 ? 300.0 + i * 5 : 550.0 + i * 3, 40, 0});
    in.layers.push_back(PlanLayer(7, cs));
    rnd::MergePlan p = rnd::BuildMergePlan(in);
    const auto& lp = p.layers.at(rnd::MergeLayerKey(1, 7));
    CHECK(lp.clusters.size() == 2 && Individuals(lp) == 0);
    const int gf = GroupOf(lp, 0), gb = GroupOf(lp, 39);
    CHECK(gf >= 0 && gb >= 0 && gf != gb);
    for (int i = 0; i < 40; i++) CHECK(GroupOf(lp, i) == (i < 20 ? gf : gb));
    CHECK(lp.clusters[gf].depth < 1300 && lp.clusters[gb].depth > 1300);
}

TEST(merge_tilted_wall_sides)
{
    // a 300 x 300 wall turned 60 degrees through (200, 150, 500): copies clearly in front, clearly behind, and three
    // that pierce it
    rnd::MergeSceneInput in;
    in.cam = PlanCam();
    in.planes.push_back(PlanWall(200, 150, 500, 300, 300, 60));
    std::vector<PlanCopy> cs;
    for (int i = 0; i < 5; i++) cs.push_back({170.0 + i * 15, 150, 100, 30, 0});  // 0..4 in front
    for (int i = 0; i < 5; i++) cs.push_back({170.0 + i * 15, 150, 1000, 30, 0});  // 5..9 behind
    for (int i = 0; i < 3; i++) cs.push_back({185.0 + i * 15, 120 + i * 30.0, 500, 60, 0});  // 10..12 pierce it
    in.layers.push_back(PlanLayer(7, cs));
    rnd::MergePlan p = rnd::BuildMergePlan(in);
    const auto& lp = p.layers.at(rnd::MergeLayerKey(1, 7));
    CHECK(lp.clusters.size() == 2 && Individuals(lp) == 3);
    for (int i = 10; i < 13; i++) CHECK(GroupOf(lp, i) < 0);
    CHECK(GroupOf(lp, 0) >= 0 && GroupOf(lp, 0) == GroupOf(lp, 4) && GroupOf(lp, 5) == GroupOf(lp, 9) &&
          GroupOf(lp, 0) != GroupOf(lp, 5));
    CHECK(lp.clusters[GroupOf(lp, 0)].depth < lp.clusters[GroupOf(lp, 5)].depth);
}

TEST(quads_intersect_3d)
{
    const double a[4][3] = {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}};
    const double cross[4][3] = {{5, 2, -5}, {5, 2, 5}, {5, 8, 5}, {5, 8, -5}};      // pierces a
    const double beside[4][3] = {{5, 20, -5}, {5, 20, 5}, {5, 30, 5}, {5, 30, -5}};  // its plane, not a
    const double parallel[4][3] = {{0, 0, 3}, {10, 0, 3}, {10, 10, 3}, {0, 10, 3}};
    CHECK(rnd::QuadsIntersect3D(a, cross) && rnd::QuadsIntersect3D(cross, a));
    CHECK(!rnd::QuadsIntersect3D(a, beside) && !rnd::QuadsIntersect3D(beside, a));
    CHECK(!rnd::QuadsIntersect3D(a, parallel));
}

// A copy that straddles a wall's plane (and the wall its plane) beside the wall, overlapping it on screen without
// meeting it (the user's house scene, 11/25): AE orders such surfaces its own way, so nothing merges this frame.
TEST(merge_ambiguous_no_merge)
{
    rnd::MergeSceneInput in;
    in.cam = PlanCam();
    in.planes.push_back(PlanWall(200, 150, 500, 100, 100, 0));
    std::vector<PlanCopy> cs;
    for (int i = 0; i < 5; i++) cs.push_back({60.0 + i * 10, 60, 100, 30, 0});  // would merge on their own
    cs.push_back({200, 232, 500, 60, 72});                                      // just below the wall, turned
    in.layers.push_back(PlanLayer(7, cs));
    rnd::MergePlan p = rnd::BuildMergePlan(in);
    const auto& lp = p.layers.at(rnd::MergeLayerKey(1, 7));
    CHECK(p.ambiguous);
    CHECK(lp.clusters.empty() && Individuals(lp) == 6);
    // the same copy moved up into the wall pierces it: AE cuts it exactly, the others merge
    in.layers[0] = PlanLayer(7, {cs[0], cs[1], cs[2], cs[3], cs[4], {200, 170, 500, 60, 72}});
    rnd::MergePlan q = rnd::BuildMergePlan(in);
    const auto& lq = q.layers.at(rnd::MergeLayerKey(1, 7));
    CHECK(!q.ambiguous && lq.clusters.size() == 1 && GroupOf(lq, 5) < 0);
}

TEST(merge_side_free)
{
    // a narrow wall at the screen's centre; copies at the same depth in the free space left and right of it
    rnd::MergeSceneInput in;
    in.cam = PlanCam();
    in.planes.push_back(PlanWall(200, 150, 500, 40, 300, 0));
    std::vector<PlanCopy> cs;
    for (int i = 0; i < 10; i++) cs.push_back({i < 5 ? 60.0 + i * 5 : 330.0 + i * 5, 60.0 + i * 18, 480.0 + i * 4, 30, 0});
    in.layers.push_back(PlanLayer(7, cs));
    rnd::MergePlan p = rnd::BuildMergePlan(in);
    const auto& lp = p.layers.at(rnd::MergeLayerKey(1, 7));
    CHECK(lp.clusters.size() == 1 && lp.subs.size() == 1);
}

TEST(merge_near_plane_individual)
{
    rnd::MergeSceneInput in;
    in.cam = PlanCam();
    std::vector<PlanCopy> cs;
    for (int i = 0; i < 6; i++) cs.push_back({100.0 + i * 40, 150, 300, 40, 0});
    cs.push_back({260, 150, -790, 300, 80});  // crosses the camera's plane (z -800), beside the camera
    in.layers.push_back(PlanLayer(7, cs));
    rnd::MergePlan p = rnd::BuildMergePlan(in);
    const auto& lp = p.layers.at(rnd::MergeLayerKey(1, 7));
    CHECK(GroupOf(lp, 6) < 0 && Individuals(lp) >= 1);
    CHECK(GroupOf(lp, 0) >= 0 && GroupOf(lp, 0) == GroupOf(lp, 5));
}

TEST(merge_blocked_layer)
{
    rnd::MergeSceneInput in;
    in.cam = PlanCam();
    std::vector<PlanCopy> cs;
    for (int i = 0; i < 6; i++) cs.push_back({100.0 + i * 40, 150, 300, 40, 0});
    in.layers.push_back(PlanLayer(7, cs, rnd::MB_LIGHTS));
    rnd::MergePlan p = rnd::BuildMergePlan(in);
    const auto& lp = p.layers.at(rnd::MergeLayerKey(1, 7));
    CHECK(lp.clusters.empty() && lp.subs.size() == 6);
    for (int i = 0; i < 6; i++) CHECK(lp.subs[i].copy == i);
}

TEST(merge_group_depth_order)
{
    // two merge-on layers overlapping on screen: A (z 300..400) in front of B (z 600..700)
    rnd::MergeSceneInput in;
    in.cam = PlanCam();
    std::vector<PlanCopy> a, b;
    for (int i = 0; i < 10; i++) a.push_back({120.0 + i * 15, 150, 300.0 + i * 10, 40, 0});
    for (int i = 0; i < 10; i++) b.push_back({120.0 + i * 15, 160, 600.0 + i * 10, 40, 0});
    in.layers.push_back(PlanLayer(7, a));
    in.layers.push_back(PlanLayer(8, b));
    rnd::MergePlan p = rnd::BuildMergePlan(in);
    const auto& la = p.layers.at(rnd::MergeLayerKey(1, 7));
    const auto& lb = p.layers.at(rnd::MergeLayerKey(1, 8));
    CHECK(la.clusters.size() == 1 && lb.clusters.size() == 1);
    CHECK(la.clusters[0].depth < lb.clusters[0].depth);
    // interleaved: A at 300 and 900, B at 600, all overlapping: A's near and far copies cannot share a group
    rnd::MergeSceneInput in2;
    in2.cam = PlanCam();
    std::vector<PlanCopy> a2, b2;
    for (int i = 0; i < 5; i++) a2.push_back({150.0 + i * 20, 150, 300, 60, 0});
    for (int i = 0; i < 5; i++) a2.push_back({150.0 + i * 20, 150, 900, 60, 0});
    for (int i = 0; i < 5; i++) b2.push_back({150.0 + i * 20, 150, 600, 60, 0});
    in2.layers.push_back(PlanLayer(7, a2));
    in2.layers.push_back(PlanLayer(8, b2));
    rnd::MergePlan p2 = rnd::BuildMergePlan(in2);
    const auto& la2 = p2.layers.at(rnd::MergeLayerKey(1, 7));
    const auto& lb2 = p2.layers.at(rnd::MergeLayerKey(1, 8));
    const int gn = GroupOf(la2, 0), gf = GroupOf(la2, 5);
    CHECK(gn < 0 || gf < 0 || gn != gf);
    if (gn >= 0 && GroupOf(lb2, 0) >= 0) CHECK(la2.clusters[gn].depth < lb2.clusters[GroupOf(lb2, 0)].depth);
    if (gf >= 0 && GroupOf(lb2, 0) >= 0) CHECK(la2.clusters[gf].depth > lb2.clusters[GroupOf(lb2, 0)].depth);
}

TEST(merge_area_cap)
{
    rnd::MergeSceneInput in;
    in.cam = PlanCam();
    std::vector<PlanCopy> cs;
    for (int i = 0; i < 10; i++) cs.push_back({20.0 + i * 40, 20.0 + i * 28, 300, 40, 0});  // spread over the screen
    in.layers.push_back(PlanLayer(7, cs));
    rnd::MergeOptions o;
    o.maxAreaScreens = 0.3;
    rnd::MergePlan p = rnd::BuildMergePlan(in, o);
    const auto& lp = p.layers.at(rnd::MergeLayerKey(1, 7));
    double area = 0;
    for (auto& c : lp.clusters) area += (c.rect[2] - c.rect[0]) * (c.rect[3] - c.rect[1]);
    CHECK(area <= 0.3 * 400 * 300 + 1e-6);
}

static void PlanSpeed(const char* name, int n, int walls, double size, double maxMs, bool expectGroups)
{
    rnd::MergeSceneInput in;
    in.cam = PlanCam();
    for (int k = 0; k < walls; k++) in.planes.push_back(PlanWall(400.0 * (k + 0.5) / walls, 150, 100.0 + 50 * k, 60, 300, 30.0 * (k % 3)));
    std::vector<PlanCopy> cs;
    for (int i = 0; i < n; i++)
        cs.push_back({(double)((i * 37) % 400), (double)((i * 53) % 300), 50.0 + (i * 71) % 1200, size, (double)(i * 13 % 70)});
    in.layers.push_back(PlanLayer(7, cs));
    auto t0 = std::chrono::steady_clock::now();
    rnd::MergePlan p = rnd::BuildMergePlan(in);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const auto& lp = p.layers.at(rnd::MergeLayerKey(1, 7));
    std::printf("  %s: %d copies, %d walls: %.1f ms, %d rounds (pre %.0f, filter %.0f, groups %.0f, depths %.0f ms)%s, %zu groups, "
                "%d individual, AE cycle %d ties %d\n",
                name, n, walls, ms, p.rounds, p.ms[0], p.ms[1], p.ms[2], p.ms[3], p.overBudget ? " OVER BUDGET" : "",
                lp.clusters.size(), Individuals(lp));
    CHECK(ms < maxMs);
    if (p.overBudget) CHECK(lp.clusters.empty() && (int)lp.subs.size() == n);  // nothing merges
    if (expectGroups) CHECK(!p.overBudget && !lp.clusters.empty());
}

// a bounded cost however dense the scene (past the budget nothing merges), and a realistic scene merges quickly
TEST(merge_plan_speed)
{
    PlanSpeed("merge_plan_speed (stress)", 5000, 20, 20, 600, false);  // the budget (200 ms) bounds it
    PlanSpeed("merge_plan_speed (field)", 1000, 3, 30, 100, true);
    PlanSpeed("merge_plan_speed (free)", 5000, 0, 20, 100, true);
}

// ---- BuildFrontBackGroupPlan (11/26)
// The plan's subs and groups agree: one sub per group (its representative, in the group), the rest one by one, every
// copy once; a front group's place is +1, a back group's -1.
static bool FbPlanWellFormed(const rnd::MergeLayerPlan& lp, int count)
{
    std::vector<int> seen(count, 0);
    std::vector<int> subsOf(lp.clusters.size(), 0);
    for (const auto& s : lp.subs) {
        if (s.copy < 0 || s.copy >= count) return false;
        if (s.cluster < 0) {
            seen[s.copy]++;
            continue;
        }
        if (s.cluster >= (int)lp.clusters.size()) return false;
        subsOf[s.cluster]++;
        const auto& c = lp.clusters[s.cluster].copies;
        if (std::find(c.begin(), c.end(), s.copy) == c.end()) return false;
    }
    for (size_t g = 0; g < lp.clusters.size(); g++) {
        if (subsOf[g] != 1 || lp.clusters[g].copies.size() < 2 || !lp.clusters[g].place) return false;
        for (int c : lp.clusters[g].copies) seen[c]++;
    }
    for (int v : seen)
        if (v != 1) return false;
    return true;
}
static int FbPlace(const rnd::MergeLayerPlan& lp, int copy)
{
    const int g = GroupOf(lp, copy);
    return g < 0 ? 0 : lp.clusters[g].place;
}
static bool InQuad(const rnd::ScreenQuad& q, double x, double y)
{
    int pos = 0, neg = 0;
    for (int k = 0; k < 4; k++) {
        const int n = (k + 1) % 4;
        const double c = (q.x[n] - q.x[k]) * (y - q.y[k]) - (q.y[n] - q.y[k]) * (x - q.x[k]);
        pos += c > 0, neg += c < 0;
    }
    return !pos || !neg;
}

TEST(fb_wall_front_back)
{
    // a wall covering the screen at world z 500; copies 0..19 in front of it, 20..39 behind, apart from each other
    rnd::MergeSceneInput in;
    in.cam = PlanCam();
    in.planes.push_back(PlanWall(200, 150, 500, 2000, 2000, 0));
    std::vector<PlanCopy> cs;
    for (int i = 0; i < 40; i++) cs.push_back({30.0 + (i % 8) * 48, 30.0 + (i / 8) * 55, i < 20 ? 300.0 + i * 5 : 550.0 + i * 3, 30, 0});
    in.layers.push_back(PlanLayer(7, cs));
    rnd::MergePlan p = rnd::BuildFrontBackGroupPlan(in);
    const auto& lp = p.layers.at(rnd::MergeLayerKey(1, 7));
    CHECK(FbPlanWellFormed(lp, 40));
    CHECK(lp.clusters.size() == 2 && lp.subs.size() == 2);
    for (int i = 0; i < 40; i++) CHECK(FbPlace(lp, i) == (i < 20 ? 1 : -1));
}

TEST(fb_between_crossing_band)
{
    // copy 0 between two walls (a near one over its left half, the far one everywhere): not grouped. Copy 1 crosses
    // the far wall (turned 60 degrees through it): not grouped. Copies 2 and 3 are 2 px apart (the margin band, no
    // edge in AE): not grouped. Copies 4..7 in front of the far wall, apart: the front group.
    rnd::MergeSceneInput in;
    in.cam = PlanCam();
    in.planes.push_back(PlanWall(200, 150, 500, 2000, 2000, 0));
    in.planes.push_back(PlanWall(60, 60, 100, 40, 40, 0));
    std::vector<PlanCopy> cs = {{60, 60, 300, 40, 0}, {200, 60, 500, 60, 60}, {100, 200, 300, 40, 0},
                                {142, 200, 300, 40, 0}, {300, 60, 300, 30, 0}, {350, 60, 300, 30, 0},
                                {300, 230, 300, 30, 0}, {350, 230, 300, 30, 0}};
    in.layers.push_back(PlanLayer(7, cs));
    rnd::MergePlan p = rnd::BuildFrontBackGroupPlan(in);
    const auto& lp = p.layers.at(rnd::MergeLayerKey(1, 7));
    CHECK(FbPlanWellFormed(lp, 8));
    for (int i = 0; i < 4; i++) CHECK(FbPlace(lp, i) == 0);
    for (int i = 4; i < 8; i++) CHECK(FbPlace(lp, i) == 1);
    CHECK(p.fb[3] == 3 && p.fb[4] == 1);  // unsure: crossing and the band pair; between: copy 0
    CHECK(p.fb[6] == 1 && p.fb[11] == 1);  // the band pair (two copies), the crossing (a copy and the wall)
}

TEST(fb_two_layers_and_blocked)
{
    // two merge layers over the same wall; a blocked layer's copies are outside surfaces for the others
    rnd::MergeSceneInput in;
    in.cam = PlanCam();
    in.planes.push_back(PlanWall(200, 150, 500, 2000, 2000, 0));
    in.layers.push_back(PlanLayer(7, {{60, 60, 300, 40, 0}, {160, 60, 300, 40, 0}, {60, 200, 600, 40, 0}, {160, 200, 600, 40, 0}}));
    // layer 8: copy 0 overlaps layer 7's copy 0 from behind it (both in front of the wall); copy 1 apart
    in.layers.push_back(PlanLayer(8, {{80, 70, 350, 40, 0}, {300, 60, 300, 40, 0}, {300, 200, 300, 40, 0}}));
    in.layers.push_back(PlanLayer(9, {{300, 120, 200, 40, 0}, {340, 260, 200, 40, 0}}, rnd::MB_LIGHTS));
    rnd::MergePlan p = rnd::BuildFrontBackGroupPlan(in);
    const auto& a = p.layers.at(rnd::MergeLayerKey(1, 7));
    const auto& b = p.layers.at(rnd::MergeLayerKey(1, 8));
    const auto& c = p.layers.at(rnd::MergeLayerKey(1, 9));
    CHECK(FbPlanWellFormed(a, 4) && FbPlanWellFormed(b, 3) && FbPlanWellFormed(c, 2));
    CHECK(c.clusters.empty());
    CHECK(FbPlace(a, 0) == 1 && FbPlace(a, 1) == 1 && FbPlace(a, 2) == -1 && FbPlace(a, 3) == -1);
    CHECK(FbPlace(b, 0) == 0);  // in front of the wall, behind layer 7's copy 0 (outside its group): between
}

// A random field of n copies around two walls: every grouped copy is, wherever it shares a point of the screen with
// anything outside its group, nearer than it (front) or farther (back)
static void FbOrdersHold(const char* name, int n, unsigned seed)
{
    rnd::MergeSceneInput in;
    in.cam = PlanCam();
    in.planes.push_back(PlanWall(200, 150, 500, 300, 200, 20));
    in.planes.push_back(PlanWall(120, 100, 250, 80, 60, -30));
    std::vector<PlanCopy> cs;
    unsigned s = seed;
    auto rnd01 = [&] { return (s = s * 1103515245u + 12345u, (s >> 8) / 16777216.0); };
    for (int i = 0; i < n; i++) cs.push_back({rnd01() * 400, rnd01() * 300, 100 + rnd01() * 700, 10 + rnd01() * 30, rnd01() * 120 - 60});
    in.layers.push_back(PlanLayer(7, cs));
    rnd::MergePlan p = rnd::BuildFrontBackGroupPlan(in);
    const auto& lp = p.layers.at(rnd::MergeLayerKey(1, 7));
    CHECK(FbPlanWellFormed(lp, n));
    CHECK(p.fb[1] + p.fb[2] > 0);
    struct S {
        double w[16];
        rnd::ScreenQuad q;
        int place;  // 0 for walls and copies one by one
        int group;
    };
    std::vector<S> ss;
    for (int i = 0; i < n; i++) {
        S x;
        std::memcpy(x.w, &in.layers[0].explicitCopies[(size_t)i * 16], sizeof(x.w));
        x.q = rnd::ProjectRect(in.cam, x.w, in.layers[0].srcRect);
        x.group = GroupOf(lp, i), x.place = FbPlace(lp, i);
        ss.push_back(x);
    }
    for (const auto& w : in.planes) {
        S x;
        std::memcpy(x.w, w.toWorld, sizeof(x.w));
        x.q = rnd::ProjectRect(in.cam, x.w, w.rect);
        x.group = -2 - (int)ss.size(), x.place = 0;
        ss.push_back(x);
    }
    int checked = 0, bad = 0;
    for (size_t i = 0; i < (size_t)n; i++) {
        if (!ss[i].place) continue;
        for (size_t j = 0; j < ss.size(); j++) {
            if (j == i || ss[j].group == ss[i].group) continue;
            for (int gy = 0; gy <= 150; gy++)
                for (int gx = 0; gx <= 200; gx++) {
                    const double x = gx * 2.0, y = gy * 2.0;
                    if (!InQuad(ss[i].q, x, y) || !InQuad(ss[j].q, x, y)) continue;
                    double zi = 0, zj = 0;
                    if (!rnd::PlaneDepthAt(in.cam, ss[i].w, x, y, &zi) || !rnd::PlaneDepthAt(in.cam, ss[j].w, x, y, &zj)) continue;
                    checked++;
                    if (ss[i].place > 0 ? !(zi < zj) : !(zi > zj)) bad++;
                }
        }
    }
    printf("  %s: %d front, %d back, %d unsure, %d between; %d shared points checked, %d wrong\n", name, p.fb[1], p.fb[2],
           p.fb[3], p.fb[4], checked, bad);
    CHECK(checked > 0 && bad == 0);
}

TEST(fb_orders_hold)
{
    FbOrdersHold("fb_orders_hold (dense)", 300, 12345);
    FbOrdersHold("fb_orders_hold (sparse)", 80, 777);
    FbOrdersHold("fb_orders_hold (sparse 2)", 80, 4242);
}

// Test B (11/26): a group's representative must be a copy AE draws. A big copy near the camera but off the screen
// (AE culls it) would take the whole group with it; copies off screen still join groups.
TEST(fb_representative_on_screen)
{
    rnd::MergeSceneInput in;
    in.cam = PlanCam();
    // copy 0: huge, near the camera, entirely left of the screen; copies 1..3 small on screen; copy 4 off screen right
    in.layers.push_back(PlanLayer(7, {{-900, 150, -300, 400, 0}, {100, 100, 300, 30, 0}, {200, 100, 300, 30, 0},
                                      {300, 100, 300, 30, 0}, {1500, 150, 300, 30, 0}}));
    for (int iso = 0; iso < 2; iso++) {
        rnd::MergePlan p = iso ? rnd::BuildIsolatedGroupPlan(in) : rnd::BuildFrontBackGroupPlan(in);
        const auto& lp = p.layers.at(rnd::MergeLayerKey(1, 7));
        CHECK(lp.clusters.size() == 1 && lp.clusters[0].copies.size() == 5);
        int rep = -1;
        for (const auto& s : lp.subs)
            if (s.cluster >= 0) rep = s.copy;
        CHECK(rep >= 1 && rep <= 3);
    }
    // nothing on screen: no group (every copy one by one, AE culls them)
    rnd::MergeSceneInput off;
    off.cam = PlanCam();
    off.layers.push_back(PlanLayer(7, {{-900, 150, -300, 400, 0}, {1500, 150, 300, 30, 0}, {1600, 150, 300, 30, 0}}));
    CHECK(rnd::BuildFrontBackGroupPlan(off).layers.at(rnd::MergeLayerKey(1, 7)).clusters.empty());
    CHECK(rnd::BuildIsolatedGroupPlan(off).layers.at(rnd::MergeLayerKey(1, 7)).clusters.empty());
}

// ---- core/ae_pair against Advanced3D's own pair test, called in this process: the AE 2025 Advanced3D.aex is loaded
// (with its Support Files for its DLLs) and sub_18002B040 is called on surfaces made here, the same inputs given to
// AePairBoxes / AePairSamples. Skipped when AE 2025 is not installed. RANDAMU_PAIR_FUZZ: the number of cases.
namespace pairfuzz {
typedef void (*PairFn)(uintptr_t a, uintptr_t b, unsigned char* f1, unsigned char* f2);
// a surface as sub_18002B040 reads it: +8 x0, +0x10 y0, +0x18 x1, +0x20 y1, +0xC8 its samples (192 bytes each),
// +0xD0 their count; a sample: +0 / +8 its vertex records (32 bytes: x, y, ...), +0x18 .. +0x30 a, b, c, d
struct Surf {
    alignas(16) unsigned char s[232] = {};
    std::vector<unsigned char> samples;
    std::vector<std::vector<double>> verts;
};
void Make(Surf& S, const std::vector<std::vector<double>>& polys, const std::vector<std::array<double, 4>>& planes)
{
    S.verts = polys;
    S.samples.assign(192 * polys.size(), 0);
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (size_t k = 0; k < polys.size(); k++) {
        unsigned char* r = &S.samples[192 * k];
        const double* b = S.verts[k].data();
        const double* e = b + S.verts[k].size();
        std::memcpy(r, &b, 8), std::memcpy(r + 8, &e, 8);
        std::memcpy(r + 0x18, planes[k].data(), 32);
        for (size_t i = 0; i + 3 < S.verts[k].size(); i += 4)
            x0 = std::min(x0, b[i]), x1 = std::max(x1, b[i]), y0 = std::min(y0, b[i + 1]), y1 = std::max(y1, b[i + 1]);
    }
    const double box[4] = {x0, y0, x1, y1};
    std::memcpy(S.s + 8, box, 32);
    unsigned char* sp = S.samples.data();
    std::memcpy(S.s + 0xC8, &sp, 8);
    const int n = (int)polys.size();
    std::memcpy(S.s + 0xD0, &n, 4);
}
// ours: sub_18002B040 with core/ae_pair
void Ours(const Surf& A, const Surf& B, unsigned char* f1, unsigned char* f2, bool* ok)
{
    *f1 = *f2 = 0, *ok = true;
    double a[4], b[4];
    std::memcpy(a, A.s + 8, 32), std::memcpy(b, B.s + 8, 32);
    if (!rnd::AePairBoxes(a, b)) return;
    const int nA = *(const int*)(A.s + 0xD0), nB = *(const int*)(B.s + 0xD0), n = std::max(nA, nB);
    for (int k = 0; k < n; k++) {
        auto sample = [](const Surf& S, int idx, rnd::AePairSample* out) {
            const unsigned char* r = *(unsigned char* const*)(S.s + 0xC8) + 192 * (size_t)idx;
            const double* b = *(const double* const*)r;
            const double* e = *(const double* const*)(r + 8);
            if (b == e) return false;
            out->v = b, out->n = (int)((e - b) / 4);
            std::memcpy(&out->a, r + 0x18, 32);
            return true;
        };
        rnd::AePairSample sa, sb;
        if (!sample(A, std::min(k, nA - 1), &sa) || !sample(B, std::min(k, nB - 1), &sb)) continue;
        if (!rnd::AePairSamples(sa, sb, f1, f2)) {
            *ok = false;
            return;
        }
        if (*f1 && *f2) return;
    }
}
// U.dll's memory bookkeeping is not set up outside AE: the pair test's calls to it go to these (the work memory is
// allocated by the callback as in AE, once)
typedef int (*PurgeCb)(void* ctx, unsigned char* done);
int StubPurgeLoop(size_t, PurgeCb cb, void* ctx, int)
{
    unsigned char done = 0;
    return cb(ctx, &done);
}
int StubAccount(size_t, bool) { return 0; }
int StubQuiet() { return 0; }
bool PatchImport(HMODULE m, const char* dll, const char* name, void* fn)
{
    auto base = (unsigned char*)m;
    auto nt = (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((const char*)(base + d->Name), dll)) continue;
        auto names = (IMAGE_THUNK_DATA64*)(base + d->OriginalFirstThunk);
        auto slots = (IMAGE_THUNK_DATA64*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, name)) continue;
            DWORD old = 0;
            VirtualProtect(&slots->u1.Function, 8, PAGE_READWRITE, &old);
            slots->u1.Function = (ULONGLONG)fn;
            VirtualProtect(&slots->u1.Function, 8, old, &old);
            return true;
        }
    }
    return false;
}
}  // namespace pairfuzz

TEST(ae_pair_matches_advanced3d)
{
    using namespace pairfuzz;
    const char* dir = "C:\\Program Files\\Adobe\\Adobe After Effects 2025\\Support Files";
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\Required\\Advanced3D.aex", dir);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        printf("  ae_pair: AE 2025 not installed, skipped\n");
        return;
    }
    SetDllDirectoryA(dir);
    HMODULE m = LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        printf("  ae_pair: Advanced3D.aex not loaded (error %lu), skipped\n", GetLastError());
        return;
    }
    const unsigned char* fn = (const unsigned char*)m + 0x2B040;
    static const unsigned char kHead[] = {0x40, 0x55, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x38};
    if (std::memcmp(fn, kHead, sizeof(kHead))) {
        printf("  ae_pair: Advanced3D is another version, skipped\n");
        return;
    }
    const PairFn ae = (PairFn)fn;
    const bool stubs = PatchImport(m, "U.dll", "U_PurgeLoop", (void*)&StubPurgeLoop) &&
                       PatchImport(m, "U.dll", "U_AccountAllocationEx", (void*)&StubAccount) &&
                       PatchImport(m, "U.dll", "U_StartQuietErrorsOnCurrentThread", (void*)&StubQuiet) &&
                       PatchImport(m, "U.dll", "U_EndQuietErrorsOnCurrentThread", (void*)&StubQuiet);
    CHECK(stubs);
    if (!stubs) return;
    char ev[16] = {0};
    const int cases = GetEnvironmentVariableA("RANDAMU_PAIR_FUZZ", ev, sizeof(ev)) > 0 ? atoi(ev) : 200000;
    unsigned s = 20261126;
    auto r01 = [&] { return (s = s * 1103515245u + 12345u, (s >> 8) / 16777216.0); };
    // coordinates: often on a coarse grid (shared lines, touching corners, equal values), sometimes free
    auto coord = [&](double base) { return r01() < 0.6 ? base + std::floor(r01() * 8) * 2 : base + r01() * 16; };
    auto poly = [&](double cx, double cy) {
        const int n = r01() < 0.1 ? 3 : r01() < 0.9 ? 4 : 5 + (int)(r01() * 4);
        std::vector<double> v;
        const double rx = 2 + r01() * 8, ry = 2 + r01() * 8, a0 = r01() * 6.283;
        for (int k = 0; k < n; k++) {
            double x, y;
            if (r01() < 0.5) {  // a convex polygon
                const double a = a0 + 6.283 * k / n;
                x = cx + rx * std::cos(a), y = cy + ry * std::sin(a);
                if (r01() < 0.3) x = std::round(x), y = std::round(y);
            } else {
                x = coord(cx - 8), y = coord(cy - 8);
            }
            v.insert(v.end(), {x, y, 0, 0});
        }
        return v;
    };
    auto plane = [&](const std::array<double, 4>* like) {
        std::array<double, 4> p;
        if (like && r01() < 0.3) {  // the same plane, or almost
            p = *like;
            if (r01() < 0.5) p[3] += (r01() - 0.5) * 1e-11;
            return p;
        }
        p = {r01() - 0.5, r01() - 0.5, 0.5 + r01(), (r01() - 0.5) * 20};
        if (r01() < 0.3) p[0] = 0, p[1] = 0;
        return p;
    };
    int same = 0, diff = 0, skipped = 0, tested = 0, f1n = 0, f2n = 0;
    for (int c = 0; c < cases; c++) {
        Surf A, B;
        const int na = r01() < 0.9 ? 1 : 2, nb = r01() < 0.9 ? 1 : 2;
        std::vector<std::vector<double>> pa, pb;
        std::vector<std::array<double, 4>> qa, qb;
        for (int k = 0; k < na; k++) pa.push_back(poly(10, 10)), qa.push_back(plane(nullptr));
        for (int k = 0; k < nb; k++) pb.push_back(poly(10 + (r01() - 0.5) * 12, 10 + (r01() - 0.5) * 12)), qb.push_back(plane(&qa[0]));
        Make(A, pa, qa), Make(B, pb, qb);
        unsigned char a1 = 7, a2 = 7, o1 = 0, o2 = 0;
        bool ok = true;
        ae((uintptr_t)A.s, (uintptr_t)B.s, &a1, &a2);
        Ours(A, B, &o1, &o2, &ok);
        if (!ok) {
            skipped++;
            continue;
        }
        tested++;
        f1n += a1, f2n += a2;
        if (a1 == o1 && a2 == o2) same++;
        else if (diff++ < 5) printf("  ae_pair: case %d differs: AE %d %d ours %d %d\n", c, a1, a2, o1, o2);
    }
    printf("  ae_pair: %d cases: %d same, %d differ, %d not ours (AE f1 %d f2 %d)\n", tested, same, diff, skipped, f1n, f2n);
    const long long* cov = rnd::AePairCoverage();
    printf("  ae_pair: paths: collinear %lld, on b's line %lld / %lld, on a's line %lld / %lld, crossing %lld, inside %lld / %lld\n",
           cov[0], cov[1], cov[2], cov[3], cov[4], cov[5], cov[6], cov[7]);
    CHECK(tested > 0 && diff == 0);
    for (int k = 0; k < AE_PAIR_COV_N; k++) CHECK(cov[k] > 100);  // every path taken often enough to mean something
}

// The plan's cost (it is rebuilt whenever the scene changes, every frame of an animation)
TEST(fb_plan_speed)
{
    for (const int n : {380, 1000, 5000}) {
        rnd::MergeSceneInput in;
        in.cam = PlanCam();
        in.planes.push_back(PlanWall(200, 150, 500, 300, 200, 20));
        std::vector<PlanCopy> cs;
        unsigned s = 99;
        auto rnd01 = [&] { return (s = s * 1103515245u + 12345u, (s >> 8) / 16777216.0); };
        for (int i = 0; i < n; i++) cs.push_back({rnd01() * 400, rnd01() * 300, 100 + rnd01() * 700, 8 + rnd01() * 16, rnd01() * 120 - 60});
        in.layers.push_back(PlanLayer(7, cs));
        const auto t0 = std::chrono::steady_clock::now();
        rnd::MergePlan p = rnd::BuildFrontBackGroupPlan(in);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const auto t1 = std::chrono::steady_clock::now();
        rnd::BuildIsolatedGroupPlan(in);
        const double iso = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        printf("  fb_plan_speed: %d copies %.1f ms (%d front, %d back%s; the isolated plan alone %.1f ms)\n", n, ms,
               p.fb[1], p.fb[2], p.fb[12] ? ", over budget: isolated groups" : "", iso);
        CHECK(ms < iso * 1.5 + 80);  // the budget (40 ms) and the isolated plan bound it (a busy PC: room to spare)
        const auto& lp = p.layers.at(rnd::MergeLayerKey(1, 7));
        for (const auto& c : lp.clusters) CHECK(p.fb[12] ? c.place == 0 : c.place != 0);
    }
}

// The merge checkbox goes after every shipped parameter (stream indices and disk IDs never move) and fits in
// ScatterParams' padding (the saved sequence data keeps its size); it is off by default.
TEST(params_merge_last)
{
    CHECK(RP_MERGE == RP_WORLD_TOPIC_END + 1);
    CHECK(RP_NUM_PARAMS == RP_MERGE + 1);
    CHECK(RD_MERGE == RD_WORLD_TOPIC_END + 1);
    CHECK(sizeof(rnd::ScatterParams) == 96);
    rnd::ScatterParams p;
    CHECK(!p.merge);
}

// 20000 copies that each cover a whole 8K buffer (a corner behind the camera) make 2.6e9 tile entries: more than an
// int can count. BuildTiles must refuse with bad_alloc at once (AE reports it as out of memory), never wrap around
// (a wrapped positive total would write past the list's end).
TEST(tiles_entry_limit)
{
    rnd::RasterImage s = {nullptr, 100, 100, 400, 8, 0, 0}, d = {nullptr, 7680, 4320, 7680 * 4, 8, 0, 0};
    std::vector<rnd::RasterCopy> c(20000);
    for (auto& x : c) {
        std::memset(x.screenToSrc, 0, sizeof(x.screenToSrc));  // not invertible: covers everything
        x.opacity = 1;
        x.depth = 1;
    }
    rnd::TileLists t;
    bool badAlloc = false;
    auto t0 = std::chrono::steady_clock::now();
    try {
        rnd::BuildTiles(s, c.data(), (int)c.size(), 1.0, 1.0, d, 16, &t);
    } catch (const std::bad_alloc&) {
        badAlloc = true;
    } catch (...) {
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(badAlloc);
    CHECK(ms < 200);  // refused before counting tile by tile
}

template <class P>
static int MaxDiff8(const std::vector<P>& a, const std::vector<P>& b, float scale)
{
    float m = 0;
    for (size_t i = 0; i < a.size(); i++)
        for (int k = 0; k < 4; k++) {
            float x = (float)((const decltype(a[0].a)*)&a[i])[k], y = (float)((const decltype(a[0].a)*)&b[i])[k];
            m = std::max(m, std::fabs(x - y) * scale);
        }
    return (int)std::ceil(m - 1e-3f);
}

TEST(raster_matches_reference)
{
    // 20 scenes: 1..200 copies, oblique, semi-transparent, off screen, near the camera; 8 and 32 bpc; ds 1 and 0.5
    const int counts[5] = {1, 7, 30, 90, 200};
    int worst = 0;
    for (int sc = 0; sc < 20; sc++) {
        const int n = counts[sc % 5];
        auto copies = SceneCopies(100 + sc, n, 120, 90, 500 + 100 * (sc % 3), sc % 2 ? 80 : 20, sc % 4 ? 30 : 100);
        const double ds = sc % 3 == 2 ? 0.5 : 1.0;
        const int w = (int)(640 * ds), h = (int)(360 * ds), ox = (int)(640 * ds), oy = (int)(360 * ds);
        auto s8 = MakeSrc8(120, 90);
        if (sc % 2 == 0) {
            std::vector<Px8> a((size_t)w * h), b((size_t)w * h);
            rnd::RasterImage s = Img(s8, 120, 90, 8), da = Img(a, w, h, 8, ox, oy), db = Img(b, w, h, 8, ox, oy);
            da.screenShiftX = db.screenShiftX = sc * 3.25;
            auto c1 = copies, c2 = copies;
            rnd::RasterizeCopies(s, c1.data(), (int)c1.size(), ds, ds, da, 4);
            rnd::RasterizeCopiesReference(s, c2.data(), (int)c2.size(), ds, ds, db, 4);
            worst = std::max(worst, MaxDiff8(a, b, 1.f));
        } else {
            std::vector<PxF> sf(s8.size()), a((size_t)w * h), b((size_t)w * h);
            for (size_t i = 0; i < s8.size(); i++)
                sf[i] = PxF{s8[i].a / 255.f, s8[i].r / 255.f, s8[i].g / 255.f, s8[i].b / 255.f};
            rnd::RasterImage s = Img(sf, 120, 90, 32), da = Img(a, w, h, 32, ox, oy), db = Img(b, w, h, 32, ox, oy);
            auto c1 = copies, c2 = copies;
            rnd::RasterizeCopies(s, c1.data(), (int)c1.size(), ds, ds, da, 4);
            rnd::RasterizeCopiesReference(s, c2.data(), (int)c2.size(), ds, ds, db, 4);
            worst = std::max(worst, MaxDiff8(a, b, 255.f));
        }
    }
    std::printf("  tiled vs reference: max diff %d (8 bpc units)\n", worst);
    CHECK(worst <= 1);
}

TEST(raster_overflow_64)
{
    // 100 semi-transparent copies on the same pixels, depths interleaved: over the 64-fragment limit
    std::vector<Px8> src = MakeSrc8(20, 20), a((size_t)20 * 20), b((size_t)20 * 20);
    rnd::RasterImage s = Img(src, 20, 20, 8), da = Img(a, 20, 20, 8), db = Img(b, 20, 20, 8);
    std::vector<rnd::RasterCopy> c;
    for (int i = 0; i < 100; i++) {
        rnd::RasterCopy k = Copy(0.3, 0, 1.0 + 0.001 * i, 0, 0);
        k.screenToSrc[8] = 1.0 + 0.01 * ((i * 37) % 100);  // per-pixel depth 1 / w varies by copy
        k.screenToSrc[0] *= k.screenToSrc[8];
        k.screenToSrc[4] *= k.screenToSrc[8];
        k.depth = 1000 - i;
        c.push_back(k);
    }
    auto c2 = c;
    rnd::RasterizeCopies(s, c.data(), 100, 1.0, 1.0, da, 2);
    rnd::RasterizeCopiesReference(s, c2.data(), 100, 1.0, 1.0, db, 2);
    CHECK(MaxDiff8(a, b, 1.f) <= 1);
}

// Past the 64-fragment limit the order must stay right: 70 faint fronto-parallel copies on one pixel, each a
// different colour, arriving nearest first. The 65th (farther than every kept fragment) must go under the
// others, not above the evicted one; a farther opaque copy must not hide nearer evicted ones.
TEST(raster_overflow_order)
{
    for (int opaqueLast = 0; opaqueLast < 2; opaqueLast++) {
        const int n = 70;
        std::vector<PxF> src(n), a(1), b(1);
        for (int i = 0; i < n; i++) src[i] = i % 2 ? PxF{1, 0, 0, 1} : PxF{1, 1, 0, 0};
        rnd::RasterImage s = Img(src, n, 1, 32), da = Img(a, 1, 1, 32), db = Img(b, 1, 1, 32);
        std::vector<rnd::RasterCopy> c;
        for (int i = 0; i < n; i++) {
            const double w = 1.0 + 0.01 * i;  // 1 / camera z: copy n-1 is the nearest
            // screen (x, y) -> src (x + i, y) at depth 1 / w: u = (x * w + i * w) / w
            rnd::RasterCopy k = {{w, 0, 0, 0, w, 0, i * w, 0, w}, (opaqueLast && i == 0) ? 1.0 : 0.05, 1.0 / w};
            c.push_back(k);
        }
        auto c2 = c;
        rnd::RasterizeCopies(s, c.data(), n, 1.0, 1.0, da, 1);
        rnd::RasterizeCopiesReference(s, c2.data(), n, 1.0, 1.0, db, 1);
        float m = std::max({std::fabs(a[0].a - b[0].a), std::fabs(a[0].r - b[0].r), std::fabs(a[0].g - b[0].g),
                            std::fabs(a[0].b - b[0].b)});
        std::printf("  raster_overflow_order opaqueLast=%d: max diff %g\n", opaqueLast, m);
        CHECK(m < 1e-5f);
    }
}

// A worker thread that cannot get its scratch memory stops; the calling thread draws the rest (no
// std::terminate inside AE). The result is the same.
TEST(raster_worker_out_of_memory)
{
    std::vector<Px8> src = MakeSrc8(40, 30), a((size_t)200 * 150), b((size_t)200 * 150);
    rnd::RasterImage s = Img(src, 40, 30, 8), da = Img(a, 200, 150, 8), db = Img(b, 200, 150, 8);
    std::vector<rnd::RasterCopy> c;
    for (int i = 0; i < 30; i++) c.push_back(Copy(0.7, 100 - i, 0.5 + 0.02 * i, -3.0 * i, -2.0 * i));
    auto c2 = c;
    rnd::RasterizeCopies(s, c.data(), (int)c.size(), 1.0, 1.0, da, 4);
    rnd::RasterTestFailWorkerScratch(true);
    bool ok = rnd::RasterizeCopies(s, c2.data(), (int)c2.size(), 1.0, 1.0, db, 4);
    rnd::RasterTestFailWorkerScratch(false);
    CHECK(ok);
    CHECK(MaxDiff8(a, b, 1.f) == 0);
}

TEST(raster_tiled_speed_report)
{
    // like a cut-out PNG: an opaque ellipse on a transparent background
    std::vector<Px8> src = MakeSrc8(577, 680), dst((size_t)1920 * 1080);
    for (int y = 0; y < 680; y++)
        for (int x = 0; x < 577; x++) {
            double u = (x - 288.5) / 260.0, v = (y - 340.0) / 310.0;
            Px8& p = src[(size_t)y * 577 + x];
            if (u * u + v * v > 1) p = Px8{0, 0, 0, 0};
            else p.a = 255;
        }
    rnd::RasterImage s = Img(src, 577, 680, 8), d = Img(dst, 1920, 1080, 8);
    const int hw = (int)std::thread::hardware_concurrency();
    {
        // the part a GPU cannot remove: clearing and writing the image (no copies)
        double best = 1e9;
        for (int rep = 0; rep < 3; rep++) {
            auto t0 = std::chrono::steady_clock::now();
            rnd::RasterizeCopies(s, nullptr, 0, 1.0, 1.0, d, hw);
            best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        std::printf("raster speed: 1920x1080, no copies (clear only): %.1f ms\n", best);
    }
    for (int n : {10, 100, 1000}) {
        auto copies = SceneCopies(7, n, 577, 680, 1500, 60, 100);
        double t[2];
        for (int which = 0; which < 2; which++) {
            double best = 1e9;
            for (int rep = 0; rep < 2; rep++) {
                auto c = copies;
                auto t0 = std::chrono::steady_clock::now();
                if (which == 0)
                    rnd::RasterizeCopiesReference(s, c.data(), (int)c.size(), 1.0, 1.0, d, hw);
                else
                    rnd::RasterizeCopies(s, c.data(), (int)c.size(), 1.0, 1.0, d, hw);
                best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            }
            t[which] = best;
        }
        std::printf("raster speed: 1920x1080, %d copies of 577x680: reference %.1f ms, tiled %.1f ms\n", n, t[0], t[1]);
    }
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

// ---- AE's span drawing (core/ae_span) against captures of AE drawing a copy (RANDAMU_A3D_DRAW=3). The capture
// directories come from RANDAMU_SPAN_CAP (separated by ';'); without it the test only checks the pieces.
static std::vector<unsigned char> ReadAll(const std::string& path)
{
    std::vector<unsigned char> b;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return b;
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
static void SpanCapture(const std::string& dir)
{
    const auto st = ReadAll(dir + "\\state.bin"), pa = ReadAll(dir + "\\params.bin"), sm = ReadAll(dir + "\\samples.bin");
    const auto th = ReadAll(dir + "\\tex_hdr.bin"), tx = ReadAll(dir + "\\tex.bin"), rows = ReadAll(dir + "\\rows.bin");
    CHECK(!st.empty() && !pa.empty() && sm.size() >= 72 && !th.empty() && !tx.empty() && !rows.empty());
    if (st.empty() || pa.empty() || sm.size() < 72 || th.empty() || tx.empty() || rows.empty()) return;
    static rnd::AeRcpTable rcp;
    rnd::AeFillRcpTable(&rcp);
    rnd::AeSpanParams p;
    const int si = At<int>(st, 0x30);
    for (int k = 0; k < 9; k++) p.m[k] = At<double>(sm, 72 * (size_t)si + 8 * k);
    p.persp = st[0x98] != 0, p.a0 = At<double>(st, 0xA0), p.quality = st[0xA8];
    p.opacity = At<double>(pa, 0x20), p.colorMode = pa[0x5B];
    p.rcp = &rcp;
    rnd::AeTex8 t;
    t.data = tx.data(), t.rowbytes = At<int>(th, 40), t.width = At<int>(th, 44), t.height = At<int>(th, 48);
    long long pixels = 0, bad = 0;
    int shown = 0;
    for (size_t o = 0; o + 20 <= rows.size();) {
        const int y = At<int>(rows, o + 4), x0 = At<int>(rows, o + 8), x1 = At<int>(rows, o + 12), n = At<int>(rows, o + 16);
        o += 20;
        if (n <= 0) continue;
        std::vector<unsigned char> buf(rows.begin() + o, rows.begin() + o + n);
        const unsigned char* after = rows.data() + o + n;
        o += 2 * (size_t)n;
        rnd::AeDrawSpan8(p, t, x0, x1, y, buf.data() - 4 * (ptrdiff_t)x0);
        for (int i = 0; i < n / 4; i++) {
            pixels++;
            if (std::memcmp(&buf[4 * i], after + 4 * i, 4)) {
                if (shown++ < 8)
                    std::printf("  span %s: (%d, %d) ours %d %d %d %d AE %d %d %d %d\n", dir.c_str(), x0 + i, y, buf[4 * i],
                                buf[4 * i + 1], buf[4 * i + 2], buf[4 * i + 3], after[4 * i], after[4 * i + 1], after[4 * i + 2],
                                after[4 * i + 3]);
                bad++;
            }
        }
    }
    std::printf("  ae_span %s: %lld pixels, %lld differ (quality %d, persp %d, texture %dx%d)\n", dir.c_str(), pixels, bad,
                p.quality, (int)p.persp, t.width, t.height);
    CHECK(pixels > 0 && bad == 0);
}

TEST(ae_span_pieces)
{
    // a constant texture samples as itself; in-front transfer of opaque / transparent sources
    unsigned char tex[4 * 4 * 4];
    for (int i = 0; i < 16; i++) tex[4 * i] = 255, tex[4 * i + 1] = 10, tex[4 * i + 2] = 20, tex[4 * i + 3] = 30;
    rnd::AeTex8 t;
    t.data = tex, t.rowbytes = 16, t.width = 4, t.height = 4;
    unsigned char o[4];
    rnd::AeSubpixelSample8(t, 0x18000, 0x14000, o);
    CHECK(o[0] == 255 && o[1] == 10 && o[2] == 20 && o[3] == 30);
    rnd::AeAreaSample8(t, 0x20000, 0x20000, 0x10000, 0x10000, o);
    CHECK(o[0] == 255 && o[1] == 10 && o[2] == 20 && o[3] == 30);
    static rnd::AeRcpTable rcp;
    rnd::AeFillRcpTable(&rcp);
    unsigned char d[4] = {255, 1, 2, 3}, s[4] = {255, 9, 8, 7};
    rnd::AeXferInFront8(s, 255, rcp, d);
    CHECK(!std::memcmp(d, s, 4));
    unsigned char s0[4] = {0, 9, 8, 7}, d0[4] = {255, 1, 2, 3};
    rnd::AeXferInFront8(s0, 255, rcp, d0);
    CHECK(d0[1] == 1 && d0[2] == 2 && d0[3] == 3);
    char env[2048] = {0};
    if (GetEnvironmentVariableA("RANDAMU_SPAN_CAP", env, sizeof(env)) > 0) {
        std::string all = env;
        size_t a = 0;
        while (a <= all.size()) {
            const size_t b = all.find(';', a);
            const std::string dir = all.substr(a, b == std::string::npos ? std::string::npos : b - a);
            if (!dir.empty()) SpanCapture(dir);
            if (b == std::string::npos) break;
            a = b + 1;
        }
    }
}

int main()
{
    // never a dialog (it waits for a click nobody sees, and the run looks hung): a C runtime assertion or error
    // goes to stderr and ends the run; a crash prints its code
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* e) -> LONG {
        std::fprintf(stderr, "CRASH in test %s: exception 0x%08lx\n", g_test ? g_test : "?",
                     e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionCode : 0ul);
        std::fflush(stderr);
        return EXCEPTION_EXECUTE_HANDLER;
    });
    if (std::getenv("UNIT_ASSERT_PROBE")) {  // checks the above: a debug assertion must print and end the run
        std::vector<int> v(1);
        volatile size_t i = 5;
        std::printf("probe %d\n", v[i]);
    }
    const bool verbose = std::getenv("UNIT_VERBOSE") != nullptr;  // each test's name before it runs (finding a hang)
    for (int t = 0; t < g_numTests; t++) {
        g_test = g_tests[t].name;
        if (verbose) std::printf("[%s]\n", g_test), std::fflush(stdout);
        g_tests[t].fn();
    }
    if (g_failed == 0)
        std::printf("ALL PASS (%d tests, %d checks)\n", g_numTests, g_checks);
    else
        std::printf("%d FAILED of %d checks\n", g_failed, g_checks);
    return g_failed;
}
