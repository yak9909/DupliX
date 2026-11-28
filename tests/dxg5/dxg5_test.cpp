// DupliX 2 (design v5): tests of the dx5 core (src/dxg5), with no After Effects.
//   dxg5_test.exe                        run the tests ("ALL PASS" at the end); `only <part>` runs some
//   dxg5_test.exe eval <graph file> <t> [m<k>=<v>] [i<k>=<v>] [seed=<n>] [layers=<mask>]
//                                        print the elements the graph makes (the AE test's answer)
//   dxg5_test.exe script <script file> <graph file>   run an editor script on a new instance's graph, write it
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "dxg5/edit.h"
#include "dxg5/eval.h"
#include "test.h"

int g_failed = 0;
int g_checks = 0;
const char* g_test = "";

namespace {
struct Entry {
    const char* name;
    TestFn fn;
};
std::vector<Entry>& Tests()
{
    static std::vector<Entry> t;
    return t;
}
}  // namespace

void RegisterTest(const char* name, TestFn fn) { Tests().push_back({name, fn}); }

static int RunTests(const char* only)
{
    for (const Entry& e : Tests()) {
        if (only && !std::strstr(e.name, only)) continue;
        g_test = e.name;
        const int before = g_failed;
        e.fn();
        std::printf("%s %s\n", g_failed == before ? "ok  " : "FAIL", g_test);
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    if (!g_failed) std::printf("ALL PASS\n");
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

int main(int argc, char** argv)
{
    using namespace dx5;
    if (argc >= 3 && !std::strcmp(argv[1], "only")) return RunTests(argv[2]);
    if (argc >= 4 && !std::strcmp(argv[1], "eval")) {
        std::string text, err;
        Graph g;
        if (!ReadFile(argv[2], &text) || !Parse(text, &g, &err)) {
            std::fprintf(stderr, "cannot read %s: %s\n", argv[2], err.c_str());
            return 2;
        }
        double m[32];
        int64_t im[32];
        for (int k = 0; k < 32; k++) m[k] = MacroDefault(k), im[k] = IntMacroDefault(k);
        EvalContext c;
        c.time = std::atof(argv[3]);
        for (int i = 4; i < argc; i++) {
            int k = 0;
            double v = 0;
            unsigned mask = 0;
            if (std::sscanf(argv[i], "m%d=%lf", &k, &v) == 2 && k >= 1 && k <= 32) m[k - 1] = v;
            else if (std::sscanf(argv[i], "i%d=%lf", &k, &v) == 2 && k >= 1 && k <= 32) im[k - 1] = (int64_t)std::llround(v);
            else if (std::sscanf(argv[i], "seed=%d", &k) == 1) c.seed = k;
            else if (std::sscanf(argv[i], "layers=%u", &mask) == 1) c.layersSet = mask;
        }
        c.macro = [&](int k, double) { return m[k]; };
        c.intMacro = [&](int k, double) { return im[k]; };
        std::fputs(dxg::FormatElements(Evaluate(g, c)).c_str(), stdout);
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
        for (const MacroWrite& x : w) std::printf("%c%d=%.17g\n", x.isInt ? 'i' : 'm', x.k + 1, x.value);
        return 0;
    }
    return RunTests(nullptr);
}
