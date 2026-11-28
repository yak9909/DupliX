// Task 7: the built-in groups: their code definitions and the C++ that stands in for them give the same elements;
// version 1 graphs make P1's elements.
#include <cmath>
#include <string>

#include "dxg5/eval.h"
#include "dxg5/graph.h"
#include "dxg5/lang.h"
#include "graph/edit.h"
#include "graph/eval.h"
#include "graph/graph.h"
#include "test.h"

using namespace dx5;

namespace {

EvalContext Ctx(bool byDefinition, uint32_t layers = 0, int seed = 0)
{
    EvalContext c;
    c.macro = [](int k, double) { return k == 0 ? 4.0 : k == 1 ? 120.0 : k == 2 ? 100.0 : 0.0; };
    c.intMacro = [](int k, double) { return k == 0 ? (int64_t)3 : (int64_t)0; };
    c.groupsByDefinition = byDefinition;
    c.layersSet = layers;
    c.seed = seed;
    return c;
}

bool Read(const std::string& code, Graph* g)
{
    std::vector<Diag> errs, notes;
    if (CodeToGraph(code, Graph(), g, &errs, &notes)) return true;
    std::printf("  cannot read: %s (line %d)\n", errs.empty() ? "?" : errs[0].msg.c_str(), errs.empty() ? 0 : errs[0].pos.line);
    return false;
}

void Same(const std::string& code, uint32_t layers = 0, int seed = 0)
{
    Graph g;
    if (!Read(code, &g)) {
        CHECK(false);
        return;
    }
    const auto a = Evaluate(g, Ctx(true, layers, seed)), b = Evaluate(g, Ctx(false, layers, seed));
    for (const auto& w : a.warnings) std::printf("  definition warning: %s\n", w.c_str());
    for (const auto& w : b.warnings) std::printf("  native warning: %s\n", w.c_str());
    CHECK(a.warnings.empty() && b.warnings.empty());
    CHECK(a.elements.size() == b.elements.size());
    if (a.elements.size() != b.elements.size()) {
        std::printf("  %s: %zu vs %zu elements\n", code.c_str(), a.elements.size(), b.elements.size());
        return;
    }
    CHECK(!a.elements.empty());
    bool same = true;
    for (size_t k = 0; k < a.elements.size(); k++) {
        const dxg::Element &x = a.elements[k], &y = b.elements[k];
        bool s = x.id == y.id && x.source == y.source && std::fabs(x.opacity - y.opacity) <= 1e-9;
        for (int i = 0; i < 3; i++) s &= std::fabs(x.pos[i] - y.pos[i]) <= 1e-9 && std::fabs(x.scale[i] - y.scale[i]) <= 1e-9;
        for (int i = 0; i < 9; i++) s &= std::fabs(x.basis[i] - y.basis[i]) <= 1e-9;
        if (!s && same) {
            std::printf("  %s\n  element %zu: def pos %.9g %.9g %.9g src %d op %.6g / native pos %.9g %.9g %.9g src %d op %.6g\n", code.c_str(),
                        k, x.pos[0], x.pos[1], x.pos[2], x.source, x.opacity, y.pos[0], y.pos[1], y.pos[2], y.source, y.opacity);
        }
        same &= s;
    }
    CHECK(same);
}

}  // namespace

TEST(group_matches_native)
{
    Same("g = grid()\noutput(g)\n");
    Same("g = grid(cx: 4, cy: 3, cz: 2, sx: 30, sy: 45.5, sz: 7)\noutput(g)\n");
    Same("g = grid(cx: i1, cy: 1, sx: m2)\noutput(g)\n");
    Same("c = circle()\noutput(c)\n");
    Same("c = circle(count: 7, radius: 90, start_angle: 30, sweep: 180, align: 1)\noutput(c)\n");
    Same("c = circle(count: 1, sweep: -400, align: 1)\noutput(c)\n");
    Same("l = line()\noutput(l)\n");
    Same("l = line(count: 5, length: 300, angle: 33)\noutput(l)\n");
    Same("l = line(count: 1)\noutput(l)\n");
    Same("g = grid(cx: 3, cy: 2)\nt = transform(g, position: vec(10, -20, 5), rotation_x: 10, rotation_y: 20, rotation_z: 30, scale: 150)\noutput(t)\n");
    Same("g = grid(cx: 3, cy: 2)\nt = transform(g, position: vec(1, 2, 3), rotation_z: 45, scale: 50, pivot: 1)\noutput(t)\n");
    Same("c = circle(count: 5, align: 1)\nm = modify(c, position: vec(0, 5, 0), rotation_z: 20, scale: -50, opacity: -30)\noutput(m)\n");
    Same("c = circle(count: 5)\nc = set_weight(c, index / 4)\nm = modify(c, position: vec(10, 0, 0), scale: 100, opacity: 50)\noutput(m)\n");
    Same("g = grid(cx: 4, cy: 2)\ns = source_layers(g, first: 2, layers: 3)\noutput(s)\n", 0x0Bu);
    Same("g = grid(cx: 4, cy: 2)\ns = source_layers(g, first: 1, layers: 8, assign: 1)\noutput(s)\n", 0xA5u, 7);
    Same("g = grid(cx: 4, cy: 2)\ns = source_layers(g, first: 3, layers: 2)\noutput(s)\n", 0x01u);
}

