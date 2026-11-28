// DupliX 2 (design v4): tests of the graph core (src/graph), with no After Effects.
//   graph_test.exe                                     run the tests
//   graph_test.exe eval <graph file> <t> [m<k>=<v>]... [seed=<n>] [layers=<mask>]
//                                                      print the elements the graph makes (the AE test's answer)
//   graph_test.exe script <script file> <graph file>   run an editor script on a new instance's graph, write it
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "graph/edit.h"
#include "graph/eval.h"
#include "graph/graph.h"

using namespace dxg;

static int g_failed = 0;
static int g_checks = 0;
static const char* g_test = "";

#define CHECK(cond)                                                               \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(cond)) {                                                            \
            ++g_failed;                                                           \
            std::printf("FAIL %s: %s (line %d)\n", g_test, #cond, __LINE__);      \
        }                                                                         \
    } while (0)
#define CHECK_NEAR(a, b, eps)                                                                              \
    do {                                                                                                   \
        ++g_checks;                                                                                        \
        const double va = (a), vb = (b);                                                                   \
        if (!(std::fabs(va - vb) <= (eps))) {                                                              \
            ++g_failed;                                                                                    \
            std::printf("FAIL %s: %s = %.9g, want %.9g (line %d)\n", g_test, #a, va, vb, __LINE__);         \
        }                                                                                                  \
    } while (0)

using TestFn = void (*)();
static struct {
    const char* name;
    TestFn fn;
} g_tests[128];
static int g_numTests = 0;
static void Register(const char* n, TestFn f) { g_tests[g_numTests++] = {n, f}; }
#define TEST(name)                                         \
    static void name();                                    \
    struct name##_reg {                                    \
        name##_reg() { Register(#name, name); }            \
    } name##_inst;                                         \
    static void name()

static EvalContext Ctx(double m1 = 5, double m2 = 100, double m3 = 100)
{
    EvalContext c;
    c.macro = [=](int k, double) { return k == 0 ? m1 : k == 1 ? m2 : k == 2 ? m3 : 0.0; };
    return c;
}

TEST(default_preset_is_a_5x5_grid)
{
    const Preset p = DefaultPreset();
    const EvalResult r = Evaluate(p.graph, Ctx());
    CHECK(r.hasOutput);
    CHECK(r.elements.size() == 25);
    CHECK_NEAR(r.copyScale, 1.0, 1e-12);
    CHECK_NEAR(r.elements[0].pos[0], -200, 1e-9);
    CHECK_NEAR(r.elements[0].pos[1], -200, 1e-9);
    CHECK_NEAR(r.elements[24].pos[0], 200, 1e-9);
    CHECK_NEAR(r.elements[24].pos[1], 200, 1e-9);
    CHECK_NEAR(r.elements[6].pos[0], -100, 1e-9);
    CHECK(MacroLabel(p.graph, 0) == "Count");
    CHECK(MacroLabel(p.graph, 7) == "Macro 8");
    CHECK(UsedMacros(p.graph) == 7u);
    // the preset's macro values are the effect's defaults (a new instance draws without a write)
    for (const MacroWrite& w : p.macros) CHECK_NEAR(w.value, MacroDefault(w.k), 0);
}

TEST(count_macro_rounds_at_half)
{
    const Graph g = DefaultPreset().graph;
    CHECK(Evaluate(g, Ctx(3)).elements.size() == 9);
    CHECK(Evaluate(g, Ctx(3.49)).elements.size() == 9);
    CHECK(Evaluate(g, Ctx(3.5)).elements.size() == 16);
    CHECK(Evaluate(g, Ctx(0)).elements.size() == 0);
    CHECK(Evaluate(g, Ctx(-4)).elements.size() == 0);
    CHECK(RoundCount(2.5, 0, 10) == 3);
    CHECK(RoundCount(1e30, 0, 10) == 10);
    CHECK(RoundCount(std::nan(""), 1, 10) == 1);
}

TEST(spacing_and_copy_scale_macros)
{
    const Graph g = DefaultPreset().graph;
    const EvalResult r = Evaluate(g, Ctx(2, 50, 25));
    CHECK(r.elements.size() == 4);
    CHECK_NEAR(r.elements[0].pos[0], -25, 1e-9);
    CHECK_NEAR(r.elements[3].pos[1], 25, 1e-9);
    CHECK_NEAR(r.copyScale, 0.25, 1e-12);
}

