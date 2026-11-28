// Plan P1.5e Task 5: the editor's new operations (design v5 4.4, 8.3, 12, 12.1): copy a built-in group for
// yourself, group a selection (Ctrl+G) and back, an Expression to a group, collapse into an Expression of several
// outputs, the unused macros, and the script's new commands.
#include <cstdio>
#include <string>

#include "dxg5/edit.h"
#include "dxg5/eval.h"
#include "dxg5/lang.h"
#include "test.h"

using namespace dx5;

namespace {

EvalContext Ctx5()
{
    EvalContext c;
    std::vector<double> m(32);
    std::vector<int64_t> im(32);
    for (int k = 0; k < 32; k++) m[k] = MacroDefault(k), im[k] = IntMacroDefault(k);
    c.macro = [m](int k, double) { return m[k]; };
    c.intMacro = [im](int k, double) { return im[k]; };
    return c;
}

bool SameElements(const dxg::EvalResult& a, const dxg::EvalResult& b, bool ids)
{
    if (a.elements.size() != b.elements.size()) return false;
    for (size_t k = 0; k < a.elements.size(); k++) {
        for (int j = 0; j < 3; j++)
            if (a.elements[k].pos[j] != b.elements[k].pos[j]) return false;
        if (ids && a.elements[k].id != b.elements[k].id) return false;
    }
    return true;
}

bool ReadE(const std::string& code, Graph* g)
{
    std::vector<Diag> errs, notes;
    if (ApplyCode(*g, code, &errs, &notes)) return true;
    std::printf("  cannot read: %s\n", errs.empty() ? "?" : errs[0].msg.c_str());
    return false;
}

}  // namespace

TEST(copy_group_same_elements)
{
    Preset p = DefaultPreset();
    Graph g = p.graph;
    const auto before = Evaluate(g, Ctx5());
    const Node* grid = nullptr;
    for (const Node& n : g.nodes)
        if (n.type == "grid") grid = &n;
    CHECK(grid != nullptr);
    if (!grid) return;
    const std::string key = grid->key;
    std::string name;
    CHECK(CopyGroupForSelf(g, grid->id, &name));
    CHECK(name == "grid_copy" && g.FindKey(key) && g.FindKey(key)->type == "grid_copy");
    CHECK(FindUserGroup(g, "grid_copy") != nullptr);
    const auto after = Evaluate(g, Ctx5());
    CHECK(after.warnings.empty());
    CHECK(SameElements(before, after, true));
    // a second copy gets the next name
    uint32_t other = AddNodeAt(g, "grid", 0, 0);
    CHECK(CopyGroupForSelf(g, other, &name) && name == "grid_copy_2");
    // the code reads back the same
    const std::string code = GraphToCode(g);
    CHECK(code.find("group grid_copy(") != std::string::npos);
    Graph back;
    std::vector<Diag> errs, notes;
    CHECK(CodeToGraph(code, g, &back, &errs, &notes));
    std::string why;
    CHECK(SameGraph(g, back, &why));
    if (!why.empty()) std::printf("  %s\n", why.c_str());
}

TEST(group_selection_wires)
{
    Graph g;
    CHECK(ReadE("g = grid(cx: 3, cy: 2)\nh = set_position(g, position * 2)\noutput(h)\n", &g));
    const auto before = Evaluate(g, Ctx5());
    const Node* h = g.FindKey("h");
    CHECK(h != nullptr);
    if (!h) return;
    std::vector<uint32_t> sel = {h->id};
    for (const Node& n : g.nodes)
        if (n.key.rfind("h.", 0) == 0) sel.push_back(n.id);  // its formula
    uint32_t gn = 0;
    CHECK(GroupSelection(g, sel, "", &gn));
    const Node* G = g.Find(gn);
    CHECK(G && G->type == "group1");
    const UserGroup* u = G ? FindUserGroup(g, G->type) : nullptr;
    CHECK(u && u->def.inputs.size() == 1 && u->def.inputs[0].type == Type::Geometry && u->def.outputs.size() == 1);
    CHECK(!g.FindKey("h"));
    const auto after = Evaluate(g, Ctx5());
    CHECK(SameElements(before, after, false));
}

