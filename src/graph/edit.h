// DupliX 2 (design v4, 1.2 and 3): the editor's operations on a graph. The editor's mouse and keyboard handlers
// call only these, and so do the tests (RunScript), so what a test drives is what the user drives.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "graph/graph.h"

namespace dxg {

// A macro whose value must be set in After Effects together with the graph (in the same undo step).
struct MacroWrite {
    int k = -1;       // 0..31
    double value = 0;
};

// Adds a node of `type` at (x, y); returns its id (0 if the type is unknown or a second Output is asked for).
uint32_t AddNode(Graph& g, NodeType type, double x, double y);
// Removes the node and every wire to or from it.
bool RemoveNode(Graph& g, uint32_t id);
bool MoveNode(Graph& g, uint32_t id, double x, double y);
// Feeds `from`'s output into `to`'s input `port` (replacing what fed it). Refused if it would make a loop.
bool Connect(Graph& g, uint32_t from, uint32_t to, int port);
bool Disconnect(Graph& g, uint32_t to, int port);
// Sets a param's base value. Structural params are rounded (design v4 2).
bool SetValue(Graph& g, uint32_t id, const std::string& key, double v);
// Links macro k (0-based) to a param with `depth` (adds or changes the link).
bool LinkMacro(Graph& g, uint32_t id, const std::string& key, int k, double depth);
bool UnlinkMacro(Graph& g, uint32_t id, const std::string& key, int k);
// "Expose" (design v4 3): the param is driven by a free macro at depth 1 from base 0; the macro takes the
// param's value now (returned in `write`, to be set in AE in the same undo step), its name (the param's label
// unless `name` is given) and its slider range (the param's). Returns false if no macro is free.
bool Expose(Graph& g, uint32_t id, const std::string& key, const std::string& name, MacroWrite* write);
bool RenameMacro(Graph& g, int k, const std::string& name);
bool SetMacroRange(Graph& g, int k, double lo, double hi);
// The lowest macro no param links to and that has no name, or -1.
int FreeMacro(const Graph& g);
// Would feeding `from` into `to` make a loop?
bool MakesLoop(const Graph& g, uint32_t from, uint32_t to);

// ---- presets (design v4 1.2) ----
struct Preset {
    Graph graph;
    std::vector<MacroWrite> macros;  // the macro values that go with it
};
// The graph a new instance starts with: a 5 x 5 grid; Count (Macro 1), Spacing (Macro 2) and Copy Scale (Macro 3)
// named and linked. The effect's Macro 1-3 defaults (5, 100, 100) are its values, so it draws as it is.
Preset DefaultPreset();
// A circle: Count (Macro 1), Radius (Macro 2), Copy Scale (Macro 3).
Preset CirclePreset();
struct PresetInfo {
    const char* name;
    Preset (*make)();
};
const std::vector<PresetInfo>& Presets();
// The effect's defaults for Macro 1-32 (what DefaultPreset expects).
double MacroDefault(int k);

// ---- scripts (tests, and the editor's own test hook) ----
// One command per line:
//   clear | preset <name> | add <Type> <x> <y> [as <alias>] | remove <n> | move <n> <x> <y>
//   wire <from> <to> [port] | unwire <to> [port] | set <n> <key> <value>
//   link <n> <key> m<k> <depth> | unlink <n> <key> m<k> | expose <n> <key> [name]
//   name m<k> <name> | range m<k> <lo> <hi> | value m<k> <v> (a macro's value in AE)
// <n> is a node id or an alias; '#' starts a comment. Every macro value a command asks for is appended to `writes`.
bool RunScript(Graph& g, const std::string& script, std::vector<MacroWrite>* writes, std::string* error);

}  // namespace dxg
