// Task 5: code -> graph (names, types, keys, capture, each), and the Expression node's evaluation.
#include <cmath>
#include <string>

#include "dxg5/eval.h"
#include "dxg5/graph.h"
#include "dxg5/lang.h"
#include "test.h"

using namespace dx5;

namespace {

EvalContext Ctx()
{
    EvalContext c;
    c.macro = [](int k, double) { return k == 1 ? 2.0 : 0.0; };
    c.intMacro = [](int k, double) { return k == 0 ? (int64_t)3 : (int64_t)0; };
    return c;
}

bool Build(const std::string& code, Graph* g, std::string* firstError = nullptr, std::vector<Diag>* notes = nullptr,
           const Graph& prev = Graph())
{
    std::vector<Diag> errs, n;
    const bool ok = CodeToGraph(code, prev, g, &errs, notes ? notes : &n);
    if (firstError) *firstError = errs.empty() ? "" : std::to_string(errs[0].pos.line) + ": " + errs[0].msg;
    if (!ok && errs.empty()) std::printf("  (no error given)\n");
    if (!ok && !firstError) std::printf("  %s\n", errs.empty() ? "?" : errs[0].msg.c_str());
    return ok;
}

bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

}  // namespace

TEST(lower_basic)
{
    Graph g;
    CHECK(Build("g = points(5)\ng = set_position(g, position + vec(0, sin(index * 0.3) * 20, 0))\noutput(g)\n", &g));
    const auto r = Evaluate(g, Ctx());
    CHECK(r.elements.size() == 5 && r.warnings.empty());
    if (r.elements.size() == 5) CHECK_NEAR(r.elements[3].pos[1], std::sin(0.9) * 20, 1e-12);
    for (const auto& w : r.warnings) std::printf("  warning: %s\n", w.c_str());
}

TEST(lower_macros_and_constants)
{
    Graph g;
    CHECK(Build("g = points(i1)\ng = set_position(g, vec(index * m2, 6 * 2, 0))\noutput(g, copy_scale: 50)\n", &g));
    const auto r = Evaluate(g, Ctx());
    CHECK(r.elements.size() == 3);
    if (r.elements.size() == 3) {
        CHECK_NEAR(r.elements[2].pos[0], 4, 1e-12);
        CHECK_NEAR(r.elements[2].pos[1], 12, 1e-12);
    }
    CHECK_NEAR(r.copyScale, 0.5, 0);
    const Node* p = g.FindKey("g");
    CHECK(p && p->type == "points" && g.Find(p->In("count")->link.from)->key == "i1");
}

TEST(lower_rejects_unknown_name)
{
    Graph g;
    std::string e;
    CHECK(!Build("g = points(3)\ng2 = set_position(g, vec(ix, 0, 0))\noutput(g2)\n", &g, &e));
    CHECK(Has(e, "2: ") && Has(e, "unknown name 'ix'"));
    // a group that keeps ix makes it known (Task 7); a store does now
    CHECK(Build("g = points(3)\ng = store(g, \"ix\", index)\ng2 = set_position(g, vec(ix, 0, 0))\noutput(g2)\n", &g, &e));
}

TEST(lower_rejects_field_in_variable)
{
    Graph g;
    std::string e;
    CHECK(!Build("p = position\n", &g, &e));
    CHECK(Has(e, "(e) =>") && Has(e, "g.position"));
    CHECK(!Build("g = points(2)\nq = index * 2\n", &g, &e));
    CHECK(Build("n = m2 * 2\ng = points(n)\noutput(g)\n", &g, &e));  // a single value may be named
}

TEST(lower_rejects_field_to_count)
{
    Graph g;
    std::string e;
    CHECK(!Build("g = points(index)\n", &g, &e));
    CHECK(Has(e, "cannot vary per element"));
}

TEST(lower_rejects_type)
{
    Graph g;
    std::string e;
    CHECK(!Build("g = points(3)\nh = set_position(g, true)\n", &g, &e));
    CHECK(Has(e, "type mismatch"));
    CHECK(!Build("g = points(3)\nh = set_position(points(2), g)\n", &g, &e));
    CHECK(!Build("output(5)\n", &g, &e));
}

TEST(lower_rejects_loop)
{
    Graph g;
    std::string e;
    CHECK(!Build("a = set_position(b, vec(1, 0, 0))\nb = set_position(a, vec(1, 0, 0))\n", &g, &e));
    CHECK(Has(e, "unknown name 'b'"));
    CHECK(!Build("output(points(1))\noutput(points(2))\n", &g, &e) && Has(e, "Output"));
}

