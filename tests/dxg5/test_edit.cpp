// Task 8: the editor's operations, the presets and the test scripts.
#include <string>

#include "dxg5/edit.h"
#include "dxg5/eval.h"
#include "dxg5/lang.h"
#include "test.h"

using namespace dx5;

namespace {

EvalContext Ctx(const std::vector<MacroWrite>& w = {})
{
    EvalContext c;
    double m[32];
    int64_t im[32];
    for (int k = 0; k < 32; k++) m[k] = MacroDefault(k), im[k] = IntMacroDefault(k);
    for (const MacroWrite& x : w) (x.isInt ? im[x.k] = (int64_t)x.value : m[x.k] = x.value);
    std::vector<double> mv(m, m + 32);
    std::vector<int64_t> iv(im, im + 32);
    c.macro = [mv](int k, double) { return mv[k]; };
    c.intMacro = [iv](int k, double) { return iv[k]; };
    return c;
}

}  // namespace

TEST(default_preset_5x5)
{
    const Preset p = DefaultPreset();
    const auto r = Evaluate(p.graph, Ctx());
    CHECK(r.elements.size() == 25 && r.warnings.empty());
    if (r.elements.size() == 25) {
        CHECK_NEAR(r.elements[0].pos[0], -200, 1e-9);
        CHECK_NEAR(r.elements[24].pos[1], 200, 1e-9);
    }
    CHECK(MacroLabel(p.graph, true, 0) == "Count");
    CHECK(MacroLabel(p.graph, false, 1) == "Spacing");
    CHECK(IntMacroDefault(0) == 5);
    CHECK(UsedIntMacros(p.graph) == 1u && UsedMacros(p.graph) == 6u);
    // the preset's macro values are the effect's defaults (a new instance draws without a write)
    for (const MacroWrite& w : p.macros) CHECK(w.value == (w.isInt ? (double)IntMacroDefault(w.k) : MacroDefault(w.k)));
    const auto c = Evaluate(CirclePreset().graph, Ctx(CirclePreset().macros));
    CHECK(c.elements.size() == 12);
}

TEST(expose_int_for_count)
{
    Graph g;
    const uint32_t c = AddNodeAt(g, "circle", 0, 0);
    const uint32_t o = AddNodeAt(g, "output", 300, 0);
    std::string why;
    CHECK(Connect(g, c, 0, o, "geometry", &why));
    MacroWrite w;
    CHECK(Expose(g, c, "count", &w));
    CHECK(w.isInt && w.k == 0 && w.value == 12);
    CHECK(g.Find(g.Find(c)->In("count")->link.from)->type == "int_macro");
    CHECK(g.intMacros[0].name == "Count");
    CHECK(Expose(g, c, "radius", &w));
    CHECK(!w.isInt && w.k == 0 && w.value == 300);
    CHECK(g.macros[0].name == "Radius" && g.macros[0].sliderMax >= 1000);
    CHECK(Expose(g, c, "start_angle", &w) && w.k == 1 && g.macros[1].name == "Start Angle");
    CHECK(!Expose(g, c, "align", &w));  // a choice
    CHECK(UsedMacros(g) == 3u && UsedIntMacros(g) == 1u);
}

TEST(set_expression)
{
    Graph g;
    const uint32_t c = AddNodeAt(g, "circle", 0, 0);
    std::string why;
    CHECK(SetExpression(g, c, "radius", "360 / 12", &why));
    CHECK(g.Find(c)->In("radius")->link.from == 0 && g.Find(c)->In("radius")->value.f == 30);
    CHECK(SetExpression(g, c, "radius", "m1 * 2 + index", &why));
    const Node* e = g.Find(g.Find(c)->In("radius")->link.from);
    CHECK(e && e->type == "expr" && e->text == "m1 * 2 + index");
    CHECK(!SetExpression(g, c, "count", "index + 1", &why) && why.find("cannot vary per element") != std::string::npos);
    CHECK(SetExpression(g, c, "count", "3 * 4", &why) && g.Find(c)->In("count")->value.i == 12);
    CHECK(!SetExpression(g, c, "radius", "1 +", &why));
}

