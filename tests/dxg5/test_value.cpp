// Task 1: types, values, the geometry and the node table.
#include "dxg5/geometry.h"
#include "dxg5/nodes.h"
#include "dxg5/value.h"
#include "graph/eval.h"
#include "test.h"

using namespace dx5;

TEST(value_rot_matches_v1)
{
    double v1[9];
    dxg::RotationXYZ(10, 20, 30, v1);
    const Rot r = RotXYZ(10, 20, 30);
    for (int i = 0; i < 9; i++) CHECK_NEAR(r.m[i], v1[i], 1e-12);
    const Vec3 y = RotXYZ(0, 0, 90) * Vec3{1, 0, 0};
    CHECK_NEAR(y.x, 0, 1e-12);
    CHECK_NEAR(y.y, 1, 1e-12);
    CHECK_NEAR(y.z, 0, 1e-12);
    const Rot a = RotXYZ(0, 0, 30), b = RotXYZ(0, 0, 60);
    const Rot c = a * b, d = RotXYZ(0, 0, 90);
    for (int i = 0; i < 9; i++) CHECK_NEAR(c.m[i], d.m[i], 1e-12);
    const Rot e = Inverse(d) * d;
    for (int i = 0; i < 9; i++) CHECK_NEAR(e.m[i], (i % 4 == 0) ? 1 : 0, 1e-12);
}

TEST(value_convert)
{
    Value out;
    CHECK(Convert(Value::I(3), Type::Float, &out) && out.type == Type::Float && out.f == 3);
    CHECK(Convert(Value::F(2.5), Type::Int, &out) && out.i == 3);
    CHECK(Convert(Value::F(-2.5), Type::Int, &out) && out.i == -3);
    CHECK(Convert(Value::B(true), Type::Float, &out) && out.f == 1);
    CHECK(Convert(Value::F(2), Type::Vector, &out) && out.v.x == 2 && out.v.y == 2 && out.v.z == 2);
    CHECK(!Convert(Value::V({1, 2, 3}), Type::Float, &out));
    CHECK(AsInt(Value::F(1.49)) == 1);
    CHECK(AsFloat(Value::I(-7)) == -7);
}

TEST(points_defaults)
{
    const Geometry g = MakePoints(3);
    CHECK(g.count == 3);
    CHECK(g.id.size() == 3);
    const Column& vis = g.attrs.at("visible");
    CHECK(vis.type == Type::Bool && vis.i.size() == 3 && vis.i[2] == 1);
    CHECK(g.attrs.at("source").i[0] == -1);
    CHECK_NEAR(g.attrs.at("q").f[0], 0, 0);
    CHECK_NEAR(g.attrs.at("q").f[1], 0.5, 0);
    CHECK_NEAR(g.attrs.at("q").f[2], 1, 0);
    CHECK(g.attrs.at("s").i[2] == 2);
    CHECK(g.attrs.at("scale").v[1].x == 1);
    CHECK(g.attrs.at("rotation").r[0].m[4] == 1);
}

TEST(defs_have_points_and_set_position)
{
    const NodeDef* p = FindDef("points");
    CHECK(p != nullptr);
    if (p) {
        CHECK(p->inputs.size() == 1);
        CHECK(p->inputs[0].name == "count" && p->inputs[0].structural && p->inputs[0].type == Type::Int);
        CHECK(p->outputs.size() == 1 && p->outputs[0].type == Type::Geometry);
    }
    const NodeDef* s = FindDef("set_position");
    CHECK(s != nullptr);
    if (s) {
        CHECK(s->inputs.size() == 3);
        CHECK(s->inputs[0].name == "geometry" && s->inputs[0].type == Type::Geometry);
        CHECK(s->inputs[1].name == "position" && s->inputs[1].field && s->inputs[1].type == Type::Vector);
        CHECK(s->inputs[2].name == "where" && s->inputs[2].field && s->inputs[2].type == Type::Bool && s->inputs[2].dflt.b());
    }
    CHECK(FindDef("nope") == nullptr);
    CHECK(FindDef("math_add") != nullptr);
    CHECK(FindDef("bounds") && FindDef("bounds")->outputs.size() == 3);
}
