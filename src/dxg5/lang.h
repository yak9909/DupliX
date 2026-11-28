// DupliX 2 (design v5, 8): the code. CodeToGraph reads code into a graph (Task 5); GraphToCode writes a graph
// as code (Task 6). The code is the graph written out: a call is a node, an argument a wire.
#pragma once

#include <string>
#include <vector>

#include "dxg5/graph.h"
#include "dxg5/lang_ast.h"

namespace dx5 {

// Reads `code` into a new graph. Nodes whose key is the same as in `previous` keep its id, position, salt and
// legacy id; the macros' settings and the revision come from `previous`. False with `errors` (line and column)
// when the code cannot be read; `out` is then untouched. `notes` tell what the reader changed (renamed keys).
bool CodeToGraph(const std::string& code, const Graph& previous, Graph* out, std::vector<Diag>* errors,
                 std::vector<Diag>* notes);

// The graph as code (design v5 8.2): the same graph always gives the same text, and reading it back gives the
// same graph (SameGraph). Nodes the code cannot say are written as comments.
std::string GraphToCode(const Graph& g);
// A value output written as an expression (the stack view's fields).
std::string ExprText(const Graph& g, uint32_t node, int output);
// Design v5 13's "the same": node types and keys, Expression formulas (in one written form; an Expression feeding
// its only user folded into it), wires, unwired values, zones. Positions and ids are not compared.
bool SameGraph(const Graph& a, const Graph& b, std::string* why);

// ---- shared by the reader, the printer and the Expression node (dx5_exprtext.cpp) ----
// The expression as text with the fewest parentheses that keep its meaning ("a + b * c", "-(a + b)").
std::string PrintExpr(const Expr& e);
// The functions an Expression may call (element-wise math): sin, cos, vec, rot, floordiv, ...
bool IsMathFunction(const std::string& name);
// The math node / MathOp name of an operator ("+" -> "add"; unary "-" -> "neg", "!" -> "not").
std::string OpName(const std::string& sym, bool unary);
// The value of an expression made of numbers and math only (no names). False if it has names or cannot be done.
bool EvalConst(const Expr& e, Value* out);
// A number token's value: Int unless it has a '.' or an exponent (hex is Int).
Value NumberValue(const std::string& text);

}  // namespace dx5
