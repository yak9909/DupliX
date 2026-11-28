// Plan P1.5e Task 2: the Expression node's formula (design v5 8.3): outputs and inputs from its lines, applied on
// commit, wires kept by name, Random's salt written back.
#include <cmath>
#include <string>

#include "dxg5/edit.h"
#include "dxg5/eval.h"
#include "dxg5/graph.h"
#include "dxg5/lang.h"
#include "test.h"

using namespace dx5;

namespace {

EvalContext Ctx()
{
    EvalContext c;
    c.macro = [](int k, double) { return k == 0 ? 7.0 : 0.0; };
    c.intMacro = [](int, double) { return (int64_t)0; };
    return c;
}

// points(4) -> set_position(vec(<x>, <y>, 0)) -> output, with an Expression node `e`
struct FGraph {
    Graph g;
    uint32_t pts = 0, vec = 0, e = 0, sp = 0;
    FGraph()
    {
        pts = AddNode(g, "points", "pts", 0, 0);
        g.Find(pts)->In("count")->value = Value::I(4);
        vec = AddNode(g, "math_vec", "v", 0, 0);
        sp = AddNode(g, "set_position", "sp", 0, 0);
        g.Find(sp)->In("geometry")->link = {pts, 0};
        g.Find(sp)->In("position")->link = {vec, 0};
        const uint32_t o = AddNode(g, "output", "out", 0, 0);
        g.Find(o)->In("geometry")->link = {sp, 0};
        e = AddNode(g, "expr", "e", 0, 0);
    }
    const dxg::Element* El(int k)
    {
        static dxg::EvalResult r;
        r = Evaluate(g, Ctx());
        return k < (int)r.elements.size() ? &r.elements[k] : nullptr;
    }
    std::vector<std::string> Ins() const
    {
        std::vector<std::string> s;
        for (const InputSlot& i : g.Find(e)->inputs) s.push_back(i.name);
        return s;
    }
};

std::vector<std::string> Names(const std::vector<OutputDef>& o)
{
    std::vector<std::string> s;
    for (const OutputDef& d : o) s.push_back(d.name);
    return s;
}

}  // namespace

TEST(formula_two_outputs)
{
    FGraph f;
    std::string why;
    CHECK(SetFormula(f.g, f.e, "x = index * 2\ny = sin(time)", &why));
    CHECK(Names(OutputsOf(f.g, *f.g.Find(f.e))) == std::vector<std::string>({"x", "y"}));
    f.g.Find(f.vec)->In("a")->link = {f.e, 0};
    f.g.Find(f.vec)->In("b")->link = {f.e, 1};
    const dxg::Element* el = f.El(3);
    CHECK(el && el->pos[0] == 6 && el->pos[1] == 0);
    // the lines may also be split by ;
    CHECK(SetFormula(f.g, f.e, "x = index; y = x * 3", &why));
    el = f.El(3);
    CHECK(el && el->pos[0] == 3 && el->pos[1] == 9);
}

TEST(formula_private_line)
{
    FGraph f;
    std::string why;
    CHECK(SetFormula(f.g, f.e, "_d = index + 1\nvalue = _d * 2", &why));
    CHECK(Names(OutputsOf(f.g, *f.g.Find(f.e))) == std::vector<std::string>({"value"}));
    CHECK(f.Ins().empty());
    f.g.Find(f.vec)->In("a")->link = {f.e, 0};
    CHECK(f.El(2) && f.El(2)->pos[0] == 6);
}

TEST(formula_unknown_name_is_input)
{
    FGraph f;
    std::string why;
    CHECK(SetFormula(f.g, f.e, "k * indx", &why));
    CHECK(f.Ins() == std::vector<std::string>({"k", "indx"}));
    CHECK(Names(OutputsOf(f.g, *f.g.Find(f.e))) == std::vector<std::string>({"value"}));
}

TEST(formula_fixed_list_not_inputs)
{
    FGraph f;
    std::string why;
    CHECK(SetFormula(f.g, f.e, "position.x + m1 + i2 + time + seed + count + index + id + attr(\"ix\") + parent.ix", &why));
    CHECK(f.Ins().empty());
    const FormulaSockets s = ReadFormula("a = 1\nb = a + k");
    CHECK(s.ok && s.inputs == std::vector<std::string>({"k"}));
}

TEST(formula_named_attr_bare_is_input)
{
    Graph g;
    std::vector<Diag> errs, notes;
    CHECK(ApplyCode(g, "g = grid(cx: 3, cy: 1)\noutput(g)\n", &errs, &notes));
    const uint32_t e = AddNode(g, "expr", "e", 0, 0);
    std::string why;
    CHECK(SetFormula(g, e, "ix * 2", &why));
    CHECK(g.Find(e)->In("ix") != nullptr);
}