TEST(round_trip)
{
    Graph g = DefaultPreset().graph;
    g.revision = 41;
    RenameMacro(g, 5, "Name With  Spaces");
    SetMacroRange(g, 5, -2.5, 1e6);
    SetValue(g, 1, "spacingZ", 0.1);  // not exact in binary: must come back the same
    const std::string a = Serialize(g);
    Graph h;
    std::string err;
    CHECK(Parse(a, &h, &err));
    CHECK(Serialize(h) == a);
    CHECK(h.revision == 41);
    CHECK(h.macros[5].name == "Name With  Spaces");
    CHECK(h.FindNode(1)->Find("spacingZ")->in.base == 0.1);
    CHECK(h.nextId == g.nextId);
    // CRLF is read the same
    std::string crlf;
    for (char c : a) crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
    Graph k;
    CHECK(Parse(crlf, &k));
    CHECK(Serialize(k) == a);
}

TEST(unknown_things_are_kept)
{
    const std::string text =
        "DupliX graph 7\n"
        "revision 3\n"
        "future thing 1 2 3\n"
        "node 1 Grid 0 0\n"
        "node 2 Wobble 10 20\n"
        "param 2 amount 0.5 m4 2\n"
        "param 1 countX 2\n"
        "param 1 newKnob 9\n"
        "node 3 Output 0 0\n"
        "wire 1 2 0\n"
        "wire 2 3 0\n"
        "wire 1 99 0\n";
    Graph g;
    std::string err;
    CHECK(Parse(text, &g, &err));
    CHECK(g.nodes.size() == 3);
    CHECK(g.FindNode(2)->type == NodeType::Unknown);
    CHECK(g.FindNode(2)->typeName == "Wobble");
    CHECK(g.wires.size() == 2);  // the wire to node 99 is dropped
    CHECK(g.unknownLines.size() == 1);
    CHECK(g.nextId == 4);
    const std::string s = Serialize(g);
    CHECK(s.find("future thing 1 2 3") != std::string::npos);
    CHECK(s.find("node 2 Wobble 10 20") != std::string::npos);
    CHECK(s.find("param 2 amount 0.5 m4 2") != std::string::npos);
    CHECK(s.find("param 1 newKnob 9") != std::string::npos);
    CHECK(UsedMacros(g) == 1u << 3);
    // an unknown node passes nothing
    CHECK(Evaluate(g, Ctx()).elements.empty());
    Graph h;
    CHECK(!Parse("hello\n", &h, &err));
    CHECK(!Parse("", &h, &err));
    CHECK(!Parse("DupliX graph 0\n", &h, &err));
}

TEST(same_result_in_any_order)
{
    // Count driven by a macro that changes over time: frames computed in any order give the same elements
    Graph g = DefaultPreset().graph;
    EvalContext c;
    c.macro = [](int k, double t) { return k == 0 ? 1 + t * 2 : k == 1 ? 50 + 10 * t : 100.0; };
    std::string fwd[4], back[4];
    auto dump = [&](double t) {
        c.time = t;
        const EvalResult r = Evaluate(g, c);
        std::string s;
        for (const Element& e : r.elements) {
            char b[160];
            std::snprintf(b, sizeof(b), "%llx %.17g %.17g %.17g\n", (unsigned long long)e.id, e.pos[0], e.pos[1], e.pos[2]);
            s += b;
        }
        return s;
    };
    for (int i = 0; i < 4; i++) fwd[i] = dump(i * 0.5);
    for (int i = 3; i >= 0; i--) back[i] = dump(i * 0.5);
    for (int i = 0; i < 4; i++) CHECK(fwd[i] == back[i]);
    CHECK(fwd[0] != fwd[3]);
}

