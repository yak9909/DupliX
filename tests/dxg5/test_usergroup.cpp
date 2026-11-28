// Plan P1.5e Task 4: the user's groups (design v5 12.1): defined in code as functions, saved in the graph,
// evaluated by their definition.
#include <cstdio>
#include <string>

#include "dxg5/edit.h"
#include "dxg5/eval.h"
#include "dxg5/graph.h"
#include "dxg5/lang.h"
#include "test.h"

using namespace dx5;

namespace {

bool ReadU(const std::string& code, Graph* g, std::vector<Diag>* errsOut = nullptr, const Graph& prev = Graph())
{
    std::vector<Diag> errs, notes;
    if (CodeToGraph(code, prev, g, &errs, &notes)) return true;
    if (errsOut) *errsOut = errs;
    else
        std::printf("  cannot read: %s (line %d)\n%s\n", errs.empty() ? "?" : errs[0].msg.c_str(), errs.empty() ? 0 : errs[0].pos.line,
                    code.c_str());
    return false;
}

EvalContext Ctx()
{
    EvalContext c;
    c.macro = [](int, double) { return 0.0; };
    c.intMacro = [](int, double) { return (int64_t)0; };
    return c;
}

const char* kSpread =
    "group spread(geometry: geometry, k = 2) {\n  h = set_position(geometry, position * k)\n  return h\n}\n"
    "g = grid(cx: 3, cy: 1)\nh = spread(g, k: 3)\noutput(h)\n";

}  // namespace

TEST(group_defined_in_code)
{
    Graph g;
    CHECK(ReadU(kSpread, &g));
    CHECK(g.groups.size() == 1 && g.groups[0].name == "spread");
    const Node* h = g.FindKey("h");
    CHECK(h && h->type == "spread" && h->In("k") && AsFloat(h->In("k")->value) == 3);
    const auto r = Evaluate(g, Ctx());
    CHECK(r.elements.size() == 3 && r.elements[0].pos[0] == -300 && r.elements[2].pos[0] == 300);
}

TEST(group_two_outputs)
{
    Graph g;
    CHECK(ReadU("group lay(n: int = 3) {\n  g = grid(cx: n, cy: 1)\n  return (geometry: g, center: bounds(g, position).max)\n}\n"
                "r = lay(n: 2)\nh = set_position(r.geometry, position + r.center)\noutput(h)\n",
                &g));
    CHECK(g.groups.size() == 1 && g.groups[0].def.outputs.size() == 2 && g.groups[0].def.outputs[1].name == "center");
    const auto e = Evaluate(g, Ctx());
    CHECK(e.elements.size() == 2 && e.elements[0].pos[0] == 0 && e.elements[1].pos[0] == 100);  // -50 + 50, 50 + 50
}

TEST(group_recursion_rejected)
{
    Graph g;
    std::vector<Diag> errs;
    CHECK(!ReadU("group a() {\n  return b()\n}\ngroup b() {\n  return a()\n}\ng = a()\noutput(g)\n", &g, &errs));
    CHECK(!errs.empty());
    CHECK(!ReadU("group a() {\n  return a()\n}\ng = a()\noutput(g)\n", &g, &errs));
    CHECK(!errs.empty() && errs[0].msg.find("itself") != std::string::npos);
}

TEST(group_name_clash_rejected)
{
    Graph g;
    std::vector<Diag> errs;
    CHECK(!ReadU("group grid() {\n  return points(2)\n}\ng = grid()\noutput(g)\n", &g, &errs));
    CHECK(!errs.empty() && errs[0].pos.line == 1);
    CHECK(!ReadU("group points() {\n  return grid()\n}\ng = points()\noutput(g)\n", &g, &errs));
}

TEST(group_outer_variable_rejected)
{
    Graph g;
    std::vector<Diag> errs;
    CHECK(!ReadU("g0 = points(2)\ngroup bad() {\n  return set_position(g0, vec(1, 0, 0))\n}\nh = bad()\noutput(h)\n", &g, &errs));
    CHECK(!errs.empty() && errs[0].msg.find("g0") != std::string::npos);
}

TEST(group_structural_auto)
{
    Graph g;
    CHECK(ReadU("group row(count = 4) {\n  p = points(count * 2)\n  return p\n}\ng = row(count: 2)\noutput(g)\n", &g));
    CHECK(g.groups.size() == 1);
    if (g.groups.empty()) return;
    const NodeDef& d = g.groups[0].def;
    CHECK(d.Input("count") >= 0 && d.inputs[d.Input("count")].structural);
    CHECK(GraphToCode(g).find("count: int structural = 4") != std::string::npos ||
          GraphToCode(g).find("count: float structural = 4") != std::string::npos);
    CHECK(Evaluate(g, Ctx()).elements.size() == 4);
}

TEST(group_saved_and_read)
{
    Graph g;
    CHECK(ReadU(kSpread, &g));
    const std::string s = Serialize(g);
    CHECK(s.find("group spread 0") != std::string::npos && s.find("\nend\n") != std::string::npos);
    Graph back;
    CHECK(Parse(s, &back));
    std::string why;
    CHECK(SameGraph(g, back, &why));
    if (!why.empty()) std::printf("  %s\n", why.c_str());
    const auto r = Evaluate(back, Ctx());
    CHECK(r.elements.size() == 3 && r.elements[2].pos[0] == 300);
}

TEST(group_round_trip)
{
    Graph a;
    CHECK(ReadU(kSpread, &a));
    const std::string c1 = GraphToCode(a);
    Graph b;
    CHECK(ReadU(c1, &b, nullptr, a));
    std::string why;
    CHECK(SameGraph(a, b, &why));
    if (!why.empty()) std::printf("  %s\n%s\n", why.c_str(), c1.c_str());
    const std::string c2 = GraphToCode(b);
    if (c2 != c1) std::printf("--- first:\n%s--- second:\n%s", c1.c_str(), c2.c_str());
    CHECK(c2 == c1);
}

TEST(group_ids_from_caller_key)
{
    // the same group called twice makes different ids (hash of the calling node's key and the inner key)
    Graph g;
    CHECK(ReadU("group two() {\n  p = points(2)\n  return p\n}\na = two()\nb = two()\nc = set_position(a, vec(0, 0, 0))\noutput(c)\n", &g));
    Graph h;
    CHECK(ReadU("group two() {\n  p = points(2)\n  return p\n}\na = two()\nb = two()\nc = set_position(b, vec(0, 0, 0))\noutput(c)\n", &h));
    const auto ra = Evaluate(g, Ctx()), rb = Evaluate(h, Ctx());
    CHECK(ra.elements.size() == 2 && rb.elements.size() == 2 && ra.elements[0].id != rb.elements[0].id);
}
