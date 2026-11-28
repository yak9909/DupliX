// DupliX 2 (design v5, 6): the table of node types. A node is a function (design v5 0.4): its inputs are the
// arguments, its outputs what it returns. The code's names are these names.
#pragma once

#include <string>
#include <vector>

#include "dxg5/value.h"

namespace dx5 {

struct InputDef {
    std::string name;
    Type type = Type::Float;
    Value dflt;
    bool structural = false;  // decides the number or order of elements: single values only (design v5 3)
    bool field = false;       // takes a field (a value per element)
    double softMin = 0, softMax = 100;  // the editor's slider
    std::string choices;      // "A|B|C": an Int choice (structural)
    // The geometry a field on this input is evaluated over (design v5 5): the name of one of this node's geometry
    // inputs, or "make" (the elements a generator makes), or "" (a value node: where its output is used).
    std::string context;
};

struct OutputDef {
    std::string name;
    Type type = Type::Float;
    bool field = false;  // a value per element (used where it is plugged in)
};

struct NodeDef {
    std::string name;      // the code's function name and the stored type (never change it)
    std::string label;     // the editor's title
    std::string category;  // Generate, Set, Geometry, Input, Math, Utility, Group, Zone, Output
    std::vector<InputDef> inputs;
    std::vector<OutputDef> outputs;
    bool group = false;    // a built-in group (design v5 6.1): defined in code, evaluated natively when it can
    // what the node's text holds ("" none): "expr" (an Expression's formula), "name" (Store / Attr's attribute)
    std::string text;
    // what the node's setting holds ("" none): "macro" / "int_macro" (1..32), "op" (reduce: 0 sum 1 min 2 max 3 mean)
    std::string setting;
    int Input(const std::string& n) const;   // index or -1
    int Output(const std::string& n) const;  // index or -1
};

// The table holds the five math families (math, vmath, compare, boolmath, rotate: design v5 6.2); FindDef also
// answers a math operation's code name ("math_add", "vmath_dot", and the older "math_vec") with a def of that
// operation's inputs, so the reader can call it like any node.
const NodeDef* FindDef(const std::string& name);
const std::vector<NodeDef>& AllDefs();

// ---- the math nodes: a family and an operator (the node's text) ----
struct MathOpDef {
    std::string family;          // math, vmath, compare, boolmath, rotate
    std::string op;              // add, sin, dot, lt, and, rot_mul, ...
    int arity = 2;               // inputs a, b, c used (the rest are hidden)
    std::vector<Type> inTypes;   // per used input (Any: whatever comes)
    Type out = Type::Any;
    std::string impl;            // the MathOp name that computes it
    std::string CodeName() const { return family + "_" + op; }
};
bool IsMathFamily(const std::string& type);
const MathOpDef* FindMathOp(const std::string& family, const std::string& op);
// "math_add" -> math/add, "vmath_dot" -> vmath/dot; the older "math_<op>" of any family ("math_vec", "math_eq")
const MathOpDef* FindMathOpByCodeName(const std::string& name);
std::vector<const MathOpDef*> MathOps(const std::string& family);
// the operator a new node of the family starts with
std::string DefaultMathOp(const std::string& family);
// The built-in groups' definitions (dx5_groups.cpp), part of AllDefs.
std::vector<NodeDef> GroupDefs();

}  // namespace dx5
