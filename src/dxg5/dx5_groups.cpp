// DupliX 2 (design v5, 6.1 and 12): the built-in groups. Their inputs (names, types, defaults: P1's values, design
// v4 8.1) are declared here; their definitions in code and the C++ that stands in for them come with Task 7.
#include "dxg5/eval_internal.h"
#include "dxg5/nodes.h"

namespace dx5 {

bool NativeGroup(Evaluator&, const Node&, Geometry*) { return false; }

namespace {

InputDef F(const char* n, double d, double lo, double hi, const char* ctx)
{
    InputDef i;
    i.name = n, i.type = Type::Float, i.dflt = Value::F(d), i.softMin = lo, i.softMax = hi, i.field = true, i.context = ctx;
    return i;
}
InputDef I(const char* n, int64_t d, double lo, double hi, const char* choices = "")
{
    InputDef i;
    i.name = n, i.type = Type::Int, i.dflt = Value::I(d), i.softMin = lo, i.softMax = hi, i.structural = true;
    i.choices = choices;
    return i;
}
InputDef V(const char* n, const char* ctx)
{
    InputDef i;
    i.name = n, i.type = Type::Vector, i.dflt = Value::V({0, 0, 0}), i.softMin = -1000, i.softMax = 1000, i.field = true;
    i.context = ctx;
    return i;
}
InputDef G()
{
    InputDef i;
    i.name = "geometry", i.type = Type::Geometry;
    return i;
}
NodeDef Group(const char* name, const char* label, const char* cat, std::vector<InputDef> in)
{
    NodeDef d;
    d.name = name, d.label = label, d.category = cat, d.inputs = std::move(in), d.group = true;
    d.outputs = {{"geometry", Type::Geometry, false}};
    return d;
}

}  // namespace

std::vector<NodeDef> GroupDefs()
{
    std::vector<NodeDef> d;
    d.push_back(Group("grid", "Grid", "Generate",
                      {I("cx", 5, 1, 50), I("cy", 5, 1, 50), I("cz", 1, 1, 20), F("sx", 100, 0, 400, "make"),
                       F("sy", 100, 0, 400, "make"), F("sz", 100, 0, 400, "make")}));
    d.push_back(Group("circle", "Circle", "Generate",
                      {I("count", 12, 1, 100), F("radius", 300, 0, 1000, "make"), F("start_angle", 0, -180, 180, "make"),
                       F("sweep", 360, 0, 360, "make"), I("align", 0, 0, 1, "Off|Outward")}));
    d.push_back(Group("line", "Line", "Generate",
                      {I("count", 10, 1, 100), F("length", 800, 0, 2000, "make"), F("angle", 0, -180, 180, "make")}));
    {
        // Transform moves the layout as a whole: single values (design v3 2.3)
        InputDef p = V("position", "");
        p.field = false;
        InputDef rx = F("rotation_x", 0, -180, 180, ""), ry = F("rotation_y", 0, -180, 180, ""), rz = F("rotation_z", 0, -180, 180, "");
        InputDef sc = F("scale", 100, 0, 200, "");
        for (InputDef* x : {&rx, &ry, &rz, &sc}) x->field = false;
        d.push_back(Group("transform", "Transform", "Set", {G(), p, rx, ry, rz, sc, I("pivot", 0, 0, 1, "Center|Origin")}));
    }
    d.push_back(Group("modify", "Modify", "Set",
                      {G(), V("position", "geometry"), F("rotation_x", 0, -180, 180, "geometry"),
                       F("rotation_y", 0, -180, 180, "geometry"), F("rotation_z", 0, -180, 180, "geometry"),
                       F("scale", 0, -100, 200, "geometry"), F("opacity", 0, -100, 100, "geometry")}));
    d.push_back(Group("source", "Source", "Set",
                      {G(), I("first", 1, 1, 8), I("layers", 1, 1, 8), I("assign", 0, 0, 1, "Sequential|Random")}));
    return d;
}

}  // namespace dx5