TEST(ungroup_restores)
{
    Graph g;
    CHECK(ReadE("g = grid(cx: 3, cy: 2)\nh = set_position(g, position * 2)\noutput(h)\n", &g));
    const Graph orig = g;
    std::vector<uint32_t> sel;
    for (const Node& n : g.nodes)
        if (n.key == "h" || n.key.rfind("h.", 0) == 0) sel.push_back(n.id);
    uint32_t gn = 0;
    CHECK(GroupSelection(g, sel, "", &gn));
    CHECK(UngroupNode(g, gn));
    CHECK(g.groups.empty());
    std::string why;
    CHECK(SameGraph(orig, g, &why));
    if (!why.empty()) std::printf("  %s\n", why.c_str());
}

TEST(expression_to_group)
{
    Graph g;
    CHECK(ReadE("g = points(3)\nh = set_position(g, vec(index * 10 + m2, 0, 0))\noutput(h)\n", &g));
    const auto before = Evaluate(g, Ctx5());
    const Node* e = g.FindKey("h.position");
    CHECK(e && e->type == "expr");
    if (!e) return;
    std::string name;
    CHECK(ExpressionToGroup(g, e->id, &name));
    CHECK(FindUserGroup(g, name) != nullptr);
    CHECK(SameElements(before, Evaluate(g, Ctx5()), true));
}

TEST(collapse_multi_output)
{
    Graph g;
    CHECK(ReadE("g = points(3)\nh = set_position(g, vmath_vec(math_mul(index, 2), math_add(index, 1), 0))\noutput(h)\n", &g));
    const auto before = Evaluate(g, Ctx5());
    std::vector<uint32_t> sel;
    for (const Node& n : g.nodes)
        if ((IsMathFamily(n.type) && n.text != "vec") || n.type == "index") sel.push_back(n.id);
    CHECK(CollapseToExpression(g, sel));
    int exprs = 0;
    const Node* ex = nullptr;
    for (const Node& n : g.nodes)
        if (n.type == "expr") exprs++, ex = &n;
    CHECK(exprs == 1 && ex && OutputsOf(g, *ex).size() == 2);
    CHECK(SameElements(before, Evaluate(g, Ctx5()), true));
}

TEST(unused_named_macro)
{
    Preset p = DefaultPreset();
    Graph g = p.graph;
    CHECK(UnusedNamedMacros(g, false).empty());
    // take Spacing (Macro 2) off both of the grid's inputs
    for (Node& n : g.nodes)
        for (InputSlot& s : n.inputs)
            if (s.link.from) {
                const Node* f = g.Find(s.link.from);
                if (f && f->type == "macro" && f->setting == 2) s.link = {};
            }
    CHECK(UnusedNamedMacros(g, false) == std::vector<int>({1}));
    CHECK(FreeMacro(g, false) != 1);
    CHECK(ExposeRefusal(g, false).empty());
}

TEST(expose_refusal_when_full)
{
    Graph g;
    for (int k = 0; k < 32; k++) RenameMacro(g, false, k, "M" + std::to_string(k));
    CHECK(FreeMacro(g, false) < 0);
    CHECK(ExposeRefusal(g, false) == "No free macro (clear the name of an unused macro to use it)");
    CHECK(ExposeRefusal(g, true).empty());
}

TEST(script_new_commands)
{
    Graph g;
    std::string err;
    std::vector<MacroWrite> w;
    const std::string script =
        "preset Grid\n"
        "add math 0 0 as m\n"
        "op m sin\n"
        "add expr 0 100 as e\n"
        "formula e x = index * 2\\ny = 3\n";
    CHECK(RunScript(g, script, &w, &err));
    if (!err.empty()) std::printf("  %s\n", err.c_str());
    const Node* m = nullptr;
    const Node* e = nullptr;
    for (const Node& n : g.nodes) {
        if (n.type == "math") m = &n;
        if (n.type == "expr" && n.text.find("y = 3") != std::string::npos) e = &n;
    }
    CHECK(m && m->text == "sin");
    CHECK(e && OutputsOf(g, *e).size() == 2);
    const Node* grid = nullptr;
    for (const Node& n : g.nodes)
        if (n.type == "grid") grid = &n;
    CHECK(grid != nullptr);
    if (!grid) return;
    const std::string gk = grid->key;
    CHECK(RunScript(g, "copygroup " + gk + "\n", &w, &err));
    CHECK(g.FindKey(gk) && g.FindKey(gk)->type == "grid_copy");
    CHECK(RunScript(g, "connectexpr " + gk + " sx\n", &w, &err));
    const InputSlot* sx = g.FindKey(gk)->In("sx");
    CHECK(sx && sx->link.from && g.Find(sx->link.from)->type == "expr");
}