TEST(reapply_keeps_keys)
{
    const std::string code = "s = points(4)\ns = set_scale(s, vec(2, 2, 2))\noutput(s)\n";
    Graph a, b;
    std::vector<Diag> notes;
    CHECK(Build(code, &a, nullptr, &notes));
    bool renamed = false;
    for (const Diag& d : notes) renamed |= Has(d.msg, "renamed s to s_2");
    CHECK(renamed);
    CHECK(a.FindKey("s") && a.FindKey("s_2"));
    CHECK(Build(code, &b, nullptr, nullptr, a));
    const auto ra = Evaluate(a, Ctx()), rb = Evaluate(b, Ctx());
    CHECK(ra.elements.size() == 4 && rb.elements.size() == 4);
    if (ra.elements.size() == 4 && rb.elements.size() == 4) CHECK(ra.elements[2].id == rb.elements[2].id);
    CHECK(a.FindKey("s")->id == b.FindKey("s")->id);
    // a node moved in the editor keeps its place when the code is applied again
    a.FindKey("s")->x = 777;
    Graph c;
    CHECK(Build(code, &c, nullptr, nullptr, a));
    CHECK(c.FindKey("s")->x == 777);
}

TEST(capture_syntax)
{
    Graph g;
    CHECK(Build("g = points(3)\ng = set_position(g, vec(index, 0, 0))\nb = g.position\ng = set_position(g, vec(0, 0, 0))\n"
                "g = store(g, \"old\", b)\ng = set_position(g, old)\noutput(g)\n",
                &g));
    const auto r = Evaluate(g, Ctx());
    CHECK(r.elements.size() == 3);
    if (r.elements.size() == 3) CHECK_NEAR(r.elements[2].pos[0], 2, 0);
    Graph h;
    CHECK(Build("g = points(2)\ng_b, b = capture(g, index * 3)\nh = set_position(g_b, vec(b, 0, 0))\noutput(h)\n", &h));
    const auto s = Evaluate(h, Ctx());
    CHECK(s.elements.size() == 2 && s.elements[1].pos[0] == 3);
}

TEST(lambda_and_at)
{
    Graph g;
    CHECK(Build("p = (e) => e.index * 10\ng = points(3)\ng = set_position(g, vec(p, 0, 0))\n"
                "h = set_position(g, vec(position.x, at(g, index - 1).position.x, 0))\noutput(h)\n",
                &g));
    const auto r = Evaluate(g, Ctx());
    CHECK(r.elements.size() == 3);
    if (r.elements.size() == 3) {
        CHECK_NEAR(r.elements[2].pos[0], 20, 0);
        CHECK_NEAR(r.elements[2].pos[1], 10, 0);
        CHECK_NEAR(r.elements[0].pos[1], 0, 0);
    }
    std::string e;
    CHECK(!Build("p = (e) => position\n", &g, &e) && Has(e, "e.position"));
}

TEST(each_if_once)
{
    Graph g;
    CHECK(Build("g = points(3)\ng = set_scale(g, vec(0.8, 0.8, 0.8), where: index == 0)\nh = each g {\n"
                "  if scale.x < 1 {\n    scale *= 2\n    position += vec(1, 0, 0)\n  }\n  old = position\n"
                "  position = vec(9, 0, 0)\n  store(\"was\", old)\n}\nk = set_position(h, was)\noutput(k)\n",
                &g));
    const auto r = Evaluate(g, Ctx());
    CHECK(r.elements.size() == 3);
    for (const auto& w : r.warnings) std::printf("  warning: %s\n", w.c_str());
    if (r.elements.size() == 3) {
        CHECK_NEAR(r.elements[0].scale[0], 1.6, 1e-12);
        CHECK_NEAR(r.elements[0].pos[0], 1, 0);   // moved although its scale is no longer < 1
        CHECK_NEAR(r.elements[1].scale[0], 1, 0);
        CHECK_NEAR(r.elements[1].pos[0], 0, 0);
    }
    const Node* z = g.FindKey("h");
    CHECK(z && z->type == "each");
    std::string e;
    CHECK(!Build("g = points(1)\nif true { }\n", &g, &e));
}

TEST(expression_from_v1)
{
    Graph g;
    std::string err;
    CHECK(Parse("DupliX graph 1\nnextid 3\nnode 1 Grid 0 0\nparam 1 countX 2\nparam 1 countY 1\nparam 1 spacingX 10 m2 2\n"
                "node 2 Output 0 0\nwire 1 2 0\n",
                &g, &err));
    // the group is evaluated from Task 7 on; here only the Expression node: 10 + 2 * m2 = 14
    const Node* e = g.FindKey("grid1.sx");
    CHECK(e && e->type == "expr");
    Graph h;
    CHECK(Build("g = points(2)\ng = set_position(g, vec(10 + 2 * m2, 0, 0))\noutput(g)\n", &h));
    const auto r = Evaluate(h, Ctx());
    CHECK(r.elements.size() == 2 && r.elements[1].pos[0] == 14);
}