TEST(formula_attr_reads_named_attribute)
{
    Graph g;
    std::vector<Diag> errs, notes;
    CHECK(ApplyCode(g, "g = grid(cx: 3, cy: 1, sx: 1)\nh = set_position(g, vec(0, 5, 0))\noutput(h)\n", &errs, &notes));
    Node* h = g.FindKey("h");
    CHECK(h != nullptr);
    if (!h) return;
    const uint32_t e = AddNode(g, "expr", "e", 0, 0);
    std::string why;
    CHECK(SetFormula(g, e, "vec(attr(\"ix\") * 10, 0, 0)", &why));
    g.FindKey("h")->In("position")->link = {e, 0};
    const auto r = Evaluate(g, Ctx());
    CHECK(r.elements.size() == 3 && r.elements[2].pos[0] == 20);
}

TEST(formula_keeps_wires_by_name)
{
    FGraph f;
    std::string why;
    CHECK(SetFormula(f.g, f.e, "a + b", &why));
    const uint32_t i = AddNode(f.g, "index", "i", 0, 0), t = AddNode(f.g, "time", "t", 0, 0);
    f.g.Find(f.e)->In("a")->link = {i, 0};
    f.g.Find(f.e)->In("b")->link = {t, 0};
    CHECK(SetFormula(f.g, f.e, "a + c", &why));
    const Node* n = f.g.Find(f.e);
    CHECK(n->In("a") && n->In("a")->link.from == i);
    CHECK(!n->In("b"));
    CHECK(n->In("c") && n->In("c")->link.from == 0);
}

TEST(formula_output_wires_by_name)
{
    FGraph f;
    std::string why;
    CHECK(SetFormula(f.g, f.e, "x = 1\ny = 2", &why));
    f.g.Find(f.vec)->In("b")->link = {f.e, 1};
    f.g.Find(f.vec)->In("a")->link = {f.e, 0};
    CHECK(SetFormula(f.g, f.e, "y = 2\nx = 1", &why));
    CHECK(f.g.Find(f.vec)->In("b")->link.output == 0 && f.g.Find(f.vec)->In("a")->link.output == 1);
    // an output that goes takes its wires with it
    CHECK(SetFormula(f.g, f.e, "y = 2", &why));
    CHECK(f.g.Find(f.vec)->In("a")->link.from == 0 && f.g.Find(f.vec)->In("b")->link.from == f.e);
}

TEST(formula_bad_text_untouched)
{
    FGraph f;
    std::string why;
    CHECK(SetFormula(f.g, f.e, "a + b", &why));
    const std::string before = Serialize(f.g);
    CHECK(!SetFormula(f.g, f.e, "a +", &why) && !why.empty());
    CHECK(!SetFormula(f.g, f.e, "x = 1\n2", &why));  // a line without a name when there are several
    CHECK(Serialize(f.g) == before);
    const FormulaSockets s = ReadFormula("x = 1\ny = (2");
    CHECK(!s.ok && s.error.pos.line == 2);
}

TEST(formula_random_gets_salt)
{
    FGraph f;
    std::string why;
    CHECK(SetFormula(f.g, f.e, "random() * 10", &why));
    const std::string t = f.g.Find(f.e)->text;
    CHECK(t.find("random(salt: 0x") != std::string::npos);
    CHECK(SetFormula(f.g, f.e, t, &why) && f.g.Find(f.e)->text == t);
    f.g.Find(f.vec)->In("a")->link = {f.e, 0};
    const dxg::Element* a = f.El(1);
    const double x1 = a ? a->pos[0] : -1;
    const dxg::Element* b = f.El(2);
    CHECK(x1 >= 0 && x1 < 10 && b && b->pos[0] != x1);
}

TEST(formula_geometry_call_rejected)
{
    FGraph f;
    std::string why;
    CHECK(!SetFormula(f.g, f.e, "at(g, 0).position", &why));
    CHECK(!SetFormula(f.g, f.e, "grid(3)", &why));
}

TEST(const_text_rejects_names)
{
    Graph g;
    const uint32_t c = AddNodeAt(g, "circle", 0, 0);
    std::string why;
    CHECK(SetConstText(g, c, "radius", "360 / 12", &why));
    CHECK(g.Find(c)->In("radius")->link.from == 0 && g.Find(c)->In("radius")->value.f == 30);
    CHECK(SetConstText(g, c, "count", "3 * 4", &why) && g.Find(c)->In("count")->value.i == 12);
    CHECK(!SetConstText(g, c, "radius", "m1 * 2", &why) && why == "a formula goes in an Expression node");
    CHECK(!SetConstText(g, c, "radius", "1 +", &why));
}

TEST(connect_expression)
{
    Graph g;
    const uint32_t c = AddNodeAt(g, "circle", 100, 0);
    CHECK(ConnectExpression(g, c, "radius"));
    const Node* e = g.Find(g.Find(c)->In("radius")->link.from);
    CHECK(e && e->type == "expr" && e->text == "0" && e->x < 100);
}