TEST(group_field_input)
{
    Graph g;
    CHECK(Read("c = circle(count: 36, radius: 100 + index * 5)\noutput(c)\n", &g));
    const auto r = Evaluate(g, Ctx(false));
    CHECK(r.elements.size() == 36 && r.warnings.empty());
    if (r.elements.size() == 36) {
        const double r0 = std::hypot(r.elements[0].pos[0], r.elements[0].pos[1]);
        const double r9 = std::hypot(r.elements[9].pos[0], r.elements[9].pos[1]);
        CHECK_NEAR(r0, 100, 1e-9);
        CHECK_NEAR(r9, 145, 1e-9);
    }
    // the attributes the groups keep can be read after them
    Graph h;
    CHECK(Read("g = grid(cx: 3, cy: 2)\ng = set_position(g, vec(ix * 10, iy * 100, u))\noutput(g)\n", &h));
    const auto s = Evaluate(h, Ctx(false));
    CHECK(s.elements.size() == 6);
    if (s.elements.size() == 6) {
        CHECK_NEAR(s.elements[5].pos[0], 20, 0);
        CHECK_NEAR(s.elements[5].pos[1], 100, 0);
        CHECK_NEAR(s.elements[5].pos[2], 1, 0);
    }
}

TEST(v1_graphs_same_elements)
{
    std::vector<dxg::Graph> graphs;
    graphs.push_back(dxg::DefaultPreset().graph);
    graphs.push_back(dxg::CirclePreset().graph);
    {
        dxg::Graph g;
        const uint32_t c = dxg::AddNode(g, dxg::NodeType::Circle, 0, 0);
        const uint32_t t = dxg::AddNode(g, dxg::NodeType::Transform, 0, 0);
        const uint32_t m = dxg::AddNode(g, dxg::NodeType::Modify, 0, 0);
        const uint32_t o = dxg::AddNode(g, dxg::NodeType::Output, 0, 0);
        dxg::Connect(g, c, t, 0), dxg::Connect(g, t, m, 0), dxg::Connect(g, m, o, 0);
        dxg::SetValue(g, c, "count", 9), dxg::SetValue(g, c, "align", 1);
        dxg::SetValue(g, t, "posX", 30), dxg::SetValue(g, t, "rotZ", 25), dxg::SetValue(g, t, "scale", 80);
        dxg::LinkMacro(g, t, "posY", 1, 0.5);
        dxg::SetValue(g, m, "rotX", 15), dxg::SetValue(g, m, "opacity", -40);
        dxg::SetValue(g, o, "copyScale", 50);
        graphs.push_back(g);
    }
    {
        dxg::Graph g;
        const uint32_t l = dxg::AddNode(g, dxg::NodeType::Line, 0, 0);
        const uint32_t s = dxg::AddNode(g, dxg::NodeType::Source, 0, 0);
        const uint32_t o = dxg::AddNode(g, dxg::NodeType::Output, 0, 0);
        dxg::Connect(g, l, s, 0), dxg::Connect(g, s, o, 0);
        dxg::SetValue(g, s, "first", 2), dxg::SetValue(g, s, "count", 4), dxg::SetValue(g, s, "assign", 1);
        graphs.push_back(g);
    }
    for (const dxg::Graph& v1 : graphs) {
        dxg::EvalContext c1;
        c1.macro = [](int k, double) { return k == 0 ? 4.0 : k == 1 ? 120.0 : k == 2 ? 100.0 : 0.0; };
        c1.layersSet = 0x5E;
        c1.seed = 3;
        const std::string want = dxg::FormatElements(dxg::Evaluate(v1, c1));
        Graph g;
        std::string err;
        CHECK(Parse(dxg::Serialize(v1), &g, &err));
        const std::string got = dxg::FormatElements(Evaluate(g, Ctx(false, 0x5E, 3)));
        if (got != want) std::printf("--- want:\n%s--- got:\n%s", want.substr(0, 600).c_str(), got.substr(0, 600).c_str());
        CHECK(got == want);
    }
}
