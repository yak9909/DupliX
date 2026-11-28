// DupliX 2 (design v5, 4): geometry columns.
#include "dxg5/geometry.h"

#include <algorithm>

namespace dx5 {

size_t Column::size() const
{
    switch (type) {
    case Type::Float: return f.size();
    case Type::Int:
    case Type::Bool: return i.size();
    case Type::Vector: return v.size();
    case Type::Rotation: return r.size();
    default: return 0;
    }
}

Value Column::Get(size_t k) const
{
    const size_t n = size();
    if (!n) {
        Value z;
        z.type = type;
        return z;
    }
    if (k >= n) k = n - 1;
    switch (type) {
    case Type::Float: return Value::F(f[k]);
    case Type::Int: return Value::I(i[k]);
    case Type::Bool: return Value::B(i[k] != 0);
    case Type::Vector: return Value::V(v[k]);
    case Type::Rotation: return Value::R(r[k]);
    default: return Value{};
    }
}

void Column::Set(size_t k, const Value& x)
{
    Value c;
    if (!Convert(x, type, &c)) return;
    switch (type) {
    case Type::Float: f[k] = c.f; break;
    case Type::Int:
    case Type::Bool: i[k] = type == Type::Bool ? (c.i != 0) : c.i; break;
    case Type::Vector: v[k] = c.v; break;
    case Type::Rotation: r[k] = c.r; break;
    default: break;
    }
}

void Column::Resize(size_t n, const Value& fill)
{
    Value c;
    if (!Convert(fill, type, &c)) c = Value{}, c.type = type;
    switch (type) {
    case Type::Float: f.resize(n, c.f); break;
    case Type::Int:
    case Type::Bool: i.resize(n, c.i); break;
    case Type::Vector: v.resize(n, c.v); break;
    case Type::Rotation: r.resize(n, c.r); break;
    default: break;
    }
}

Column MakeColumn(Type t, size_t n, const Value& fill)
{
    Column c;
    c.type = t;
    c.Resize(n, fill);
    return c;
}

const std::vector<BuiltinAttr>& BuiltinAttrs()
{
    static const std::vector<BuiltinAttr> a = {
        {"position", Type::Vector, Value::V({0, 0, 0})},
        {"rotation", Type::Rotation, Value::R(Rot{})},
        {"scale", Type::Vector, Value::V({1, 1, 1})},
        {"opacity", Type::Float, Value::F(1)},
        {"source", Type::Int, Value::I(-1)},
        {"visible", Type::Bool, Value::B(true)},
        {"weight", Type::Float, Value::F(1)},
        {"q", Type::Float, Value::F(0)},
        {"s", Type::Int, Value::I(0)},
    };
    return a;
}

const BuiltinAttr* FindBuiltin(const std::string& name)
{
    for (const BuiltinAttr& b : BuiltinAttrs())
        if (name == b.name) return &b;
    return nullptr;
}

void Geometry::Filter(const std::vector<char>& keep)
{
    size_t n = 0;
    for (size_t k = 0; k < count; k++)
        if (keep[k]) n++;
    auto pick = [&](auto& vec) {
        size_t o = 0;
        for (size_t k = 0; k < count && k < vec.size(); k++)
            if (keep[k]) vec[o++] = vec[k];
        vec.resize(std::min(vec.size(), n));
    };
    pick(id);
    for (auto& kv : attrs) {
        Column& c = kv.second;
        if (c.size() != count) continue;  // a single value stays
        switch (c.type) {
        case Type::Float: pick(c.f); break;
        case Type::Int:
        case Type::Bool: pick(c.i); break;
        case Type::Vector: pick(c.v); break;
        case Type::Rotation: pick(c.r); break;
        default: break;
        }
    }
    count = n;
}

Geometry MakePoints(size_t n)
{
    Geometry g;
    g.count = n;
    g.id.assign(n, 0);
    for (const BuiltinAttr& b : BuiltinAttrs()) g.attrs[b.name] = MakeColumn(b.type, n, b.dflt);
    Column& q = g.attrs["q"];
    Column& s = g.attrs["s"];
    for (size_t k = 0; k < n; k++) {
        q.f[k] = n > 1 ? (double)k / (double)(n - 1) : 0;
        s.i[k] = (int64_t)k;
    }
    return g;
}

}  // namespace dx5
