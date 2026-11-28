// DupliX 2 (design v5, 3): values and rotations.
#include "dxg5/value.h"

#include <charconv>
#include <cmath>

namespace dx5 {

namespace {
constexpr double kDeg = 3.14159265358979323846 / 180.0;
}

const char* TypeName(Type t)
{
    switch (t) {
    case Type::Geometry: return "Geometry";
    case Type::Float: return "Float";
    case Type::Int: return "Int";
    case Type::Bool: return "Bool";
    case Type::Vector: return "Vector";
    case Type::Rotation: return "Rotation";
    case Type::Color: return "Color";
    case Type::Any: return "Any";
    }
    return "?";
}

Rot operator*(const Rot& a, const Rot& b)
{
    Rot o;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            o.m[i * 3 + j] = a.m[i * 3] * b.m[j] + a.m[i * 3 + 1] * b.m[3 + j] + a.m[i * 3 + 2] * b.m[6 + j];
    return o;
}

Vec3 operator*(const Rot& r, Vec3 v)
{
    return {r.m[0] * v.x + r.m[1] * v.y + r.m[2] * v.z, r.m[3] * v.x + r.m[4] * v.y + r.m[5] * v.z,
            r.m[6] * v.x + r.m[7] * v.y + r.m[8] * v.z};
}

Rot Inverse(const Rot& r)
{
    Rot o;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) o.m[i * 3 + j] = r.m[j * 3 + i];
    return o;
}

Rot RotXYZ(double rx, double ry, double rz)
{
    const double cx = std::cos(rx * kDeg), sx = std::sin(rx * kDeg);
    const double cy = std::cos(ry * kDeg), sy = std::sin(ry * kDeg);
    const double cz = std::cos(rz * kDeg), sz = std::sin(rz * kDeg);
    const Rot X{{1, 0, 0, 0, cx, -sx, 0, sx, cx}};
    const Rot Y{{cy, 0, sy, 0, 1, 0, -sy, 0, cy}};
    const Rot Z{{cz, -sz, 0, sz, cz, 0, 0, 0, 1}};
    return (Z * Y) * X;
}

int64_t RoundHalfAway(double v)
{
    if (!(v == v)) return 0;
    if (v > 9.2e18) return INT64_MAX;
    if (v < -9.2e18) return INT64_MIN;
    return (int64_t)std::llround(v);
}

double AsFloat(const Value& v)
{
    switch (v.type) {
    case Type::Float: return v.f;
    case Type::Int:
    case Type::Bool: return (double)v.i;
    default: return 0;
    }
}

int64_t AsInt(const Value& v)
{
    switch (v.type) {
    case Type::Float: return RoundHalfAway(v.f);
    case Type::Int:
    case Type::Bool: return v.i;
    default: return 0;
    }
}

bool Convert(const Value& in, Type to, Value* out)
{
    if (in.type == to || to == Type::Any) {
        *out = in;
        return true;
    }
    const bool scalar = in.type == Type::Float || in.type == Type::Int || in.type == Type::Bool;
    switch (to) {
    case Type::Float:
        if (in.type != Type::Int && in.type != Type::Bool) return false;
        *out = Value::F(AsFloat(in));
        return true;
    case Type::Int:
        if (in.type != Type::Float && in.type != Type::Bool) return false;
        *out = Value::I(AsInt(in));
        return true;
    case Type::Vector:
        if (!scalar) return false;
        {
            const double x = AsFloat(in);
            *out = Value::V({x, x, x});
        }
        return true;
    default: return false;
    }
}

std::string Num(double v)
{
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, r.ptr);
}

uint64_t HashStr(const std::string& s)
{
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

}  // namespace dx5
