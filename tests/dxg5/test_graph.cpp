// Task 2: the graph, its stored form (version 2) and the conversion of version 1 graphs.
#include <string>

#include "dxg5/graph.h"
#include "graph/edit.h"
#include "graph/graph.h"
#include "test.h"

using namespace dx5;

static Graph Sample()
{
    Graph g;
    const uint32_t p = AddNode(g, "points", "pts", 10, 20);
    const uint32_t s = AddNode(g, "set_position", "", 200, 20);
    const uint32_t m = AddNode(g, "macro", "m4", 0, 120);
    const uint32_t o = AddNode(g, "output", "out", 400, 20);
    g.Find(m)->setting = 4;
    g.Find(p)->In("count")->value = Value::I(7);
    g.Find(s)->In("geometry")->link = {p, 0};
    g.Find(s)->In("position")->value = Value::V({1.5, -2, 0.1});
    g.Find(s)->In("where")->value = Value::B(false);
    g.Find(o)->In("geometry")->link = {s, 0};
    g.Find(o)->In("copy_scale")->link = {m, 0};
    const uint32_t e = AddNode(g, "expr", "out.copy_scale", 300, 120);
    g.Find(e)->text = "m4 * 2 + 0.5";
    g.Find(e)->salt = 0x3fa1;
    g.macros[3] = {"Spread Out", -5, 500};
    g.intMacros[0] = {"Count", 1, 50};
    g.revision = 9;
    g.unknownLines.push_back("future thing 1 2 3");
    return g;
}

TEST(v2_roundtrip)
{
    const Graph g = Sample();
    const std::string a = Serialize(g);
    CHECK(a.rfind("DupliX graph 2\n", 0) == 0);
    Graph h;
    std::string err;
    CHECK(Parse(a, &h, &err));
    CHECK(Serialize(h) == a);
    CHECK(h.revision == 9);
    CHECK(h.macros[3].name == "Spread Out");
    CHECK(h.intMacros[0].sliderMax == 50);
    const Node* s = h.FindKey("set_position1");
    CHECK(s && s->In("position")->value.v.x == 1.5 && s->In("where")->value.b() == false);
    CHECK(h.FindKey("out.copy_scale") && h.FindKey("out.copy_scale")->text == "m4 * 2 + 0.5");
    CHECK(h.FindKey("out.copy_scale")->salt == 0x3fa1);
    CHECK(h.FindKey("m4")->setting == 4);
    CHECK(h.OutputNode() && h.OutputNode()->key == "out");
    CHECK(h.nextId == g.nextId);
    // CRLF reads the same
    std::string crlf;
    for (char c : a) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    Graph k;
    CHECK(Parse(crlf, &k, &err) && Serialize(k) == a);
}

TEST(v2_unknown_node_kept)
{
    const std::string text = "DupliX graph 2\nrevision 1\nnextid 6\nnode 5 wobble 10 20 w1\nin 5 amount const f:0.5\n"
                             "in 5 geo link 3 0\nnode 3 points 0 0 pts\nin 3 count const i:4\nlater 1 2\n";
    Graph g;
    std::string err;
    CHECK(Parse(text, &g, &err));
    const Node* w = g.FindKey("w1");
    CHECK(w && w->type == "wobble" && w->inputs.size() == 2);
    const std::string s = Serialize(g);
    CHECK(s.find("node 5 wobble 10 20 w1") != std::string::npos);
    CHECK(s.find("in 5 amount const f:0.5") != std::string::npos);
    CHECK(s.find("in 5 geo link 3 0") != std::string::npos);
    CHECK(s.find("later 1 2") != std::string::npos);
    Graph h;
    CHECK(!Parse("hello\n", &h, &err));
    CHECK(!Parse("", &h, &err));
}

TEST(v1_default_preset_converts)
{
    const std::string v1 = dxg::Serialize(dxg::DefaultPreset().graph);
    Graph g;
    std::string err;
    CHECK(Parse(v1, &g, &err));
    const Node* grid = g.FindKey("grid1");
    CHECK(grid && grid->type == "grid" && grid->legacyId == 1);
    const Node* out = g.FindKey("output2");
    CHECK(out && out->type == "output" && out->In("geometry")->link.from == grid->id);
    const Node* m1 = g.FindKey("m1");
    CHECK(m1 && m1->type == "macro" && m1->setting == 1);
    CHECK(grid->In("cx")->link.from == m1->id && grid->In("cy")->link.from == m1->id);
    CHECK(grid->In("sx")->link.from == g.FindKey("m2")->id);
    CHECK(out->In("copy_scale")->link.from == g.FindKey("m3")->id);
    CHECK(grid->In("cz")->value.type == Type::Int && grid->In("cz")->value.i == 1);
    CHECK(g.macros[0].name == "Count" && g.macros[0].sliderMax == 50);
}

TEST(v1_expression_link)
{
    const std::string v1 = "DupliX graph 1\nnextid 3\nnode 1 Grid 0 0\nparam 1 spacingX 10 m2 2\n"
                           "param 1 spacingY 0 m2 -0.5 m3 1\nnode 2 Transform 0 0\nparam 2 posX 5\nparam 2 posY 0 m1 1\n"
                           "param 2 pivot 1\nwire 1 2 0\n";
    Graph g;
    std::string err;
    CHECK(Parse(v1, &g, &err));
    const Node* grid = g.FindKey("grid1");
    CHECK(grid != nullptr);
    if (!grid) return;
    const Node* e = g.Find(grid->In("sx")->link.from);
    CHECK(e && e->type == "expr" && e->text == "10 + 2*m2" && e->key == "grid1.sx");
    const Node* f = g.Find(grid->In("sy")->link.from);
    CHECK(f && f->text == "-0.5*m2 + m3");
    const Node* tr = g.FindKey("transform2");
    CHECK(tr && tr->In("geometry")->link.from == grid->id);
    const Node* p = g.Find(tr->In("position")->link.from);
    CHECK(p && p->text == "vec(5, m1, 0)");
    CHECK(tr->In("pivot")->value.type == Type::Int && tr->In("pivot")->value.i == 1);
    // constants only: a Vector constant
    Graph h;
    CHECK(Parse("DupliX graph 1\nnextid 2\nnode 1 Modify 0 0\nparam 1 posX 5\nparam 1 posZ -1\n", &h, &err));
    const InputSlot* hp = h.FindKey("modify1")->In("position");
    CHECK(hp->link.from == 0 && hp->value.v.x == 5 && hp->value.v.y == 0 && hp->value.v.z == -1);
}
