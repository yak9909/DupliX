// DupliX 2 (design v5): tests of the dx5 core (src/dxg5), with no After Effects.
//   dxg5_test.exe                        run the tests ("ALL PASS" at the end)
#include <cstring>
#include <string>
#include <vector>

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

int main(int argc, char** argv)
{
    if (argc >= 3 && !std::strcmp(argv[1], "only")) return RunTests(argv[2]);
    return RunTests(nullptr);
}