TEST(circle_line_transform_modify)
{
    Graph g;
    std::string err;
    std::vector<MacroWrite> w;
    CHECK(RunScript(g,
                    "add Circle 0 0 as c\n"
                    "add Output 100 0 as o\n"
                    "wire c o\n"
                    "set c count 4\n"
                    "set c radius 100\n",
                    &w, &err));
    EvalResult r = Evaluate(g, Ctx());
    CHECK(r.elements.size() == 4);
    CHECK_NEAR(r.elements[0].pos[0], 0, 1e-9);
    CHECK_NEAR(r.elements[0].pos[1], -100, 1e-9);  // 0 degrees = up
    CHECK_NEAR(r.elements[1].pos[0], 100, 1e-9);   // clockwise
    CHECK_NEAR(r.elements[1].pos[1], 0, 1e-9);
    CHECK_NEAR(r.elements[3].pos[0], -100, 1e-9);
    // an arc includes both ends
    // aliases live for one script: later scripts use the ids (the circle is node 1)
    CHECK(RunScript(g, "set 1 sweep 180\nset 1 count 3\n", &w, &err));
    r = Evaluate(g, Ctx());
    CHECK_NEAR(r.elements[2].pos[1], 100, 1e-9);
    // Align turns the copies outward
    CHECK(RunScript(g, "set 1 align 1\n", &w, &err));
    r = Evaluate(g, Ctx());
    CHECK_NEAR(r.elements[1].basis[0], 0, 1e-12);   // 90 degrees about Z
    CHECK_NEAR(r.elements[1].basis[3], 1, 1e-12);

    Graph h;
    CHECK(RunScript(h,
                    "add Line 0 0 as l\nset l count 3\nset l length 200\n"
                    "add Transform 0 0 as t\nset t rotZ 90\nset t posX 10\n"
                    "add Modify 0 0 as m\nset m scale -50\nset m opacity -75\nset m posY 5\n"
                    "add Output 0 0 as o\nwire l t\nwire t m\nwire m o\n",
                    &w, &err));
    r = Evaluate(h, Ctx());
    CHECK(r.elements.size() == 3);
    // the line (-100..100 on x) turned 90 degrees about its centre: along y
    CHECK_NEAR(r.elements[0].pos[0], 10, 1e-9);
    CHECK_NEAR(r.elements[0].pos[1], -100 + 5, 1e-9);
    CHECK_NEAR(r.elements[2].pos[1], 100 + 5, 1e-9);
    CHECK_NEAR(r.elements[0].scale[0], 0.5, 1e-12);
    CHECK_NEAR(r.elements[0].opacity, 0.25, 1e-12);
    CHECK_NEAR(r.elements[0].basis[3], 1, 1e-12);
    // scale and opacity never go below 0
    CHECK(RunScript(h, "set 3 scale -300\nset 3 opacity -300\n", &w, &err));  // node 3 is the Modify
    r = Evaluate(h, Ctx());
    CHECK_NEAR(r.elements[0].scale[0], 0, 0);
    CHECK_NEAR(r.elements[0].opacity, 0, 0);
}

TEST(expose_gives_a_free_macro)
{
    Preset p = CirclePreset();
    Graph& g = p.graph;
    MacroWrite w;
    CHECK(Expose(g, 1, "startAngle", "", &w));
    CHECK(w.k == 3);
    CHECK_NEAR(w.value, 0, 0);
    CHECK(g.macros[3].name == "Start Angle");
    CHECK_NEAR(g.macros[3].sliderMin, -180, 0);
    CHECK_NEAR(g.macros[3].sliderMax, 180, 0);
    const Param* sa = g.FindNode(1)->Find("startAngle");
    CHECK(sa->in.base == 0 && sa->in.links.size() == 1 && sa->in.links[0].src == "m4");
    // a value outside the param's slider range widens the macro's
    SetValue(g, 1, "sweep", 720);
    CHECK(Expose(g, 1, "sweep", "Turns", &w));
    CHECK(w.k == 4 && w.value == 720 && g.macros[4].sliderMax == 720 && g.macros[4].name == "Turns");
    // a choice cannot be exposed or linked
    CHECK(!Expose(g, 1, "align", "", &w));
    CHECK(!LinkMacro(g, 1, "align", 9, 1));
    // the circle is drawn the same before and after (the macro takes the value)
    EvalContext c;
    c.macro = [](int k, double) { return k == 0 ? 12 : k == 1 ? 300 : k == 2 ? 100 : 0.0; };
    const EvalResult r = Evaluate(CirclePreset().graph, c);
    CHECK(r.elements.size() == 12);
    CHECK_NEAR(r.elements[3].pos[0], 300, 1e-9);
}

TEST(wires_refuse_loops_and_a_second_output)
{
    Graph g;
    const uint32_t a = AddNode(g, NodeType::Grid, 0, 0);
    const uint32_t t = AddNode(g, NodeType::Transform, 0, 0);
    const uint32_t m = AddNode(g, NodeType::Modify, 0, 0);
    const uint32_t o = AddNode(g, NodeType::Output, 0, 0);
    CHECK(AddNode(g, NodeType::Output, 0, 0) == 0);
    CHECK(Connect(g, a, t, 0));
    CHECK(Connect(g, t, m, 0));
    CHECK(!Connect(g, m, t, 0));  // a loop
    CHECK(!Connect(g, a, a, 0));
    CHECK(!Connect(g, o, m, 0));  // Output has no output
    CHECK(!Connect(g, a, o, 1));  // no such input
    CHECK(!Connect(g, t, a, 0));  // Grid has no input
    CHECK(Connect(g, m, o, 0));
    CHECK(Connect(g, a, o, 0));   // replaces what fed the input
    CHECK(g.wires.size() == 3);
    CHECK(g.WireInto(o, 0)->from == a);
    CHECK(RemoveNode(g, t));
    CHECK(g.wires.size() == 1);
}

