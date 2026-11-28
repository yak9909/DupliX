// Plan P1.5e Task 6a: opening an Expression node or a group and editing inside (design v5 8.3, 12.1).
#include <cstdio>
#include <string>

#include "dxg5/edit.h"
#include "dxg5/eval.h"
#include "dxg5/lang.h"
#include "test.h"

using namespace dx5;

namespace {

EvalContext CtxI()
{
    EvalContext c;
    c.macro = [](int k, double) { return k == 1 ? 5.0 : 0.0; };
    c.intMacro = [](int, double) { return (int64_t)0; };
    return c;
}

bool ReadI(const std::string& code, Graph* g)
{
    std::vector<Diag> errs, notes;
    if (ApplyCode(*g, code, &errs, &notes)) return true;
    std::printf("  cannot read: %s\n", errs.empty() ? "?" : errs[0].msg.c_str());
    return false;
}

int Count(const Graph& g, const char* type)
{
    int k = 0;
    for (const Node& n : g.nodes) k += n.type == type;
    return k;
}

}  // namespace

TEST(open_expression_shows_nodes)
{
    Graph g;
    CHECK(ReadI("g = points(3)\nw = expr(k: 3) { x = k * index + m2; y = sin(time) }\nh = set_position(g, vec(w.x, w.y, 0))\noutput(h)\n", &g));
    const Node* w = g.FindKey("w");
    CHECK(w != nullptr);
    if (!w) return;
    Graph in;
    CHECK(OpenExpression(g, w->id, &in));
    CHECK(Count(in, "param") == 1 && Count(in, "group_output") == 1);
    const Node* ret = nullptr;
    for (const Node& n : in.nodes)
        if (n.type == "group_output") ret = &n;
    CHECK(ret && ret->inputs.size() == 2 && ret->inputs[0].name == "x" && ret->inputs[1].name == "y");
    int math = 0;
    for (const Node& n : in.nodes) math += IsMathFamily(n.type);
    CHECK(math >= 3);  // mul, add, sin
}

TEST(close_expression_round_trip)
{
    Graph g;
    CHECK(ReadI("g = points(3)\nw = expr(k: 3) { x = k * index + m2; y = sin(time) }\nh = set_position(g, vec(w.x, w.y, 0))\noutput(h)\n", &g));
    const auto before = Evaluate(g, CtxI());
    const uint32_t w = g.FindKey("w")->id;
    Graph in;
    CHECK(OpenExpression(g, w, &in));
    std::string why;
    CHECK(CloseExpression(g, w, in, &why));
    if (!why.empty()) std::printf("  %s\n", why.c_str());
    const Node* n = g.Find(w);
    CHECK(n && OutputsOf(g, *n).size() == 2 && n->In("k") && AsFloat(n->In("k")->value) == 3);
    const auto after = Evaluate(g, CtxI());
    CHECK(after.elements.size() == 3 && after.elements[2].pos[0] == before.elements[2].pos[0] &&
          after.elements[2].pos[1] == before.elements[2].pos[1]);
}

TEST(close_expression_after_edit)
{
    Graph g;
    CHECK(ReadI("g = points(3)\nh = set_position(g, vec(index * 10, 0, 0))\noutput(h)\n", &g));
    const uint32_t e = g.FindKey("h.position")->id;
    Graph in;
    CHECK(OpenExpression(g, e, &in));
    // inside: the mul becomes an add
    for (Node& n : in.nodes)
        if (n.type == "math" && n.text == "mul") n.text = "add";
    std::string why;
    CHECK(CloseExpression(g, e, in, &why));
    CHECK(g.Find(e)->text.find("index + 10") != std::string::npos);
    const auto r = Evaluate(g, CtxI());
    CHECK(r.elements.size() == 3 && r.elements[2].pos[0] == 12);
}

TEST(set_group_body)
{
    Graph g;
    CHECK(ReadI("group spread(geometry: geometry, k = 2) {\n  h = set_position(geometry, position * k)\n  return h\n}\n"
                "g = grid(cx: 3, cy: 1)\nh = spread(g, k: 3)\noutput(h)\n",
                &g));
    const UserGroup* u = FindUserGroup(g, "spread");
    CHECK(u != nullptr);
    if (!u) return;
    Graph body = *u->body;
    // inside: the scale becomes position * k * 2
    std::vector<Diag> errs, notes;
    Node* e = nullptr;
    for (Node& n : body.nodes)
        if (n.type == "expr") e = &n;
    CHECK(e != nullptr);
    if (!e) return;
    std::string why;
    CHECK(SetFormula(body, e->id, e->text + " * 2", &why));
    CHECK(SetGroupBody(g, "spread", body, &why));
    const auto r = Evaluate(g, CtxI());
    CHECK(r.elements.size() == 3 && r.elements[2].pos[0] == 600);
    // a built-in group cannot be changed
    Graph none;
    CHECK(!SetGroupBody(g, "grid", none, &why));
}
