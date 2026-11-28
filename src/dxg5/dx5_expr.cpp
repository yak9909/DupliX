// DupliX 2 (design v5, 8.3): the Expression node's formula (Task 5 evaluates it with the language's parser).
#include "dxg5/eval_internal.h"

namespace dx5 {

Column EvalExpressionNode(Evaluator& ev, const Node& n, const GeoPtr& ctx)
{
    ev.Warn(n.key + ": expressions are not evaluated yet");
    return MakeColumn(Type::Float, ctx->count, Value::F(0));
}

}  // namespace dx5
