// DupliX 2 (design v5, 3): the value types. Portable C++17, no After Effects or Win32 includes.
#pragma once

#include <cstdint>
#include <string>

namespace dx5 {

// Any: a math node's input or output, whose type follows its inputs (Int + Int = Int, a Float makes it Float,
// a Vector makes it Vector, a comparison is Bool).
enum class Type { Geometry, Float, Int, Bool, Vector, Rotation, Color, Any };
const char* TypeName(Type t);

struct Vec3 {
    double x = 0, y = 0, z = 0;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline Vec3 operator/(Vec3 a, Vec3 b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }

// A rotation as a 3x3 matrix, row major, its columns the copy's x, y, z axes (the layout of dxg::Element::basis).
struct Rot {
    double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};
// R = Rz * Ry * Rx in degrees (After Effects' X / Y / Z Rotation; the same as dxg::RotationXYZ).
Rot RotXYZ(double rx, double ry, double rz);
Rot operator*(const Rot& a, const Rot& b);
Vec3 operator*(const Rot& r, Vec3 v);
Rot Inverse(const Rot& r);  // the transpose (a rotation)

struct Value {
    Type type = Type::Float;
    double f = 0;
    int64_t i = 0;  // Int, and Bool as 0 / 1
    Vec3 v;
    Rot r;
    static Value F(double x) { Value a; a.type = Type::Float, a.f = x; return a; }
    static Value I(int64_t x) { Value a; a.type = Type::Int, a.i = x; return a; }
    static Value B(bool x) { Value a; a.type = Type::Bool, a.i = x ? 1 : 0; return a; }
    static Value V(Vec3 x) { Value a; a.type = Type::Vector, a.v = x; return a; }
    static Value R(const Rot& x) { Value a; a.type = Type::Rotation, a.r = x; return a; }
    bool b() const { return i != 0; }
};

// Structural inputs round halves away from zero (design v4 8.1).
int64_t RoundHalfAway(double v);
double AsFloat(const Value& v);   // Float, Int, Bool (a Vector or a Rotation: 0)
int64_t AsInt(const Value& v);    // rounded (half away from zero)
// The implicit conversions of design v5 3 only: Int -> Float, Float -> Int (rounded), Bool -> Int / Float,
// Int / Float / Bool -> Vector (all three the same). Same type: copied. Anything else: false.
bool Convert(const Value& in, Type to, Value* out);

// The shortest text that reads back to the same double, never locale dependent.
std::string Num(double v);
// Hash of a string (FNV-1a, 64 bit): element ids from node keys (design v5 11).
uint64_t HashStr(const std::string& s);

}  // namespace dx5
