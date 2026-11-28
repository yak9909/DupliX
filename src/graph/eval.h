// DupliX 2 (design v4, section 2): evaluating a graph. The result depends only on the graph, the time, the seed,
// the macro values and which layers are set (never on an earlier frame), so frames can be computed in any order
// and at the same time on several threads.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "graph/graph.h"

namespace dxg {

// One element (one copy). Positions are in layer pixels, the origin at the layer's centre, x right, y down, z away
// from the viewer (After Effects' axes). `basis` is the copy's orientation (3x3, row major, columns = the copy's
// x, y, z axes); `scale` is relative to the copy's Source at Copy Scale 100%.
struct Element {
    uint64_t id = 0;       // fixed by the path it was made on (design v4 2.4): random values come from it
    uint32_t index = 0;    // its place in the list
    double pos[3] = {0, 0, 0};
    double basis[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    double scale[3] = {1, 1, 1};
    double opacity = 1;    // 0..1
    int source = -1;       // 0..7 = Layer 1..8, -1 = the effect's own layer
    double weight = 1;     // how much Modify acts on it (0..1)
    bool visible = true;
    double q = 0;          // the Order value (0..1): Index for now
    int s = 0;             // the step (the place in the list)
};

struct EvalContext {
    double time = 0;   // seconds (layer time)
    int seed = 0;      // the effect's Random Seed
    // a numeric macro's value (k = 0..31) at time t. Called with other times than `time` later (design v4 5: the
    // elements' birth times), so it is a function, not a table.
    std::function<double(int k, double t)> macro;
    uint32_t layersSet = 0;      // bit k: Layer k+1 has a layer chosen
    size_t maxElements = 100000; // the most elements a graph may make (the rest are cut, with a warning)
};

struct EvalResult {
    std::vector<Element> elements;
    double copyScale = 1;               // the Output's Copy Scale (1 = 100%)
    std::vector<std::string> warnings;  // e.g. "cut to 100000 elements"
    bool hasOutput = false;
};

EvalResult Evaluate(const Graph& g, const EvalContext& ctx);

// The elements as text (one line each), the same in the effect's dump (DUPLIX2_DUMP) and in graph_test's eval,
// so a test compares the two line by line.
std::string FormatElements(const EvalResult& r);

// The value of one input at the context's time (base + sum of depth x macro).
double InputValue(const Input& in, const EvalContext& ctx, double t);

// Structural inputs (counts, choices) are rounded to the nearest integer (halves away from zero) and clamped:
// a keyed macro driving Count changes the count only when its value passes n + 0.5.
int RoundCount(double v, int lo, int hi);

// hash(ID, seed) helpers (design v4 2.4).
uint64_t MixId(uint64_t a, uint64_t b);
double Rand01(uint64_t id, int seed, uint32_t salt);

// R = Rz * Ry * Rx (degrees), After Effects' order for X / Y / Z Rotation, row major.
void RotationXYZ(double rx, double ry, double rz, double out[9]);

}  // namespace dxg
