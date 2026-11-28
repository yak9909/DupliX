// Task 6: graph -> code, and the round trips (design v5 8.2, 13).
#include <string>

#include "dxg5/eval.h"
#include "dxg5/graph.h"
#include "dxg5/lang.h"
#include "graph/edit.h"
#include "graph/graph.h"
#include "test.h"

using namespace dx5;

namespace {

bool Read(const std::string& code, Graph* g, const Graph& prev = Graph())
{
    std::vector<Diag> errs, notes;
    if (CodeToGraph(code, prev, g, &errs, &notes)) return true;
    std::printf("  cannot read: %s (line %d)\n%s\n", errs.empty() ? "?" : errs[0].msg.c_str(), errs.empty() ? 0 : errs[0].pos.line,
                code.c_str());
    return false;
}

// code -> graph -> code -> graph: the graphs are the same and the second code is the first
void RoundTrip(const std::string& code)
{
    Graph a, b;
    if (!Read(code, &a)) {
        CHECK(false);
        return;
    }
    const std::string c1 = GraphToCode(a);
    if (!Read(c1, &b, a)) {
        CHECK(false);
        return;
    }
    std::string why;
    const bool same = SameGraph(a, b, &why);
    if (!same) std::printf("  not the same: %s\n--- code:\n%s\n", why.c_str(), c1.c_str());
    CHECK(same);
    const std::string c2 = GraphToCode(b);
    if (c2 != c1) std::printf("--- first:\n%s--- second:\n%s", c1.c_str(), c2.c_str());
    CHECK(c2 == c1);
}

const char* kCodes[] = {
    "g = points(5)\ng = set_position(g, position + vec(0, sin(index * 0.3) * 20, 0))\noutput(g)\n",
    "g = points(i1)\ng = set_position(g, vec(index * m2, 6 * 2, 0))\noutput(g, copy_scale: 50)\n",
    "g = points(3)\ng = set_position(g, vec(index, 0, 0))\nb = g.position\ng = set_position(g, vec(0, 0, 0))\n"
    "g = store(g, \"old\", b)\ng = set_position(g, old)\noutput(g)\n",
    "p = (e) => e.index * 10\ng = points(3)\ng = set_position(g, vec(p, 0, 0))\n"
    "h = set_position(g, vec(position.x, at(g, index - 1).position.x, p))\noutput(h)\n",
    "g = points(3)\ng = set_scale(g, vec(0.8, 0.8, 0.8), where: index == 0)\nh = each g {\n"
    "  if scale.x < 1 {\n    scale *= 2\n    position += vec(1, 0, 0)\n  } else {\n    opacity = 0.5\n  }\n  old = position\n"
    "  position = vec(9, 0, 0)\n  store(\"was\", old)\n}\nk = set_position(h, was)\noutput(k)\n",
    "g = points(4)\nr = random(salt: 0x3fa1)\ng = set_opacity(g, r)\ng = set_position(g, vec(r * 100, accumulate(1), 0))\n"
    "c = bounds(g, position).center\ng = set_position(g, position - c)\noutput(g)\n",
    "g = points(2)\ng_b, b = capture(g, index * 3)\nh = set_position(g_b, vec(b, 0, 0))\nh = set_position(h, position + math_add(vec(1, 0, 0), position))\noutput(h)\n",
};

}  // namespace

TEST(print_parse_roundtrip)
{
    for (const char* c : kCodes) RoundTrip(c);
}

TEST(print_v1_default_preset)
{
    Graph g;
    std::string err;
    dxg::Graph v1 = dxg::DefaultPreset().graph;
    CHECK(Parse(dxg::Serialize(v1), &g, &err));
    const std::string code = GraphToCode(g);
    CHECK(code.find("grid1 = grid(cx: m1 /*Count*/, cy: m1 /*Count*/, sx: m2 /*Spacing*/, sy: m2 /*Spacing*/)") != std::string::npos);
    CHECK(code.find("output(grid1, copy_scale: m3 /*Copy Scale*/)") != std::string::npos);
    Graph h;
    CHECK(Read(code, &h, g));
    std::string why;
    CHECK(SameGraph(g, h, &why));
    if (!why.empty()) std::printf("  %s\n", why.c_str());
    CHECK(h.FindKey("grid1") && h.FindKey("grid1")->legacyId == 1);
    // v1 expressions print as formulas
    Graph e;
    CHECK(Parse("DupliX graph 1\nnextid 3\nnode 1 Grid 0 0\nparam 1 spacingX 10 m2 2\nnode 2 Output 0 0\nwire 1 2 0\n", &e, &err));
    const std::string ec = GraphToCode(e);
    CHECK(ec.find("sx: 10 + 2 * m2") != std::string::npos);
}

TEST(expression_vs_math)
{
    Graph a;
    CHECK(Read("g = points(2)\ng = set_position(g, math_add(position, vec(1, 0, 0)))\nh = set_position(g, position + vec(1, 0, 0))\noutput(h)\n", &a));
    const Node* m = a.FindKey("g_2.position");
    CHECK(m && m->type == "math_add");
    const Node* e = a.FindKey("h.position");
    CHECK(e && e->type == "expr");
    const std::string code = GraphToCode(a);
    CHECK(code.find("position: math_add(position, vec(1, 0, 0))") != std::string::npos);
    CHECK(code.find("set_position(g_2, position: position + vec(1, 0, 0))") != std::string::npos);
}

TEST(chained_expressions_normalized)
{
    Graph a;
    CHECK(Read("g = points(2)\ng = set_position(g, vec(index * 2, 0, 0))\noutput(g)\n", &a));
    // split the formula in two Expression nodes by hand: x = index * 2, then vec(x, 0, 0)
    Graph b = a;
    Node* e = b.FindKey("g_2.position");
    CHECK(e != nullptr);
    if (!e) return;
    const uint32_t inner = AddNode(b, "expr", "g_2.position.x", 0, 0);
    b.Find(inner)->text = "index * 2";
    e = b.FindKey("g_2.position");
    e->text = "vec(x, 0, 0)";
    InputSlot s;
    s.name = "x";
    s.link = {inner, 0};
    e->inputs.push_back(s);
    std::string why;
    CHECK(SameGraph(a, b, &why));
    if (!why.empty()) std::printf("  %s\n", why.c_str());
    CHECK(ExprText(b, e->id, 0) == "vec(index * 2, 0, 0)");
}