TEST(expand_collapse)
{
    Graph g;
    std::vector<Diag> errs, notes;
    CHECK(ApplyCode(g, "g = points(3)\ng = set_position(g, vec(index * 2 + 1, m2, 0))\noutput(g)\n", &errs, &notes));
    const auto before = Evaluate(g, Ctx());
    const Node* e = g.FindKey("g_2.position");
    CHECK(e && e->type == "expr");
    if (!e) return;
    CHECK(ExpandExpression(g, e->id));
    int math = 0, exprs = 0;
    for (const Node& n : g.nodes) math += n.type.rfind("math_", 0) == 0, exprs += n.type == "expr";
    CHECK(math == 3 && exprs == 0);  // vec, add, mul
    const auto mid = Evaluate(g, Ctx());
    CHECK(mid.elements.size() == 3 && mid.elements[2].pos[0] == before.elements[2].pos[0] && mid.elements[2].pos[1] == 100);
    std::vector<uint32_t> sel;
    for (const Node& n : g.nodes)
        if (n.type.rfind("math_", 0) == 0 || n.type == "index") sel.push_back(n.id);
    CHECK(CollapseToExpression(g, sel));
    exprs = 0, math = 0;
    for (const Node& n : g.nodes) math += n.type.rfind("math_", 0) == 0, exprs += n.type == "expr";
    CHECK(math == 0 && exprs == 1);
    const auto after = Evaluate(g, Ctx());
    CHECK(after.elements.size() == 3 && after.elements[2].pos[0] == before.elements[2].pos[0]);
}

TEST(connect_rejects)
{
    Graph g;
    const uint32_t p = AddNodeAt(g, "points", 0, 0);
    const uint32_t idx = AddNodeAt(g, "index", 0, 0);
    const uint32_t sp = AddNodeAt(g, "set_position", 0, 0);
    const uint32_t sp2 = AddNodeAt(g, "set_position", 0, 0);
    std::string why;
    CHECK(!Connect(g, idx, 0, p, "count", &why) && why.find("cannot vary per element") != std::string::npos);
    CHECK(!Connect(g, idx, 0, sp, "geometry", &why));
    CHECK(!Connect(g, p, 0, sp, "position", &why));
    CHECK(Connect(g, p, 0, sp, "geometry", &why));
    CHECK(Connect(g, sp, 0, sp2, "geometry", &why));
    CHECK(!Connect(g, sp2, 0, sp, "geometry", &why) && why.find("loop") != std::string::npos);
    CHECK(Disconnect(g, sp2, "geometry"));
    CHECK(RemoveNode(g, sp));
    CHECK(!g.Find(sp));
}

TEST(script_code_block)
{
    Graph g = DefaultPreset().graph;
    std::vector<MacroWrite> w;
    std::string err;
    CHECK(RunScript(g, "code\nc = circle(count: 6, radius: 80)\noutput(c, copy_scale: 50)\nend\n", &w, &err));
    if (!err.empty()) std::printf("  %s\n", err.c_str());
    auto r = Evaluate(g, Ctx());
    CHECK(r.elements.size() == 6);
    // the P1 commands still work (v1 names of the inputs; nodes by alias, key or id)
    Graph h;
    CHECK(RunScript(h, "clear\nadd Circle 40 60 as c\nadd Modify 200 60 as m\nadd Output 360 60 as o\nwire c m\nwire m o\n"
                       "set c count 6\nexpose c radius\nset m rotZ 45\nset m opacity -50\n",
                    &w, &err));
    if (!err.empty()) std::printf("  %s\n", err.c_str());
    r = Evaluate(h, Ctx(w));
    CHECK(r.elements.size() == 6);
    if (r.elements.size() == 6) CHECK_NEAR(r.elements[0].opacity, 0.5, 1e-12);
    Graph d = DefaultPreset().graph;
    w.clear();
    CHECK(RunScript(d, "set 1 countZ 2\nset 1 spacingZ 80\nexpose 1 spacingZ\n", &w, &err));
    if (!err.empty()) std::printf("  %s\n", err.c_str());
    CHECK(w.size() == 1 && !w[0].isInt && w[0].k == 0 && w[0].value == 80);
    CHECK(Evaluate(d, Ctx(w)).elements.size() == 50);
}
