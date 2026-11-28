// Task 3: evaluation (geometry nodes, fields evaluated over the geometry of the node they feed, ids).
#include <chrono>
#include <cmath>
#include <string>

#include "dxg5/eval.h"
#include "dxg5/graph.h"
#include "graph/eval.h"
#include "test.h"

using namespace dx5;

namespace {

struct B {
    Graph g;
    uint32_t Add(const char* type, const char* key = "") { return AddNode(g, type, key, 0, 0); }
    void Wire(uint32_t from, uint32_t to, const char* input, int output = 0) { g.Find(to)->In(input)->link = {from, output}; }
    void Set(uint32_t n, const char* input, Value v) { g.Find(n)->In(input)->value = v; }
};

EvalContext Ctx()
{
    EvalContext c;
    c.macro = [](int, double) { return 0.0; };
    c.intMacro = [](int, double) { return (int64_t)0; };
    return c;
}

// points(n) -> set_position(position + vec(0, sin(index * 0.3) * 20, 0)) -> output
B SinGraph(int n)
{
    B b;
    const uint32_t p = b.Add("points", "pts");
    b.Set(p, "count", Value::I(n));
    const uint32_t idx = b.Add("index"), k = b.Add("math_mul"), s = b.Add("math_sin"), a = b.Add("math_mul");
    b.Wire(idx, k, "a");
    b.Set(k, "b", Value::F(0.3));
    b.Wire(k, s, "a");
    b.Wire(s, a, "a");
    b.Set(a, "b", Value::F(20));
    const uint32_t v = b.Add("math_vec");
    b.Wire(a, v, "b");
    const uint32_t pos = b.Add("position"), add = b.Add("math_add");
    b.Wire(pos, add, "a");
    b.Wire(v, add, "b");
    const uint32_t sp = b.Add("set_position");
    b.Wire(p, sp, "geometry");
    b.Wire(add, sp, "position");
    const uint32_t o = b.Add("output", "out");
    b.Wire(sp, o, "geometry");
    return b;
}

}  // namespace

TEST(field_sin_index)
{
    B b = SinGraph(5);
    const dxg::EvalResult r = Evaluate(b.g, Ctx());
    CHECK(r.hasOutput);
    CHECK(r.elements.size() == 5);
    if (r.elements.size() == 5) {
        CHECK_NEAR(r.elements[3].pos[1], std::sin(0.9) * 20, 1e-12);
        CHECK_NEAR(r.elements[3].pos[0], 0, 0);
        CHECK(r.elements[3].index == 3);
    }
    CHECK_NEAR(r.copyScale, 1, 0);
}

TEST(field_context_differs)
{
    // one Index node feeds Set Opacity on a list of 3 and on a list of 5 (merged through the Output of each run)
    B b;
    const uint32_t p3 = b.Add("points", "a"), p5 = b.Add("points", "b");
    b.Set(p3, "count", Value::I(3));
    b.Set(p5, "count", Value::I(5));
    const uint32_t idx = b.Add("index"), div = b.Add("math_div");
    b.Wire(idx, div, "a");
    b.Set(div, "b", Value::F(10));
    const uint32_t s3 = b.Add("set_opacity"), s5 = b.Add("set_opacity");
    b.Wire(p3, s3, "geometry");
    b.Wire(p5, s5, "geometry");
    b.Wire(div, s3, "opacity");
    b.Wire(div, s5, "opacity");
    const uint32_t o = b.Add("output");
    b.Wire(s5, o, "geometry");
    dxg::EvalResult r = Evaluate(b.g, Ctx());
    CHECK(r.elements.size() == 5);
    if (r.elements.size() == 5) CHECK_NEAR(r.elements[4].opacity, 0.4, 1e-12);
    b.Wire(s3, o, "geometry");
    r = Evaluate(b.g, Ctx());
    CHECK(r.elements.size() == 3);
    if (r.elements.size() == 3) CHECK_NEAR(r.elements[2].opacity, 0.2, 1e-12);
}

TEST(where_selects)
{
    B b;
    const uint32_t p = b.Add("points");
    b.Set(p, "count", Value::I(4));
    const uint32_t idx = b.Add("index"), mod = b.Add("math_mod"), eq = b.Add("math_eq");
    b.Wire(idx, mod, "a");
    b.Set(mod, "b", Value::I(2));
    b.Wire(mod, eq, "a");
    b.Set(eq, "b", Value::I(0));
    const uint32_t sp = b.Add("set_position");
    b.Wire(p, sp, "geometry");
    b.Set(sp, "position", Value::V({7, 0, 0}));
    b.Wire(eq, sp, "where");
    const uint32_t o = b.Add("output");
    b.Wire(sp, o, "geometry");
    const dxg::EvalResult r = Evaluate(b.g, Ctx());
    CHECK(r.elements.size() == 4);
    if (r.elements.size() == 4) {
        CHECK(r.elements[0].pos[0] == 7 && r.elements[1].pos[0] == 0 && r.elements[2].pos[0] == 7 && r.elements[3].pos[0] == 0);
    }
}

