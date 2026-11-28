// DupliX 2 (design v5, 4): a geometry is a list of elements kept as columns of attributes.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "dxg5/value.h"

namespace dx5 {

// One attribute for every element. Only the vector of the column's type is used (Bool in `i` as 0 / 1).
struct Column {
    Type type = Type::Float;
    std::vector<double> f;
    std::vector<int64_t> i;
    std::vector<Vec3> v;
    std::vector<Rot> r;
    size_t size() const;
    Value Get(size_t k) const;          // k is clamped to the last element (a column of 1 is a single value)
    void Set(size_t k, const Value& x); // converted to the column's type (design v5 3)
    void Resize(size_t n, const Value& fill);
};
Column MakeColumn(Type t, size_t n, const Value& fill);

// The built-in attributes (design v5 4.1). `index`, `count` and `id` are not attributes: the evaluation makes them.
struct BuiltinAttr {
    const char* name;
    Type type;
    Value dflt;
};
const std::vector<BuiltinAttr>& BuiltinAttrs();
const BuiltinAttr* FindBuiltin(const std::string& name);

struct Geometry {
    size_t count = 0;
    std::vector<uint64_t> id;
    std::map<std::string, Column> attrs;  // the built-in ones always; named ones ("ix"), captures ("#capture:<key>")
    // Keeps the elements whose `keep` is true, in order (Delete).
    void Filter(const std::vector<char>& keep);
};

// n elements at the origin with the built-in attributes at their defaults; q and s are the Index order
// (q = index / (n - 1), s = index). Ids are left 0 (the node that makes them sets them).
Geometry MakePoints(size_t n);

}  // namespace dx5
