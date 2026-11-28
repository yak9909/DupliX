// DupliX 2 (design v5, 3 and 6.2): the element-wise operations of the math nodes and Expression formulas.
// Int op Int stays Int (but `/` is always Float, design v5 3); a Float makes it Float; a Vector applies the
// operation to each component (a scalar is spread to all three). Trigonometry is in radians; rot() takes degrees
// (After Effects' rotations).
#include <cmath>

#include "dxg5/eval_internal.h"

namespace dx5 {

namespace {

bool Scalar(const Value& v) { return v.type == Type::Float || v.type == Type::Int || v.type == Type::Bool; }
bool IntLike(const Value& v) { return v.type == Type::Int || v.type == Type::Bool; }
Vec3 AsVec(const Value& v)
{
    if (v.type == Type::Vector) return v.v;
    const double x = AsFloat(v);
    return {x, x, x};
}

double FloorMod(double a, double b) { return b == 0 ? 0 : a - std::floor(a / b) * b; }
int64_t IntFloorDiv(int64_t a, int64_t b)
{
    if (b == 0) return 0;
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}
int64_t IntFloorMod(int64_t a, int64_t b) { return b == 0 ? 0 : a - IntFloorDiv(a, b) * b; }

template <class F>
bool Arith(const Value& a, const Value& b, Value* out, bool intKeeps, F f)
{
    if (a.type == Type::Vector || b.type == Type::Vector) {
        if (!(Scalar(a) || a.type == Type::Vector) || !(Scalar(b) || b.type == Type::Vector)) return false;
        const Vec3 x = AsVec(a), y = AsVec(b);
        *out = Value::V({f(x.x, y.x), f(x.y, y.y), f(x.z, y.z)});
        return true;
    }
    if (!Scalar(a) || !Scalar(b)) return false;
    const double r = f(AsFloat(a), AsFloat(b));
    if (intKeeps && IntLike(a) && IntLike(b) && std::isfinite(r)) *out = Value::I((int64_t)std::llround(r));
    else *out = Value::F(r);
    return true;
}

template <class F>
bool Unary(const Value& a, Value* out, bool intKeeps, F f)
{
    if (a.type == Type::Vector) {
        *out = Value::V({f(a.v.x), f(a.v.y), f(a.v.z)});
        return true;
    }
    if (!Scalar(a)) return false;
    const double r = f(AsFloat(a));
    if (intKeeps && std::isfinite(r)) *out = Value::I((int64_t)std::llround(r));
    else *out = Value::F(r);
    return true;
}

bool Fail(std::string* err, const std::string& op, const Value* a, int n)
{
    if (err) {
        *err = "cannot " + op + " ";
        for (int k = 0; k < n; k++) *err += std::string(k ? ", " : "") + TypeName(a[k].type);
    }
    return false;
}

}  // namespace

bool MathOp(const std::string& op, const Value* a, int n, Value* out, std::string* err)
{
    auto need = [&](int k) { return n >= k; };
    if (!need(1)) return Fail(err, op, a, n);
    const Value& x = a[0];
    const Value y = n > 1 ? a[1] : Value::F(0);
    bool ok = false;
    if (op == "add") ok = Arith(x, y, out, true, [](double p, double q) { return p + q; });
    else if (op == "sub") ok = Arith(x, y, out, true, [](double p, double q) { return p - q; });
    else if (op == "mul") {
        if (x.type == Type::Rotation && y.type == Type::Rotation) *out = Value::R(x.r * y.r), ok = true;
        else if (x.type == Type::Rotation && y.type == Type::Vector) *out = Value::V(x.r * y.v), ok = true;
        else ok = Arith(x, y, out, true, [](double p, double q) { return p * q; });
    } else if (op == "div") ok = Arith(x, y, out, false, [](double p, double q) { return q == 0 ? 0.0 : p / q; });
    else if (op == "floordiv") {
        if (IntLike(x) && IntLike(y)) *out = Value::I(IntFloorDiv(x.i, y.i)), ok = true;
        else ok = Arith(x, y, out, false, [](double p, double q) { return q == 0 ? 0.0 : std::floor(p / q); });
    } else if (op == "mod") {
        if (IntLike(x) && IntLike(y)) *out = Value::I(IntFloorMod(x.i, y.i)), ok = true;
        else ok = Arith(x, y, out, false, FloorMod);
    } else if (op == "pow") {
        const bool keep = IntLike(x) && IntLike(y) && y.i >= 0;
        ok = Arith(x, y, out, keep, [](double p, double q) { return std::pow(p, q); });
    } else if (op == "min") ok = Arith(x, y, out, true, [](double p, double q) { return std::min(p, q); });
    else if (op == "max") ok = Arith(x, y, out, true, [](double p, double q) { return std::max(p, q); });
    else if (op == "atan2") ok = Arith(x, y, out, false, [](double p, double q) { return std::atan2(p, q); });
    else if (op == "eq" || op == "ne") {
        bool same;
        if (x.type == Type::Vector || y.type == Type::Vector) {
            const Vec3 p = AsVec(x), q = AsVec(y);
            same = p.x == q.x && p.y == q.y && p.z == q.z;
        } else if (Scalar(x) && Scalar(y)) {
            same = AsFloat(x) == AsFloat(y);
        } else {
            return Fail(err, op, a, n);
        }
        *out = Value::B(op == "eq" ? same : !same), ok = true;
    } else if (op == "lt" || op == "le" || op == "gt" || op == "ge") {
        if (!Scalar(x) || !Scalar(y)) return Fail(err, op, a, n);
        const double p = AsFloat(x), q = AsFloat(y);
        const bool r = op == "lt" ? p < q : op == "le" ? p <= q : op == "gt" ? p > q : p >= q;
        *out = Value::B(r), ok = true;
    } else if (op == "and" || op == "or") {
        if (!Scalar(x) || !Scalar(y)) return Fail(err, op, a, n);
        const bool p = AsFloat(x) != 0, q = AsFloat(y) != 0;
        *out = Value::B(op == "and" ? (p && q) : (p || q)), ok = true;
    } else if (op == "not") {
        if (!Scalar(x)) return Fail(err, op, a, n);
        *out = Value::B(AsFloat(x) == 0), ok = true;
    } else if (op == "neg") ok = Unary(x, out, IntLike(x), [](double p) { return -p; });
    else if (op == "abs") ok = Unary(x, out, IntLike(x), [](double p) { return std::fabs(p); });
    else if (op == "floor") ok = Unary(x, out, x.type != Type::Vector, [](double p) { return std::floor(p); });
    else if (op == "ceil") ok = Unary(x, out, x.type != Type::Vector, [](double p) { return std::ceil(p); });
    else if (op == "round") ok = Unary(x, out, x.type != Type::Vector, [](double p) { return (double)std::llround(p); });
    else if (op == "sin") ok = Unary(x, out, false, [](double p) { return std::sin(p); });
    else if (op == "cos") ok = Unary(x, out, false, [](double p) { return std::cos(p); });
    else if (op == "tan") ok = Unary(x, out, false, [](double p) { return std::tan(p); });
    else if (op == "asin") ok = Unary(x, out, false, [](double p) { return std::asin(std::max(-1.0, std::min(1.0, p))); });
    else if (op == "acos") ok = Unary(x, out, false, [](double p) { return std::acos(std::max(-1.0, std::min(1.0, p))); });
    else if (op == "sqrt") ok = Unary(x, out, false, [](double p) { return p > 0 ? std::sqrt(p) : 0.0; });
    else if (op == "len") {
        if (x.type == Type::Vector) *out = Value::F(std::sqrt(x.v.x * x.v.x + x.v.y * x.v.y + x.v.z * x.v.z)), ok = true;
        else if (Scalar(x)) *out = Value::F(std::fabs(AsFloat(x))), ok = true;
    } else if (op == "normalize") {
        if (x.type != Type::Vector) return Fail(err, op, a, n);
        const double l = std::sqrt(x.v.x * x.v.x + x.v.y * x.v.y + x.v.z * x.v.z);
        *out = Value::V(l > 0 ? x.v * (1 / l) : Vec3{}), ok = true;
    } else if (op == "dot") {
        if (x.type != Type::Vector || y.type != Type::Vector) return Fail(err, op, a, n);
        *out = Value::F(x.v.x * y.v.x + x.v.y * y.v.y + x.v.z * y.v.z), ok = true;
    } else if (op == "cross") {
        if (x.type != Type::Vector || y.type != Type::Vector) return Fail(err, op, a, n);
        const Vec3 p = x.v, q = y.v;
        *out = Value::V({p.y * q.z - p.z * q.y, p.z * q.x - p.x * q.z, p.x * q.y - p.y * q.x}), ok = true;
    } else if (op == "clamp" && n >= 3) {
        Value lo;
        if (!Arith(x, a[1], &lo, true, [](double p, double q) { return std::max(p, q); })) return Fail(err, op, a, n);
        ok = Arith(lo, a[2], out, true, [](double p, double q) { return std::min(p, q); });
    } else if (op == "mix" && n >= 3) {
        if (!Scalar(a[2])) return Fail(err, op, a, n);
        const double t = AsFloat(a[2]);
        ok = Arith(x, y, out, false, [t](double p, double q) { return p + (q - p) * t; });
    } else if (op == "smoothstep" && n >= 3) {
        if (!Scalar(x) || !Scalar(y) || !Scalar(a[2])) return Fail(err, op, a, n);
        const double e0 = AsFloat(x), e1 = AsFloat(y);
        double t = e1 == e0 ? (AsFloat(a[2]) >= e1 ? 1 : 0) : (AsFloat(a[2]) - e0) / (e1 - e0);
        t = std::max(0.0, std::min(1.0, t));
        *out = Value::F(t * t * (3 - 2 * t)), ok = true;
    } else if (op == "switch" && n >= 3) {
        if (!Scalar(x)) return Fail(err, op, a, n);
        const Value& p = AsFloat(x) != 0 ? a[1] : a[2];
        const Value& q = AsFloat(x) != 0 ? a[2] : a[1];
        // the type both can take (Int and Float: Float; a Vector: Vector)
        Value c;
        if (p.type == q.type || !Convert(p, (q.type == Type::Float || q.type == Type::Vector) ? q.type : p.type, &c)) c = p;
        *out = c, ok = true;
    } else if (op == "vec" && n >= 3) {
        if (!Scalar(x) || !Scalar(y) || !Scalar(a[2])) return Fail(err, op, a, n);
        *out = Value::V({AsFloat(x), AsFloat(y), AsFloat(a[2])}), ok = true;
    } else if (op == "sep_x" || op == "sep_y" || op == "sep_z") {
        if (x.type != Type::Vector) return Fail(err, op, a, n);
        *out = Value::F(op == "sep_x" ? x.v.x : op == "sep_y" ? x.v.y : x.v.z), ok = true;
    } else if (op == "rot" && n >= 3) {
        if (!Scalar(x) || !Scalar(y) || !Scalar(a[2])) return Fail(err, op, a, n);
        *out = Value::R(RotXYZ(AsFloat(x), AsFloat(y), AsFloat(a[2]))), ok = true;
    } else if (op == "rot_mul") {
        if (x.type != Type::Rotation || y.type != Type::Rotation) return Fail(err, op, a, n);
        *out = Value::R(x.r * y.r), ok = true;
    } else if (op == "rot_apply") {
        if (x.type != Type::Rotation || y.type != Type::Vector) return Fail(err, op, a, n);
        *out = Value::V(x.r * y.v), ok = true;
    } else if (op == "inverse") {
        if (x.type != Type::Rotation) return Fail(err, op, a, n);
        *out = Value::R(Inverse(x.r)), ok = true;
    } else {
        if (err) *err = "unknown operation " + op;
        return false;
    }
    return ok ? true : Fail(err, op, a, n);
}

}  // namespace dx5
