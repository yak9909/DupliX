// Plan P1.5e Task 3: code for the new graph forms (design v5 8.1, 8.2, 8.3, 13): expr(...) { ... }, w.x, the
// math nodes' code names and their disabled wires (_b:), attr() for named attributes, and the comparison.
#include <cstdio>
#include <string>

#include "dxg5/edit.h"
#include "dxg5/eval.h"
#include "dxg5/graph.h"
#include "dxg5/lang.h"
#include "test.h"

using namespace dx5;

namespace {

bool ReadC(const std::string& code, Graph* g, const Graph& prev = Graph(), std::vector<Diag>* errsOut = nullptr)
{
    std::vector<Diag> errs, notes;
    if (CodeToGraph(code, prev, g, &errs, &notes)) return true;
    if (errsOut) *errsOut = errs;
    else
        std::printf("  cannot read: %s (line %d)\n%s\n", errs.empty() ? "?" : errs[0].msg.c_str(), errs.empty() ? 0 : errs[0].pos.line,
                    code.c_str());
    return false;
}

// graph -> code -> graph is the same graph, and the code prints again the same
void GraphRoundTrip(const Graph& a)
{
    const std::string c1 = GraphToCode(a);
    Graph b;
    if (!ReadC(c1, &b, a)) {
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

EvalContext Ctx()
{
    EvalContext c;
    c.macro = [](int k, double) { return k == 0 ? 7.0 : 0.0; };
    c.intMacro = [](int, double) { return (int64_t)0; };
    return c;
}

const Node* FirstOf(const Graph& g, const char* type)
{
    for (const Node& n : g.nodes)
        if (n.type == type) return &n;
    return nullptr;
}

}  // namespace

TEST(parse_expr_block)
{
    Program p;
    Diag d;
    CHECK(ParseProgram("w = expr(k: 3) { x = k * index; y = sin(time) }\ng = points(2)\ng = set_position(g, vec(w.x, w.y, 0))\n", &p, &d));
    CHECK(ParseProgram("w = expr {\n  x = index\n  y = 2\n}\n", &p, &d));
}

TEST(lower_expr_block)
{
    Graph g;
    CHECK(ReadC("w = expr(k: 3) { x = k * index; y = sin(time) }\ng = points(4)\nh = set_position(g, vec(w.x, w.y, 0))\noutput(h)\n", &g));
    const Node* w = g.FindKey("w");
    CHECK(w && w->type == "expr" && w->In("k") && AsFloat(w->In("k")->value) == 3 && !w->In("k")->link.from);
    CHECK(w && OutputsOf(g, *w).size() == 2);
    const auto r = Evaluate(g, Ctx());
    CHECK(r.elements.size() == 4 && r.elements[2].pos[0] == 6);
}

TEST(lower_expr_block_unknown_name_rejected)
{
    Graph g;
    std::vector<Diag> errs;
    CHECK(!ReadC("w = expr { value = indx }\ng = points(2)\noutput(g)\n", &g, Graph(), &errs));
    CHECK(!errs.empty() && errs[0].pos.line == 1 && errs[0].msg.find("indx") != std::string::npos);
}

TEST(lower_expr_block_reads_variables_and_attrs)
{
    Graph g;
    CHECK(ReadC("a = m1 * 2\ng = grid(cx: 3, cy: 1)\nw = expr { value = a + ix }\nh = set_position(g, vec(w, 0, 0))\noutput(h)\n", &g));
    const Node* w = g.FindKey("w");
    CHECK(w && w->In("a") && w->In("a")->link.from != 0);
    CHECK(w && w->text.find("attr(\"ix\")") != std::string::npos);
    const auto r = Evaluate(g, Ctx());
    CHECK(r.elements.size() == 3 && r.elements[2].pos[0] == 16);  // 7 * 2 + 2
}

TEST(lower_inline_named_attr_is_attr_call)
{
    Graph g;
    CHECK(ReadC("g = grid(cx: 3, cy: 1)\nh = set_position(g, vec(ix * 10, 0, 0))\noutput(h)\n", &g));
    const Node* e = g.FindKey("h.position");
    CHECK(e && e->type == "expr" && e->text.find("attr(\"ix\")") != std::string::npos);
    // written back bare (the attribute is known there)
    CHECK(GraphToCode(g).find("vec(ix * 10, 0, 0)") != std::string::npos);
}

TEST(lower_inline_at_becomes_input)
{
    Graph g;
    CHECK(ReadC("g = points(3)\nh = set_position(g, at(g, index - 1).position + vec(0, 10, 0))\noutput(h)\n", &g));
    const Node* e = g.FindKey("h.position");
    CHECK(e && e->type == "expr" && e->inputs.size() == 1);
    const Node* at = e && !e->inputs.empty() ? g.Find(e->inputs[0].link.from) : nullptr;
    CHECK(at && at->type == "at");
}

TEST(print_math_op_call)
{
    Graph g;
    CHECK(ReadC("g = points(2)\nh = set_position(g, vec(math_add(index, 2), 0, 0))\noutput(h)\n", &g));
    const Node* m = FirstOf(g, "math");
    CHECK(m && m->text == "add");
    CHECK(GraphToCode(g).find("math_add(index, 2)") != std::string::npos);
}

TEST(print_disabled_arg_round_trip)
{
    Graph g;
    CHECK(ReadC("g = points(2)\nh = set_position(g, vec(math_add(index, time), 0, 0))\noutput(h)\n", &g));
    Node* m = nullptr;
    for (Node& n : g.nodes)
        if (n.type == "math") m = &n;
    CHECK(m != nullptr);
    if (!m) return;
    std::string why;
    CHECK(SetMathOp(g, m->id, "sin", &why));
    const std::string code = GraphToCode(g);
    CHECK(code.find("math_sin(index, _b: time)") != std::string::npos);
    Graph b;
    CHECK(ReadC(code, &b, g));
    const Node* mb = FirstOf(b, "math");
    CHECK(mb && mb->text == "sin" && mb->In("b")->link.from != 0 && !LinkActive(b, *mb, 1));
    GraphRoundTrip(g);
}

TEST(print_expr_block_form)
{
    Graph g;
    CHECK(ReadC("g = points(4)\nh = set_position(g, vec(index, 0, 0))\noutput(h)\n", &g));
    const uint32_t e = AddNode(g, "expr", "w", 0, 0);
    std::string why;
    CHECK(SetFormula(g, e, "x = k * index\ny = 2", &why));
    g.Find(e)->In("k")->value = Value::F(3);
    RemoveNode(g, g.FindKey("h.position")->id);
    const uint32_t v = AddNode(g, "math_vec", "h.position", 0, 0);
    g.FindKey("h")->In("position")->link = {v, 0};
    g.Find(v)->In("a")->link = {e, 0};
    g.Find(v)->In("b")->link = {e, 1};
    const std::string code = GraphToCode(g);
    CHECK(code.find("w = expr(k: 3) {") != std::string::npos);
    CHECK(code.find("w.x") != std::string::npos && code.find("w.y") != std::string::npos);
    GraphRoundTrip(g);
}

TEST(print_renames_value_key)
{
    // an Expression node with an unwired input, keyed like an inline one (not an identifier)
    Graph g;
    CHECK(ReadC("g = points(4)\nh = set_position(g, vec(index, 0, 0))\noutput(h)\n", &g));
    const Node* old = g.FindKey("h.position");
    CHECK(old != nullptr);
    if (!old) return;
    const uint32_t e = old->id;
    std::string why;
    CHECK(SetFormula(g, e, "vec(index * k, 0, 0)", &why));
    const std::string code = GraphToCode(g);
    CHECK(code.find("h_position = expr(k: 0) {") != std::string::npos);
    GraphRoundTrip(g);
}

TEST(same_graph_folds_input_nodes)
{
    Graph a, b;
    CHECK(ReadC("g = points(2)\nh = set_position(g, vec(m1 * 2, 0, 0))\noutput(h)\n", &a));
    CHECK(ReadC("g = points(2)\nh = set_position(g, vec(m1 * 2, 0, 0))\noutput(h)\n", &b));
    // in b, the macro comes into the formula through an input
    Node* e = b.FindKey("h.position");
    CHECK(e != nullptr);
    if (!e) return;
    const uint32_t eid = e->id;
    std::string why;
    CHECK(SetFormula(b, eid, "vec(a * 2, 0, 0)", &why));
    std::vector<Diag> errs, notes;
    const uint32_t m = AddNode(b, "macro", "m1", 0, 0);
    b.Find(m)->setting = 1;
    b.Find(eid)->In("a")->link = {m, 0};
    CHECK(SameGraph(a, b, &why));
    if (!why.empty()) std::printf("  %s\n", why.c_str());
}

TEST(same_graph_ignores_value_keys_compares_salt)
{
    Graph a, b;
    CHECK(ReadC("g = points(2)\nh = set_position(g, vec(math_add(index, 1), 0, 0))\noutput(h)\n", &a));
    b = a;
    for (Node& n : b.nodes)
        if (n.type == "math") n.key = "renamed";
    std::string why;
    CHECK(SameGraph(a, b, &why));
    Graph c, d;
    CHECK(ReadC("g = points(2)\nr = random(salt: 0x10)\ng = set_opacity(g, r)\noutput(g)\n", &c));
    CHECK(ReadC("g = points(2)\nr = random(salt: 0x11)\ng = set_opacity(g, r)\noutput(g)\n", &d));
    CHECK(!SameGraph(c, d, &why));
}

TEST(round_trip_new_forms)
{
    const char* codes[] = {
        "w = expr(k: 3) {\n  x = k * index\n  y = sin(time)\n}\ng = points(4)\nh = set_position(g, vec(w.x, w.y, 0))\noutput(h)\n",
        "g = grid(cx: 3, cy: 1)\nh = set_position(g, vec(ix * 10, compare_lt(index, 1), 0))\noutput(h)\n",
        "g = points(3)\nh = set_position(g, vec(math_mul(index, 2), 0, 0))\noutput(h)\n",
    };
    for (const char* c : codes) {
        Graph a;
        if (!ReadC(c, &a)) {
            CHECK(false);
            continue;
        }
        GraphRoundTrip(a);
    }
}
