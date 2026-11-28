// DupliX 2 (design v5, 5 and 13): the evaluator's inside, shared by the files that evaluate parts of it (the
// Expression node's formula, the built-in groups). Not for the effect: use eval.h.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "dxg5/eval.h"
#include "dxg5/geometry.h"

namespace dx5 {

using GeoPtr = std::shared_ptr<const Geometry>;

class Evaluator {
public:
    Evaluator(const Graph& g, const EvalContext& ctx, dxg::EvalResult& res) : g_(g), ctx_(ctx), res_(res) {}

    // The geometry an output of a node makes (once per evaluation).
    GeoPtr Geo(uint32_t node, int output);
    // A value output of a node over `ctx` (a column of ctx->count, or of 1 for a single value).
    Column Field(uint32_t node, int output, const GeoPtr& ctx);
    // An input of a node over `ctx`: its wire's field, or its constant as a single value.
    Column Input(const Node& n, const std::string& input, const GeoPtr& ctx);
    // A structural input: a single value (a field there is read as its first value, with a warning).
    Value Single(const Node& n, const std::string& input);
    // The geometry wired into an input (empty if none).
    GeoPtr InputGeo(const Node& n, const std::string& input);

    void Warn(const std::string& w);
    const EvalContext& Ctx() const { return ctx_; }
    const Graph& G() const { return g_; }
    dxg::EvalResult& Result() { return res_; }
    size_t Cap(size_t n);  // n cut to maxElements (with the warning)
    uint64_t ElementId(const Node& n, size_t i, const std::string& inner = "") const;

    // Evaluating a group's definition: this evaluator runs the group's graph; its param nodes read `node`'s inputs
    // through `outer`; element ids are made from the group node's key (design v5 11).
    void BindGroup(Evaluator* outer, const Node* node);

private:
    Column FieldUncached(const Node& n, int output, const GeoPtr& ctx);
    GeoPtr GeoUncached(const Node& n, int output);

    const Graph& g_;
    const EvalContext& ctx_;
    dxg::EvalResult& res_;
    std::map<std::pair<uint32_t, int>, GeoPtr> geo_;
    std::map<std::tuple<uint32_t, int, const Geometry*>, Column> field_;
    std::vector<GeoPtr> keep_;     // contexts used by field_ stay alive (their addresses are keys)
    std::vector<uint32_t> stack_;  // to stop at a loop
    Evaluator* outer_ = nullptr;   // a group's definition: the evaluator of the graph the group node is in
    const Node* outerNode_ = nullptr;
    std::string idPrefix_;         // the group node's key path
    uint32_t outerLegacy_ = 0;
};

// A built-in group's definition evaluated (dx5_groups.cpp). False if it has none.
bool GroupByDefinition(Evaluator& ev, const Node& n, Geometry* out);

// Element-wise operations of the math nodes and the Expression node ("add", "sin", ...): a, b, c are values of one
// element. False when the types do not fit (`err` says why).
bool MathOp(const std::string& op, const Value* args, int n, Value* out, std::string* err);

// The Expression node (dx5_expr.cpp): its formula over ctx.
Column EvalExpressionNode(Evaluator& ev, const Node& n, const GeoPtr& ctx, int output);
// A built-in group computed in C++ (dx5_groups.cpp). False when it must be evaluated from its definition.
bool NativeGroup(Evaluator& ev, const Node& n, Geometry* out);

}  // namespace dx5