TEST(source_assigns_the_set_layers)
{
    Graph g;
    std::string err;
    CHECK(RunScript(g,
                    "add Line 0 0 as l\nset l count 5\nadd Source 0 0 as s\nset s count 3\n"
                    "add Output 0 0 as o\nwire l s\nwire s o\n",
                    nullptr, &err));
    EvalContext c = Ctx();
    c.layersSet = 0b101;  // Layer 1 and Layer 3
    EvalResult r = Evaluate(g, c);
    CHECK(r.elements.size() == 5);
    CHECK(r.elements[0].source == 0 && r.elements[1].source == 2 && r.elements[2].source == 0);
    c.layersSet = 0;
    r = Evaluate(g, c);
    CHECK(r.elements[1].source == -1);  // nothing chosen: this layer
    CHECK(UsedLayers(g) == 0b111);
    // Random is fixed by the element's id and the seed
    CHECK(RunScript(g, "set 2 assign 1\n", nullptr, &err));  // node 2 is the Source
    c.layersSet = 0b111;
    const EvalResult a = Evaluate(g, c), b = Evaluate(g, c);
    for (size_t i = 0; i < a.elements.size(); i++) CHECK(a.elements[i].source == b.elements[i].source);
}

TEST(ids_are_fixed_by_the_path)
{
    const Graph g = DefaultPreset().graph;
    const EvalResult a = Evaluate(g, Ctx(5)), b = Evaluate(g, Ctx(5));
    for (size_t i = 0; i < a.elements.size(); i++) CHECK(a.elements[i].id == b.elements[i].id);
    for (size_t i = 1; i < a.elements.size(); i++) CHECK(a.elements[i].id != a.elements[i - 1].id);
    CHECK(Rand01(a.elements[0].id, 1, 0) == Rand01(a.elements[0].id, 1, 0));
    CHECK(Rand01(a.elements[0].id, 1, 0) != Rand01(a.elements[0].id, 2, 0));
    CHECK(MixId(1, 2) != MixId(2, 1));
}

TEST(element_cap)
{
    Graph g;
    std::string err;
    CHECK(RunScript(g, "add Grid 0 0 as g\nset g countX 1000\nset g countY 1000\nadd Output 0 0 as o\nwire g o\n", nullptr,
                    &err));
    EvalContext c = Ctx();
    c.maxElements = 100000;
    const EvalResult r = Evaluate(g, c);
    CHECK(r.elements.size() == 100000);
    CHECK(!r.warnings.empty());
}

TEST(script_errors)
{
    Graph g;
    std::string err;
    CHECK(!RunScript(g, "add Nope 0 0\n", nullptr, &err));
    CHECK(err.find("line 1") != std::string::npos);
    CHECK(!RunScript(g, "frobnicate\n", nullptr, &err));
    std::vector<MacroWrite> w;
    CHECK(RunScript(g, "preset Circle\nvalue m5 7 # a comment\n", &w, &err));
    CHECK(w.size() == 4 && w[3].k == 4 && w[3].value == 7);
}

static int RunTests()
{
    for (int i = 0; i < g_numTests; i++) {
        g_test = g_tests[i].name;
        const int before = g_failed;
        g_tests[i].fn();
        std::printf("%s %s\n", g_failed == before ? "ok  " : "FAIL", g_test);
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}

static bool ReadFile(const char* path, std::string* out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

static void PrintElements(FILE* fp, const EvalResult& r) { std::fputs(FormatElements(r).c_str(), fp); }

int main(int argc, char** argv)
{
    if (argc >= 4 && !std::strcmp(argv[1], "eval")) {
        std::string text;
        Graph g;
        std::string err;
        if (!ReadFile(argv[2], &text) || !Parse(text, &g, &err)) {
            std::fprintf(stderr, "cannot read %s: %s\n", argv[2], err.c_str());
            return 2;
        }
        double m[kNumMacros];
        for (int k = 0; k < kNumMacros; k++) m[k] = MacroDefault(k);
        EvalContext c;
        c.time = std::atof(argv[3]);
        for (int i = 4; i < argc; i++) {
            int k = 0;
            double v = 0;
            unsigned mask = 0;
            if (std::sscanf(argv[i], "m%d=%lf", &k, &v) == 2 && k >= 1 && k <= kNumMacros) m[k - 1] = v;
            else if (std::sscanf(argv[i], "seed=%d", &k) == 1) c.seed = k;
            else if (std::sscanf(argv[i], "layers=%u", &mask) == 1) c.layersSet = mask;
        }
        c.macro = [&](int k, double) { return m[k]; };
        PrintElements(stdout, Evaluate(g, c));
        return 0;
    }
    if (argc >= 4 && !std::strcmp(argv[1], "script")) {
        std::string text, err;
        if (!ReadFile(argv[2], &text)) return 2;
        Graph g = DefaultPreset().graph;
        std::vector<MacroWrite> w;
        if (!RunScript(g, text, &w, &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        std::ofstream(argv[3], std::ios::binary) << Serialize(g);
        for (const MacroWrite& x : w) std::printf("m%d=%.17g\n", x.k + 1, x.value);
        return 0;
    }
    return RunTests();
}
