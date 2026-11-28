// DupliX 2 (design v5): the test harness of the dx5 core (tests/dxg5/*.cpp register their tests here).
#pragma once

#include <cmath>
#include <cstdio>

extern int g_failed;
extern int g_checks;
extern const char* g_test;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failed;                                                      \
            std::printf("FAIL %s: %s (%s:%d)\n", g_test, #cond, __FILE__, __LINE__); \
        }                                                                    \
    } while (0)
#define CHECK_NEAR(a, b, eps)                                                                                      \
    do {                                                                                                           \
        ++g_checks;                                                                                                \
        const double va = (a), vb = (b);                                                                           \
        if (!(std::fabs(va - vb) <= (eps))) {                                                                      \
            ++g_failed;                                                                                            \
            std::printf("FAIL %s: %s = %.9g, want %.9g (%s:%d)\n", g_test, #a, va, vb, __FILE__, __LINE__);        \
        }                                                                                                          \
    } while (0)

using TestFn = void (*)();
void RegisterTest(const char* name, TestFn fn);
#define TEST(name)                                         \
    static void name();                                    \
    namespace {                                            \
    struct name##_reg {                                    \
        name##_reg() { RegisterTest(#name, name); }        \
    } name##_inst;                                         \
    }                                                      \
    static void name()
