// DupliX 2 (design v5, 6.2): the basic nodes. Changing a name breaks saved graphs and code: add new ones instead.
#include "dxg5/nodes.h"

namespace dx5 {

namespace {

InputDef In(const char* n, Type t, Value d, const char* ctx = "")
{
    InputDef i;
    i.name = n, i.type = t, i.dflt = d, i.context = ctx;
    return i;
}
InputDef Field(const char* n, Type t, Value d, const char* ctx = "geometry")
{
    InputDef i = In(n, t, d, ctx);
    i.field = true;
    return i;
}
InputDef Struct(const char* n, int64_t d, double lo, double hi)
{
    InputDef i = In(n, Type::Int, Value::I(d));
    i.structural = true, i.softMin = lo, i.softMax = hi;
    return i;
}
InputDef Geo(const char* n = "geometry") { return In(n, Type::Geometry, Value{}); }
OutputDef Out(const char* n, Type t, bool field = false) { return {n, t, field}; }

NodeDef Def(const char* name, const char* label, const char* cat, std::vector<InputDef> in, std::vector<OutputDef> out)
{
    NodeDef d;
    d.name = name, d.label = label, d.category = cat, d.inputs = std::move(in), d.outputs = std::move(out);
    return d;
}

NodeDef Set(const char* name, const char* label, const char* attr, Type t, Value dflt)
{
    return Def(name, label, "Set", {Geo(), Field(attr, t, dflt), Field("where", Type::Bool, Value::B(true))},
               {Out("geometry", Type::Geometry)});
}

NodeDef Read(const char* name, const char* label, Type t, bool field = true)
{
    return Def(name, label, "Input", {}, {Out("value", t, field)});
}

NodeDef Math(const char* name, const char* label, int args, Type out = Type::Any)
{
    std::vector<InputDef> in;
    const char* names[3] = {"a", "b", "c"};
    for (int k = 0; k < args; k++) in.push_back(Field(names[k], Type::Any, Value::F(0), ""));
    return Def(name, label, "Math", in, {Out("value", out, true)});
}

std::vector<NodeDef> Base()
{
    std::vector<NodeDef> d;
    {
        NodeDef p = Def("points", "Points", "Generate", {Struct("count", 1, 0, 100)}, {Out("geometry", Type::Geometry)});
        d.push_back(p);
    }
    d.push_back(Set("set_position", "Set Position", "position", Type::Vector, Value::V({0, 0, 0})));
    d.push_back(Set("set_rotation", "Set Rotation", "rotation", Type::Rotation, Value::R(Rot{})));
    d.push_back(Set("set_scale", "Set Scale", "scale", Type::Vector, Value::V({1, 1, 1})));
    d.push_back(Set("set_opacity", "Set Opacity", "opacity", Type::Float, Value::F(1)));
    d.push_back(Set("set_source", "Set Source", "source", Type::Int, Value::I(-1)));
    d.push_back(Set("set_visible", "Set Visible", "visible", Type::Bool, Value::B(true)));
    d.push_back(Set("set_weight", "Set Weight", "weight", Type::Float, Value::F(1)));
    {
        NodeDef s = Def("store", "Store", "Set",
                        {Geo(), Field("value", Type::Any, Value::F(0)), Field("where", Type::Bool, Value::B(true))},
                        {Out("geometry", Type::Geometry)});
        s.text = "name";
        d.push_back(s);
    }
    d.push_back(Def("capture", "Capture", "Set", {Geo(), Field("value", Type::Any, Value::F(0))},
                    {Out("geometry", Type::Geometry), Out("value", Type::Any, true)}));
    d.push_back(Def("delete", "Delete", "Geometry", {Geo(), Field("where", Type::Bool, Value::B(false))},
                    {Out("geometry", Type::Geometry)}));
    {
        InputDef cs = In("copy_scale", Type::Float, Value::F(100));
        cs.softMin = 0, cs.softMax = 200;
        d.push_back(Def("output", "Output", "Output", {Geo(), cs}, {}));
    }
    // a zone: `inner` starts the chain inside it, `body` is where that chain ends (design v5 9)
    d.push_back(Def("each", "Each", "Zone", {Geo(), Geo("body")}, {Out("geometry", Type::Geometry), Out("inner", Type::Geometry)}));

    // values read from the element (design v5 4.4)
    d.push_back(Read("position", "Position", Type::Vector));
    d.push_back(Read("index", "Index", Type::Int));
    d.push_back(Read("id", "ID", Type::Int));
    d.push_back(Read("count", "Count", Type::Int, false));
    d.push_back(Read("rotation", "Rotation", Type::Rotation));
    d.push_back(Read("scale", "Scale", Type::Vector));
    d.push_back(Read("opacity", "Opacity", Type::Float));
    d.push_back(Read("source", "Source", Type::Int));
    d.push_back(Read("visible", "Visible", Type::Bool));
    d.push_back(Read("weight", "Weight", Type::Float));
    d.push_back(Read("q", "Order Q", Type::Float));
    d.push_back(Read("s", "Order Step", Type::Int));
    d.push_back(Read("time", "Time", Type::Float, false));
    d.push_back(Read("seed", "Seed", Type::Int, false));
    {
        NodeDef a = Read("attr", "Named Attribute", Type::Any);
        a.text = "name";
        d.push_back(a);
    }
    {
        NodeDef m = Read("macro", "Macro", Type::Float, false);
        m.setting = "macro";
        d.push_back(m);
        NodeDef i = Read("int_macro", "Int Macro", Type::Int, false);
        i.setting = "int_macro";
        d.push_back(i);
    }
    d.push_back(Read("random", "Random", Type::Float));  // salt: Node::salt
    {
        NodeDef e = Def("expr", "Expression", "Math", {}, {Out("value", Type::Any, true)});
        e.text = "expr";
        d.push_back(e);
    }

    // math: five families, the operator chosen on the node (design v5 6.2; the Expression node holds a formula)
    for (const auto& f : std::vector<std::pair<const char*, const char*>>{
             {"math", "Math"}, {"vmath", "Vector Math"}, {"compare", "Compare"}, {"boolmath", "Boolean Math"}, {"rotate", "Rotate"}}) {
        NodeDef m = Math(f.first, f.second, 3);
        m.text = "op";
        d.push_back(m);
    }

    // reading other elements and the whole list (design v5 5, 6.2)
    d.push_back(Def("at", "Sample Index", "Utility",
                    {Geo(), Field("index", Type::Int, Value::I(0), ""), Field("value", Type::Any, Value::F(0))},
                    {Out("value", Type::Any, true)}));
    d.push_back(Def("accumulate", "Accumulate", "Utility", {Field("value", Type::Any, Value::F(0), "")},
                    {Out("value", Type::Any, true)}));
    d.push_back(Def("bounds", "Bounds", "Utility", {Geo(), Field("value", Type::Vector, Value::V({0, 0, 0}))},
                    {Out("min", Type::Vector), Out("max", Type::Vector), Out("center", Type::Vector)}));
    {
        NodeDef r = Def("reduce", "Reduce", "Utility", {Geo(), Field("value", Type::Any, Value::F(0))}, {Out("value", Type::Any)});
        r.setting = "op";
        d.push_back(r);
    }
    {
        InputDef k = Struct("k", 1, 1, 8);
        d.push_back(Def("layer_set", "Layer Set", "Input", {k}, {Out("value", Type::Bool)}));
    }
    // inside a group's definition: its inputs (text: the input's name)
    {
        NodeDef p = Def("param", "Group Input", "Hidden", {}, {Out("value", Type::Any, true)});
        p.text = "name";
        d.push_back(p);
        NodeDef q = Def("param_geo", "Group Geometry", "Hidden", {}, {Out("geometry", Type::Geometry)});
        q.text = "name";
        d.push_back(q);
    }
    return d;
}

const std::vector<MathOpDef>& Ops()
{
    static const std::vector<MathOpDef> ops = [] {
        std::vector<MathOpDef> o;
        const Type A = Type::Any, F = Type::Float, V = Type::Vector, R = Type::Rotation, B = Type::Bool;
        auto add = [&](const char* fam, const char* op, std::vector<Type> in, Type out, const char* impl = nullptr) {
            MathOpDef m;
            m.family = fam, m.op = op, m.arity = (int)in.size(), m.inTypes = in, m.out = out, m.impl = impl ? impl : op;
            o.push_back(m);
        };
        for (const char* n : {"add", "sub", "mul", "div", "floordiv", "mod", "pow", "min", "max", "atan2"}) add("math", n, {A, A}, A);
        for (const char* n : {"neg", "sin", "cos", "tan", "asin", "acos", "sqrt", "abs", "floor", "ceil", "round"})
            add("math", n, {A}, A);
        for (const char* n : {"clamp", "mix", "smoothstep", "switch"}) add("math", n, {A, A, A}, A);
        add("vmath", "add", {V, V}, V);
        add("vmath", "sub", {V, V}, V);
        add("vmath", "mul", {V, V}, V);
        add("vmath", "scale", {V, F}, V, "mul");
        add("vmath", "len", {V}, F);
        add("vmath", "normalize", {V}, V);
        add("vmath", "dot", {V, V}, F);
        add("vmath", "cross", {V, V}, V);
        add("vmath", "vec", {F, F, F}, V);
        add("vmath", "sep_x", {V}, F);
        add("vmath", "sep_y", {V}, F);
        add("vmath", "sep_z", {V}, F);
        for (const char* n : {"eq", "ne", "lt", "le", "gt", "ge"}) add("compare", n, {A, A}, B);
        add("boolmath", "and", {B, B}, B);
        add("boolmath", "or", {B, B}, B);
        add("boolmath", "not", {B}, B);
        add("rotate", "rot", {F, F, F}, R);
        add("rotate", "rot_mul", {R, R}, R);
        add("rotate", "rot_apply", {R, V}, V);
        add("rotate", "inverse", {R}, R);
        return o;
    }();
    return ops;
}

// one def per operation, under its code name and (for the older code) math_<op>
const std::vector<NodeDef>& OpDefs()
{
    static const std::vector<NodeDef> defs = [] {
        std::vector<NodeDef> d;
        for (const MathOpDef& m : Ops()) {
            NodeDef n = Math(m.CodeName().c_str(), m.op.c_str(), m.arity, m.out);
            n.category = "Hidden";
            d.push_back(n);
        }
        return d;
    }();
    return defs;
}

}  // namespace

bool IsMathFamily(const std::string& t)
{
    return t == "math" || t == "vmath" || t == "compare" || t == "boolmath" || t == "rotate";
}

const MathOpDef* FindMathOp(const std::string& family, const std::string& op)
{
    for (const MathOpDef& m : Ops())
        if (m.family == family && m.op == op) return &m;
    return nullptr;
}

const MathOpDef* FindMathOpByCodeName(const std::string& name)
{
    const size_t u = name.find('_');
    if (u == std::string::npos) return nullptr;
    const std::string fam = name.substr(0, u), op = name.substr(u + 1);
    if (!IsMathFamily(fam)) return nullptr;
    if (const MathOpDef* m = FindMathOp(fam, op)) return m;
    if (fam != "math") return nullptr;
    for (const MathOpDef& m : Ops())  // the older math_<op> of the other families
        if (m.op == op) return &m;
    return nullptr;
}

std::vector<const MathOpDef*> MathOps(const std::string& family)
{
    std::vector<const MathOpDef*> r;
    for (const MathOpDef& m : Ops())
        if (m.family == family) r.push_back(&m);
    return r;
}

std::string DefaultMathOp(const std::string& family)
{
    if (family == "compare") return "lt";
    if (family == "boolmath") return "and";
    if (family == "rotate") return "rot";
    return "add";
}

int NodeDef::Input(const std::string& n) const
{
    for (size_t k = 0; k < inputs.size(); k++)
        if (inputs[k].name == n) return (int)k;
    return -1;
}
int NodeDef::Output(const std::string& n) const
{
    for (size_t k = 0; k < outputs.size(); k++)
        if (outputs[k].name == n) return (int)k;
    return -1;
}

const std::vector<NodeDef>& AllDefs()
{
    static const std::vector<NodeDef> all = [] {
        std::vector<NodeDef> d = Base();
        for (NodeDef& g : GroupDefs()) d.push_back(g);
        return d;
    }();
    return all;
}

const NodeDef* FindDef(const std::string& name)
{
    for (const NodeDef& d : AllDefs())
        if (d.name == name) return &d;
    if (const MathOpDef* m = FindMathOpByCodeName(name)) {
        const std::string code = m->CodeName();
        for (const NodeDef& d : OpDefs())
            if (d.name == code) return &d;
    }
    return nullptr;
}

}  // namespace dx5
