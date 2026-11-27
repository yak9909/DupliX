// DupliX core: Advanced3D's (classic 3D) pair test of two surfaces, rebuilt instruction by instruction from its code
// (Advanced3D 2025: sub_18002B040 -> sub_18002B350, with sub_18002B1B0 and sub_18002BC80; dev log 2026-11-26 8d).
// It decides the edges of AE's drawing-order graph: aFront ("a is in front somewhere") and bFront. No After Effects
// dependency: the caller passes the surfaces' data as AE keeps it.
#pragma once

namespace rnd {

// One motion sample of a surface: its screen polygon (n vertices, x at v[4 k], y at v[4 k + 1]: AE's 32-byte vertex
// records) and its plane: camera depth relation (a, b, c, d) as AE stores it at +0x18 .. +0x30 of the sample.
struct AePairSample {
    const double* v = nullptr;
    int n = 0;
    double a = 0, b = 0, c = 0, d = 0;
};

// sub_18002B350: the two samples' polygons where they meet (edge crossings, points on each other's edges, vertices
// inside the other) and the sign of the depth difference there. Sets *f1 / *f2 (never clears them). false: a polygon
// has more vertices than the test keeps on the stack (the caller asks AE instead).
bool AePairSamples(const AePairSample& A, const AePairSample& B, unsigned char* f1, unsigned char* f2);

// The same test with each sample prepared once (11/27, many pairs of the same surfaces): its vertices copied next to
// each other and the plane's quotients a / c, b / c, d / c (the very divisions AePairSamples makes for every pair, so
// the answers are the same bit for bit). No path counts (AePairCoverage) on this one.
struct AePairPrep {
    const double* v = nullptr;  // AE's vertex records (32 bytes each), or a copy of them
    int n = 0;
    double qa = 0, qb = 0, qd = 0;
};
AePairPrep AePairPrepare(const AePairSample& s);
bool AePairPrepared(const AePairPrep& A, const AePairPrep& B, unsigned char* f1, unsigned char* f2);

// Diagnostics (tests): this thread's counts of the paths taken: 0 collinear edges, 1 / 2 a's start / end on b's line,
// 3 / 4 b's start / end on a's line, 5 a crossing, 6 / 7 a vertex of a / b inside the other
#define AE_PAIR_COV_N 8
const long long* AePairCoverage();

// sub_18002B040's screen box test: boxes x0, y0, x1, y1 (+8 .. +0x20 of a surface); true when the samples are tested.
bool AePairBoxes(const double a[4], const double b[4]);

}  // namespace rnd
