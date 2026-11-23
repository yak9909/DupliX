// randamu unit tests (no framework). Exit code = number of failed checks.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/scatter.h"
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
