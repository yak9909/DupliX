// DupliX 2 (design v5, 8.3, 14, 15): the editor's operations on a graph. The editor's views (nodes, stack, code)
// call only these, and so do the tests (RunScript), so what a test drives is what the user drives.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dxg5/graph.h"
#include "dxg5/lang_ast.h"

namespace dx5 {

// A macro whose value must be set in After Effects together with the graph (in the same undo step).
struct MacroWrite {
    bool isInt = false;
    int k = -1;  // 0..31
    double value = 0;
};

uint32_t AddNodeAt(Graph& g, const std::string& type, double x, double y);
// Removes the node, the wires from it, and (an Each zone) the nodes inside it.
bool RemoveNode(Graph& g, uint32_t id);
bool MoveNode(Graph& g, uint32_t id, double x, double y);
// Feeds `input` of `to` from `output` of `from` (replacing what fed it). Refused (with why) for a type that does
// not fit, a field into an input that takes single values, or a loop.
bool Connect(Graph& g, uint32_t from, int output, uint32_t to, const std::string& input, std::string* why);
bool Disconnect(Graph& g, uint32_t to, const std::string& input);
// Sets an unwired value (the wire is taken off). Structural inputs are rounded, choices kept in range.
bool SetConst(Graph& g, uint32_t id, const std::string& input, double v);
// A value typed in the editor (design v5 8.3): a number or a formula of numbers is set as the value; any other
// formula becomes an Expression node wired into the input. Refused with why.
bool SetExpression(Graph& g, uint32_t id, const std::string& input, const std::string& text, std::string* why);
// Wires Macro k+1 (or Int Macro k+1) into the input (one Macro node per macro, shared).
bool LinkMacro(Graph& g, uint32_t id, const std::string& input, bool isInt, int k);
// "Expose" (design v4 3): a free macro (an Int Macro for an Int input) takes the input's value now (returned in
// `write`), the input's name and its slider range, and is wired in. False for a choice, a wired input, or no free
// macro.
bool Expose(Graph& g, uint32_t id, const std::string& input, MacroWrite* write);
bool RenameMacro(Graph& g, bool isInt, int k, const std::string& name);
bool SetMacroRange(Graph& g, bool isInt, int k, double lo, double hi);
// The lowest macro no node reads and that has no name, or -1.
int FreeMacro(const Graph& g, bool isInt);
// Bits of the macros / layers the graph reads.
uint32_t UsedMacros(const Graph& g);
uint32_t UsedIntMacros(const Graph& g);
uint32_t UsedLayers(const Graph& g);
// Is this output a field (a value per element)?
bool IsField(const Graph& g, uint32_t node, int output);

// An Expression node made into one Math node per operation, and back (design v5 8.3).
bool ExpandExpression(Graph& g, uint32_t exprNode);
// The nodes (Math, Expression, attribute reads) with one output used outside them become one Expression node.
bool CollapseToExpression(Graph& g, const std::vector<uint32_t>& nodes);

// The code view's Apply: the graph read from `code` (keys kept from `g`). On failure `g` is untouched.
bool ApplyCode(Graph& g, const std::string& code, std::vector<Diag>* errors, std::vector<Diag>* notes);

// ---- presets ----
struct Preset {
    Graph graph;
    std::vector<MacroWrite> macros;  // the macro values that go with it
};
// The graph a new instance starts with: a 5 x 5 grid; Count (Int Macro 1), Spacing (Macro 2), Copy Scale (Macro 3).
// The effect's defaults (Int Macro 1 = 5, Macro 2 = 100, Macro 3 = 100) are its values, so it draws as it is.
Preset DefaultPreset();
Preset CirclePreset();
struct PresetInfo {
    const char* name;
    Preset (*make)();
};
const std::vector<PresetInfo>& Presets();
// The effect's defaults: Macro 1..32 (5, 100, 100, then 0: Macro 1-3 keep P1's defaults) and Int Macro 1..32 (5, 0...).
double MacroDefault(int k);
int64_t IntMacroDefault(int k);

// ---- scripts (tests, and the editor's test hook) ----
// One command per line:
//   clear | preset <name> | add <Type> <x> <y> [as <alias>] | remove <n> | move <n> <x> <y>
//   wire <from> <to> | unwire <to> | set <n> <input> <value> | expr <n> <input> <formula>
//   link <n> <input> m<k>|i<k> | unlink <n> <input> | expose <n> <input>
//   name m<k>|i<k> <name> | range m<k>|i<k> <lo> <hi> | value m<k>|i<k> <v>
//   code ... end (the lines between applied as code)
// <n> is an alias, a key or a node id; inputs may be P1's names (countX, rotZ, ...). '#' starts a comment.
bool RunScript(Graph& g, const std::string& script, std::vector<MacroWrite>* writes, std::string* error);

// ---- the stack view (design v5 14) ----
struct StackRow {
    uint32_t node = 0;
    int depth = 0;               // indent (branches of a node with more than one geometry input)
    bool collapsedGraph = false; // the part from here up is not a tree: shown as one row that opens the node view
};
// Output first going up its geometry inputs, listed upstream first (the top row acts first).
std::vector<StackRow> StackRows(const Graph& g);

}  // namespace dx5
