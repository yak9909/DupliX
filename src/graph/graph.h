// DupliX 2 (design v4): the graph data. Portable C++17 with no After Effects or Win32 includes, so the same code
// runs in the effect, in the editor, in the tests and (later) on the Mac.
//
// A graph is a set of nodes joined by wires (geometry flows from a node's output into another node's input) and
// the settings of the macros (their names and slider ranges; their values are the effect's Macro 1-32 parameters).
// Every number a node reads is an Input: a base value plus the sum of (depth x macro value) over its links.
//
// The stored form is UTF-8 text, one statement per line (Serialize / Parse). Lines the reader does not know are
// skipped, and nodes and settings of unknown types are kept as they are (written back unchanged), so an older
// build opens a newer graph without losing what it does not understand.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dxg {

constexpr int kFormatVersion = 1;
constexpr int kNumMacros = 32;       // Macro 1-32 (numbers)
constexpr int kNumColorMacros = 8;   // Color Macro 1-8 (not linkable yet)
constexpr int kNumPointMacros = 8;   // Point Macro 1-8 (not linkable yet)
constexpr int kNumLayers = 8;        // Layer 1-8

// ---- node types ----
enum class NodeType : int {
    Unknown = -1,  // a type this build does not know: kept and written back, passes no geometry
    Grid = 0,
    Circle,
    Line,
    Transform,
    Modify,
    Source,
    Output,
    Count_
};

struct ParamSpec {
    const char* key;      // the name in the stored form (never change it)
    const char* label;    // shown in the editor
    double dflt;
    double softMin, softMax;  // the editor's slider range
    bool structural;      // decides the number or order of elements: only constants and macros may drive it
    const char* choices;  // "A|B|C" for a choice (the value is the 0-based index), or nullptr for a number
};

struct NodeSpec {
    NodeType type;
    const char* name;     // the name in the stored form and the editor's title
    int numInputs;        // geometry inputs
    bool hasOutput;       // a geometry output (Output has none)
    std::vector<ParamSpec> params;
};

const NodeSpec* Spec(NodeType t);
const NodeSpec* SpecByName(const std::string& name);
const ParamSpec* FindParam(const NodeSpec* s, const std::string& key);

// ---- values ----
struct Link {
    std::string src;  // "m1".."m32" (a macro); other sources are kept but read as 0 by this build
    double depth = 1;
};

struct Input {
    double base = 0;
    std::vector<Link> links;
};

struct Param {
    std::string key;
    Input in;
};

struct Node {
    uint32_t id = 0;            // unique in the graph, never reused while the node lives
    NodeType type = NodeType::Unknown;
    std::string typeName;       // as stored (for Unknown nodes)
    double x = 0, y = 0;        // the editor's position
    std::vector<Param> params;  // every param of the type (filled with defaults) plus unknown ones as read

    Param* Find(const std::string& key);
    const Param* Find(const std::string& key) const;
};

struct Wire {
    uint32_t from = 0;  // the node whose output feeds
    uint32_t to = 0;    // the node whose input is fed
    int port = 0;       // which input of `to`
};

struct Macro {
    std::string name;            // empty = "Macro N"
    double sliderMin = 0, sliderMax = 100;
};

struct Graph {
    uint32_t revision = 0;       // +1 on every write from the editor (AE's undo is detected by it)
    uint32_t nextId = 1;
    std::vector<Node> nodes;
    std::vector<Wire> wires;
    Macro macros[kNumMacros];
    std::vector<std::string> unknownLines;  // statements this build does not know (written back)

    Node* FindNode(uint32_t id);
    const Node* FindNode(uint32_t id) const;
    const Wire* WireInto(uint32_t to, int port) const;
    const Node* OutputNode() const;  // the first Output node, or nullptr
};

// A new node of `type` with every param at its default (id 0: AddNode in edit.h gives the id).
Node MakeNode(NodeType type);

std::string Serialize(const Graph& g);
// Returns false only when the text is not a DupliX graph at all (then `out` is untouched). A newer format version
// is read as far as this build understands it.
bool Parse(const std::string& text, Graph* out, std::string* error = nullptr);

// "Macro 7" or the name the editor gave it.
std::string MacroLabel(const Graph& g, int k);
// The macros (0-based bits) and layers (0-based bits) the graph reads.
uint32_t UsedMacros(const Graph& g);
uint32_t UsedLayers(const Graph& g);

// Parses "m7" into 6 (0-based); -1 if `src` is not a numeric macro.
int MacroIndex(const std::string& src);
std::string MacroSrc(int k);

}  // namespace dxg