TEST(points_cap)
{
    B b;
    const uint32_t p = b.Add("points");
    const uint32_t m = b.Add("macro", "m1");
    b.g.Find(m)->setting = 1;
    b.Wire(m, p, "count");
    const uint32_t o = b.Add("output");
    b.Wire(p, o, "geometry");
    EvalContext c = Ctx();
    c.macro = [](int, double) { return 1e9; };
    const auto t0 = std::chrono::steady_clock::now();
    const dxg::EvalResult r = Evaluate(b.g, c);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r.elements.size() == 100000);
    bool warned = false;
    for (const std::string& w : r.warnings) warned |= w == "cut to 100000 elements";
    CHECK(warned);
    CHECK(ms < 1000);
}

TEST(ids_from_key)
{
    B a, b, c;
    for (B* x : {&a, &b, &c}) {
        const uint32_t p = x->Add("points", x == &b ? "other" : "same");
        x->Set(p, "count", Value::I(3));
        const uint32_t o = x->Add("output");
        x->Wire(p, o, "geometry");
    }
    const auto ra = Evaluate(a.g, Ctx()), rb = Evaluate(b.g, Ctx()), rc = Evaluate(c.g, Ctx());
    CHECK(ra.elements.size() == 3 && rb.elements.size() == 3 && rc.elements.size() == 3);
    CHECK(ra.elements[1].id == rc.elements[1].id);
    CHECK(ra.elements[1].id != rb.elements[1].id);
    CHECK(ra.elements[0].id != ra.elements[1].id);
    CHECK(ra.elements[1].id == dxg::MixId(HashStr("same"), 1));
}

TEST(legacy_ids)
{
    B b;
    const uint32_t p = b.Add("points");
    b.g.Find(p)->legacyId = 3;
    b.Set(p, "count", Value::I(2));
    const uint32_t o = b.Add("output");
    b.Wire(p, o, "geometry");
    const auto r = Evaluate(b.g, Ctx());
    CHECK(r.elements.size() == 2 && r.elements[1].id == dxg::MixId(3, 1));
}

TEST(accumulate_and_at)
{
    B b;
    const uint32_t p = b.Add("points");
    b.Set(p, "count", Value::I(4));
    // x = accumulate(1) (1, 2, 3, 4)
    const uint32_t acc = b.Add("accumulate");
    b.Set(acc, "value", Value::F(1));
    const uint32_t v = b.Add("math_vec");
    b.Wire(acc, v, "a");
    const uint32_t s1 = b.Add("set_position");
    b.Wire(p, s1, "geometry");
    b.Wire(v, s1, "position");
    // y = at(s1, index - 1).position.x  (the one before; the first reads itself)
    const uint32_t idx = b.Add("index"), sub = b.Add("math_sub");
    b.Wire(idx, sub, "a");
    b.Set(sub, "b", Value::I(1));
    const uint32_t at = b.Add("at"), pos = b.Add("position"), sx = b.Add("math_sep_x");
    b.Wire(s1, at, "geometry");
    b.Wire(sub, at, "index");
    b.Wire(pos, at, "value");
    b.Wire(at, sx, "a");
    const uint32_t pos2 = b.Add("position"), x2 = b.Add("math_sep_x"), v2 = b.Add("math_vec");
    b.Wire(pos2, x2, "a");
    b.Wire(x2, v2, "a");
    b.Wire(sx, v2, "b");
    const uint32_t s2 = b.Add("set_position");
    b.Wire(s1, s2, "geometry");
    b.Wire(v2, s2, "position");
    const uint32_t o = b.Add("output");
    b.Wire(s2, o, "geometry");
    const auto r = Evaluate(b.g, Ctx());
    CHECK(r.elements.size() == 4);
    if (r.elements.size() == 4) {
        for (int k = 0; k < 4; k++) CHECK_NEAR(r.elements[k].pos[0], k + 1, 0);
        CHECK_NEAR(r.elements[0].pos[1], 1, 0);  // clamped to the first
        CHECK_NEAR(r.elements[1].pos[1], 1, 0);
        CHECK_NEAR(r.elements[3].pos[1], 3, 0);
    }
}

TEST(capture_rides_along)
{
    B b;
    const uint32_t p = b.Add("points");
    b.Set(p, "count", Value::I(4));
    // position.x = index; capture it; move all to 0; delete element 0; store the capture as x again
    const uint32_t idx = b.Add("index"), v = b.Add("math_vec");
    b.Wire(idx, v, "a");
    const uint32_t s1 = b.Add("set_position");
    b.Wire(p, s1, "geometry");
    b.Wire(v, s1, "position");
    const uint32_t cap = b.Add("capture", "old"), pos = b.Add("position");
    b.Wire(s1, cap, "geometry");
    b.Wire(pos, cap, "value");
    const uint32_t s2 = b.Add("set_position");
    b.Wire(cap, s2, "geometry");
    b.Set(s2, "position", Value::V({0, 0, 0}));
    const uint32_t idx2 = b.Add("index"), eq = b.Add("math_eq"), del = b.Add("delete");
    b.Wire(idx2, eq, "a");
    b.Set(eq, "b", Value::I(0));
    b.Wire(s2, del, "geometry");
    b.Wire(eq, del, "where");
    const uint32_t s3 = b.Add("set_position");
    b.Wire(del, s3, "geometry");
    b.Wire(cap, s3, "position", 1);
    const uint32_t o = b.Add("output");
    b.Wire(s3, o, "geometry");
    const auto r = Evaluate(b.g, Ctx());
    CHECK(r.elements.size() == 3);
    if (r.elements.size() == 3) {
        CHECK_NEAR(r.elements[0].pos[0], 1, 0);
        CHECK_NEAR(r.elements[2].pos[0], 3, 0);
    }
}
