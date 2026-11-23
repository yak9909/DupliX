// Prints the per-copy random draws as JSON, for building reference comps of real duplicate layers.
//   randamu_dump --count N --seed S --pos X Y Z --rot X Y Z --scale MIN MAX --opacity MIN MAX [--keep-first]
// Output: {"copies":[{"t":[tx,ty,tz],"r":[rx,ry,rz],"s":s,"o":o}, ...]}  (px, degrees, percent, percent)
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/scatter.h"

static bool Num(int argc, char** argv, int& i, int n, double* out)
{
    if (i + n >= argc) return false;
    for (int k = 0; k < n; k++) out[k] = std::atof(argv[++i]);
    return true;
}

int main(int argc, char** argv)
{
    rnd::ScatterParams p;
    p.count = 5;
    for (int i = 1; i < argc; i++) {
        double v[3];
        bool ok = true;
        if (!std::strcmp(argv[i], "--count")) { ok = Num(argc, argv, i, 1, v); p.count = (int)v[0]; }
        else if (!std::strcmp(argv[i], "--seed")) { ok = Num(argc, argv, i, 1, v); p.seed = (int)v[0]; }
        else if (!std::strcmp(argv[i], "--pos")) { ok = Num(argc, argv, i, 3, p.pos); }
        else if (!std::strcmp(argv[i], "--rot")) { ok = Num(argc, argv, i, 3, p.rotDeg); }
        else if (!std::strcmp(argv[i], "--scale")) { ok = Num(argc, argv, i, 2, v); p.scaleMin = v[0]; p.scaleMax = v[1]; }
        else if (!std::strcmp(argv[i], "--opacity")) { ok = Num(argc, argv, i, 2, v); p.opacityMin = v[0]; p.opacityMax = v[1]; }
        else if (!std::strcmp(argv[i], "--keep-first")) { p.keepFirst = true; }
        else { std::fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
        if (!ok) { std::fprintf(stderr, "missing value for %s\n", argv[i]); return 2; }
    }
    std::printf("{\"copies\":[");
    for (int i = 0; i < p.count; i++) {
        rnd::CopyParams c = rnd::DrawCopy(p, i);
        std::printf("%s{\"t\":[%.17g,%.17g,%.17g],\"r\":[%.17g,%.17g,%.17g],\"s\":%.17g,\"o\":%.17g}", i ? "," : "",
                    c.t[0], c.t[1], c.t[2], c.r[0], c.r[1], c.r[2], c.s, c.o);
    }
    std::printf("]}\n");
    return 0;
}
