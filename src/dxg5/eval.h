// DupliX 2 (design v5, 5 and 13): evaluating a graph. The result depends only on the graph, the time, the seed,
// the macro values and which layers are set (never on an earlier frame).
#pragma once

#include <cstdint>
#include <functional>

#include "dxg5/graph.h"
#include "graph/eval.h"  // dxg::Element, dxg::EvalResult: the form the renderer and the dump already take

namespace dx5 {

struct EvalContext {
    double time = 0;   // seconds (layer time)
    int seed = 0;      // the effect's Random Seed
    std::function<double(int k, double t)> macro;      // Macro k+1 (k = 0..31) at time t
    std::function<int64_t(int k, double t)> intMacro;  // Int Macro k+1
    uint32_t layersSet = 0;       // bit k: Layer k+1 has a layer chosen
    size_t maxElements = 100000;  // the most elements a graph may make (the rest are cut, with a warning)
};

dxg::EvalResult Evaluate(const Graph& g, const EvalContext& ctx);

}  // namespace dx5
