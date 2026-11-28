// DupliX 2 (design v5, 5 and 13): the evaluator. Geometry nodes make whole lists; a field is evaluated by the node
// it feeds, over that node's geometry (design v5 5), column by column.
#include "dxg5/eval_internal.h"

#include <algorithm>
#include <cmath>

namespace dx5 {

namespace {

// a column holding these values (mixed Int and Float become Float; a Vector among scalars makes all Vectors)
Column FromValues(const std::vector<Value>& vals, Type fallback)
{
    Type t = vals.empty() ? fallback : vals[0].type;
    for (const Value& v : vals) {
        if (v.type == t) continue;
        if (v.type == Type::Vector || t == Type::Vector) t = Type::Vector;
        else if ((v.type == Type::Float || t == Type::Float) && v.type != Type::Rotation && t != Type::Rotation) t = Type::Float;
    }
    if (t == Type::Any || t == Type::Geometry) t = Type::Float;
    Column c = MakeColumn(t, vals.size(), Value::F(0));
    for (size_t k = 0; k < vals.size(); k++) c.Set(k, vals[k]);
    return c;
}

Column One(const Value& v)
{
    Value x = v;
    if (x.type == Type::Any || x.type == Type::Geometry) x = Value::F(0);
    return MakeColumn(x.type, 1, x);
}

size_t Size(const Column& c, size_t ctxCount) { return c.size() == 1 ? 1 : ctxCount; }

}  // namespace

void Evaluator::Warn(const std::string& w)
{
    if (std::find(res_.warnings.begin(), res_.warnings.end(), w) == res_.warnings.end()) res_.warnings.push_back(w);
}

size_t Evaluator::Cap(size_t n)
{
    if (n > ctx_.maxElements) {
        Warn("cut to " + std::to_string(ctx_.maxElements) + " elements");
        return ctx_.maxElements;
    }
    return n;
}

uint64_t Evaluator::ElementId(const Node& n, size_t i, const std::string& inner) const
{
    if (n.legacyId) return dxg::MixId(n.legacyId, i);
    // inside a group's definition: the group node's key, then the inner node's (a group from a version 1 graph
    // keeps P1's ids for the points it makes)
    if (outerLegacy_ && inner.empty() && n.key == "pts") return dxg::MixId(outerLegacy_, i);
    std::string k = inner.empty() ? n.key : n.key + "/" + inner;
    if (!idPrefix_.empty()) k = idPrefix_ + "/" + k;
    return dxg::MixId(HashStr(k), i);
}

void Evaluator::BindGroup(Evaluator* outer, const Node* node)
{
    outer_ = outer, outerNode_ = node;
    idPrefix_ = outer->idPrefix_.empty() ? node->key : outer->idPrefix_ + "/" + node->key;
    outerLegacy_ = node->legacyId;
}

GeoPtr Evaluator::InputGeo(const Node& n, const std::string& input)
{
    const InputSlot* s = n.In(input);
    if (!s || !s->link.from) return std::make_shared<Geometry>(MakePoints(0));
    return Geo(s->link.from, s->link.output);
}

Column Evaluator::Input(const Node& n, const std::string& input, const GeoPtr& ctx)
{
    const InputSlot* s = n.In(input);
    if (!s) {
        const NodeDef* d = FindDef(n.type);
        const int k = d ? d->Input(input) : -1;
        return One(k >= 0 ? d->inputs[k].dflt : Value::F(0));
    }
    if (!s->link.from) return One(s->value);
    return Field(s->link.from, s->link.output, ctx);
}

Value Evaluator::Single(const Node& n, const std::string& input)
{
    static const GeoPtr none = std::make_shared<Geometry>(MakePoints(0));
    const InputSlot* s = n.In(input);
    if (!s || !s->link.from) return Input(n, input, none).Get(0);
    const Column c = Field(s->link.from, s->link.output, none);
    if (c.size() != 1) {
        Warn(n.key + "." + input + " cannot vary per element");
        const NodeDef* d = FindDef(n.type);
        const int k = d ? d->Input(input) : -1;
        return k >= 0 ? d->inputs[k].dflt : Value::F(0);
    }
    return c.Get(0);
}

GeoPtr Evaluator::Geo(uint32_t node, int output)
{
    const auto key = std::make_pair(node, output);
    auto it = geo_.find(key);
    if (it != geo_.end()) return it->second;
    const Node* n = g_.Find(node);
    GeoPtr r;
    if (!n) {
        r = std::make_shared<Geometry>(MakePoints(0));
    } else if (n->type == "each" && output == 1) {
        // where the chain inside starts: the zone's input only (the zone itself is being evaluated)
        r = InputGeo(*n, "geometry");
    } else if (std::find(stack_.begin(), stack_.end(), node) != stack_.end()) {
        Warn("a loop at " + n->key);
        r = std::make_shared<Geometry>(MakePoints(0));
    } else {
        stack_.push_back(node);
        r = GeoUncached(*n, output);
        stack_.pop_back();
    }
    geo_[key] = r;
    return r;
}

GeoPtr Evaluator::GeoUncached(const Node& n, int output)
{
    const std::string& t = n.type;
    if (t == "points") {
        const int64_t want = std::max<int64_t>(0, AsInt(Single(n, "count")));
        const size_t cnt = Cap((size_t)std::min<int64_t>(want, (int64_t)ctx_.maxElements + 1));
        auto g = std::make_shared<Geometry>(MakePoints(cnt));
        for (size_t k = 0; k < cnt; k++) g->id[k] = ElementId(n, k);
        return g;
    }
    if (t.rfind("set_", 0) == 0 || t == "store" || t == "capture" || t == "delete") {
        const GeoPtr base = InputGeo(n, "geometry");
        auto g = std::make_shared<Geometry>(*base);
        const size_t cnt = g->count;
        if (t == "capture") {
            if (output != 0) return g;
            const Column v = Input(n, "value", base);
            Column c = MakeColumn(v.type, cnt, Value::F(0));
            for (size_t k = 0; k < cnt; k++) c.Set(k, v.Get(k));
            g->attrs["#capture:" + n.key] = c;
            return g;
        }
        const Column where = Input(n, "where", base);
        if (t == "delete") {
            std::vector<char> keep(cnt);
            for (size_t k = 0; k < cnt; k++) keep[k] = !where.Get(k).b() && AsFloat(where.Get(k)) == 0;
            g->Filter(keep);
            return g;
        }
        std::string attr = t == "store" ? n.text : t.substr(4);
        const Column v = Input(n, t == "store" ? "value" : attr, base);
        if (attr.empty()) {
            Warn(n.key + ": Store needs a name");
            return g;
        }
        auto it = g->attrs.find(attr);
        if (it == g->attrs.end() || (t == "store" && it->second.type != v.type)) {
            Column c = MakeColumn(v.type == Type::Any ? Type::Float : v.type, cnt, Value::F(0));
            if (it != g->attrs.end())
                for (size_t k = 0; k < cnt; k++) c.Set(k, it->second.Get(k));
            g->attrs[attr] = c;
            it = g->attrs.find(attr);
        }
        Column& c = it->second;
        Value probe;
        if (cnt && !Convert(v.Get(0), c.type, &probe)) {
            Warn(n.key + ": " + attr + " wants " + TypeName(c.type) + ", got " + TypeName(v.type));
            return g;
        }
        for (size_t k = 0; k < cnt; k++)
            if (AsFloat(where.Get(k)) != 0) c.Set(k, v.Get(k));
        return g;
    }
    if (t == "each") return output == 1 ? InputGeo(n, "geometry") : InputGeo(n, "body");
    if (t == "param_geo") {
        if (outer_) return outer_->InputGeo(*outerNode_, n.text);
        Warn(n.key + ": a group's input outside a group");
        return std::make_shared<Geometry>(MakePoints(0));
    }
    const NodeDef* d = FindDef(t);
    if (d && d->group) {
        Geometry out;
        if (!ctx_.groupsByDefinition && NativeGroup(*this, n, &out)) return std::make_shared<Geometry>(std::move(out));
        if (GroupByDefinition(*this, n, &out)) return std::make_shared<Geometry>(std::move(out));
        Warn(n.key + ": " + t + " has no definition");
        return std::make_shared<Geometry>(MakePoints(0));
    }
    Warn(n.key + ": no geometry from " + t);
    return std::make_shared<Geometry>(MakePoints(0));
}

Column Evaluator::Field(uint32_t node, int output, const GeoPtr& ctx)
{
    const auto key = std::make_tuple(node, output, ctx.get());
    auto it = field_.find(key);
    if (it != field_.end()) return it->second;
    const Node* n = g_.Find(node);
    Column c;
    if (!n) {
        c = One(Value::F(0));
    } else if (std::find(stack_.begin(), stack_.end(), node) != stack_.end()) {
        Warn("a loop at " + n->key);
        c = One(Value::F(0));
    } else {
        stack_.push_back(node);
        c = FieldUncached(*n, output, ctx);
        stack_.pop_back();
    }
    keep_.push_back(ctx);
    field_[key] = c;
    return c;
}

Column Evaluator::FieldUncached(const Node& n, int output, const GeoPtr& ctx)
{
    const std::string& t = n.type;
    const size_t cnt = ctx->count;
    if (FindBuiltin(t)) {
        auto a = ctx->attrs.find(t);
        if (a != ctx->attrs.end()) return a->second;
        return MakeColumn(FindBuiltin(t)->type, cnt, FindBuiltin(t)->dflt);
    }
    if (t == "index" || t == "id") {
        Column c = MakeColumn(Type::Int, cnt, Value::I(0));
        for (size_t k = 0; k < cnt; k++) c.i[k] = t == "index" ? (int64_t)k : (int64_t)ctx->id[k];
        return c;
    }
    if (t == "count") return One(Value::I((int64_t)cnt));
    if (t == "time") return One(Value::F(ctx_.time));
    if (t == "seed") return One(Value::I(ctx_.seed));
    if (t == "macro" || t == "int_macro") {
        const int k = n.setting - 1;
        if (k < 0 || k >= 32) return One(t == "macro" ? Value::F(0) : Value::I(0));
        if (t == "macro") return One(Value::F(ctx_.macro ? ctx_.macro(k, ctx_.time) : 0));
        return One(Value::I(ctx_.intMacro ? ctx_.intMacro(k, ctx_.time) : 0));
    }
    if (t == "attr") {
        auto a = ctx->attrs.find(n.text);
        if (a != ctx->attrs.end() && a->second.size() == cnt) return a->second;
        if (cnt) Warn("no attribute '" + n.text + "'");
        return MakeColumn(Type::Float, cnt, Value::F(0));
    }
    if (t == "random") {
        const uint32_t salt = n.salt ? (uint32_t)n.salt : (uint32_t)HashStr(n.key);
        Column c = MakeColumn(Type::Float, cnt, Value::F(0));
        for (size_t k = 0; k < cnt; k++) c.f[k] = dxg::Rand01(ctx->id[k], ctx_.seed, salt);
        return c;
    }
    if (t == "capture") {
        auto a = ctx->attrs.find("#capture:" + n.key);
        if (a != ctx->attrs.end() && a->second.size() == cnt) return a->second;
        return MakeColumn(Type::Float, cnt, Value::F(0));
    }
    if (t == "expr") return EvalExpressionNode(*this, n, ctx, output);
    if (t == "param") {
        if (outer_) return outer_->Input(*outerNode_, n.text, ctx);
        Warn(n.key + ": a group's input outside a group");
        return MakeColumn(Type::Float, cnt, Value::F(0));
    }
    if (IsMathFamily(t)) {
        const MathOpDef* m = MathOpOf(n);
        if (!m) {
            Warn(n.key + ": no operation " + n.text);
            return MakeColumn(Type::Float, cnt, Value::F(0));
        }
        const int na = std::min<int>(m->arity, (int)n.inputs.size());
        Column in[3];
        size_t size = 1;
        for (int k = 0; k < na && k < 3; k++) {
            // a wire that does not fit the operator's input is kept but not read (design v5 6.2)
            if (LinkActive(g_, n, k)) in[k] = Input(n, n.inputs[k].name, ctx);
            else in[k] = MakeColumn(n.inputs[k].value.type, 1, n.inputs[k].value);
            if (in[k].size() != 1) size = cnt;
        }
        std::vector<Value> out(size);
        const std::string op = m->impl;
        for (size_t e = 0; e < size; e++) {
            Value a[3];
            for (int k = 0; k < na && k < 3; k++) a[k] = in[k].Get(e);
            std::string err;
            if (!MathOp(op, a, na, &out[e], &err)) {
                Warn(n.key + ": " + err);
                out[e] = Value::F(0);
            }
        }
        return FromValues(out, Type::Float);
    }
    if (t == "at") {
        const GeoPtr h = InputGeo(n, "geometry");
        const Column idx = Input(n, "index", ctx);
        const Column val = Input(n, "value", h);
        std::vector<Value> out(Size(idx, cnt));
        for (size_t e = 0; e < out.size(); e++) {
            if (!h->count) {
                out[e] = Value::F(0);
                continue;
            }
            const int64_t i = std::clamp<int64_t>(AsInt(idx.Get(e)), 0, (int64_t)h->count - 1);
            out[e] = val.Get((size_t)i);
        }
        return FromValues(out, val.type);
    }
    if (t == "accumulate") {
        const Column v = Input(n, "value", ctx);
        std::vector<Value> out(cnt);
        Value sum;
        for (size_t e = 0; e < cnt; e++) {
            const Value x = v.Get(e);
            if (e == 0) {
                sum = x;
            } else {
                const Value pair[2] = {sum, x};
                Value s;
                std::string err;
                if (MathOp("add", pair, 2, &s, &err)) sum = s;
            }
            out[e] = sum;
        }
        return FromValues(out, v.type);
    }
    if (t == "bounds" || t == "reduce") {
        const GeoPtr h = InputGeo(n, "geometry");
        const Column v = Input(n, "value", h);
        if (t == "bounds") {
            Vec3 lo{0, 0, 0}, hi{0, 0, 0};
            for (size_t e = 0; e < h->count; e++) {
                Value x;
                if (!Convert(v.Get(e), Type::Vector, &x)) continue;
                if (e == 0) lo = hi = x.v;
                lo = {std::min(lo.x, x.v.x), std::min(lo.y, x.v.y), std::min(lo.z, x.v.z)};
                hi = {std::max(hi.x, x.v.x), std::max(hi.y, x.v.y), std::max(hi.z, x.v.z)};
            }
            const Vec3 r = output == 0 ? lo : output == 1 ? hi : (lo + hi) * 0.5;
            return One(Value::V(r));
        }
        if (!h->count) return One(Value::F(0));
        Value acc = v.Get(0);
        for (size_t e = 1; e < h->count; e++) {
            const Value pair[2] = {acc, v.Get(e)};
            const char* op = n.setting == 1 ? "min" : n.setting == 2 ? "max" : "add";
            std::string err;
            Value r;
            if (MathOp(op, pair, 2, &r, &err)) acc = r;
        }
        if (n.setting == 3) {
            const Value pair[2] = {acc, Value::F((double)h->count)};
            Value r;
            std::string err;
            if (MathOp("div", pair, 2, &r, &err)) acc = r;
        }
        return One(acc);
    }
    if (t == "layer_set") {
        const int64_t k = AsInt(Single(n, "k"));
        return One(Value::B(k >= 1 && k <= kNumLayers && (ctx_.layersSet >> (k - 1) & 1)));
    }
    Warn(n.key + ": no value from " + t);
    return MakeColumn(Type::Float, cnt, Value::F(0));
}

dxg::EvalResult Evaluate(const Graph& g, const EvalContext& ctx)
{
    dxg::EvalResult res;
    const Node* out = g.OutputNode();
    if (!out) return res;
    res.hasOutput = true;
    Evaluator ev(g, ctx, res);
    res.copyScale = std::max(0.0, AsFloat(ev.Single(*out, "copy_scale")) / 100.0);
    const GeoPtr geo = ev.InputGeo(*out, "geometry");
    const Geometry& G = *geo;
    const size_t n = std::min(G.count, ctx.maxElements);
    if (G.count > n) ev.Warn("cut to " + std::to_string(ctx.maxElements) + " elements");
    auto col = [&](const char* name) -> const Column* {
        auto it = G.attrs.find(name);
        return it != G.attrs.end() && it->second.size() == G.count ? &it->second : nullptr;
    };
    const Column *pos = col("position"), *rot = col("rotation"), *scl = col("scale"), *op = col("opacity"), *src = col("source"),
                 *vis = col("visible"), *wt = col("weight"), *q = col("q"), *s = col("s");
    res.elements.resize(n);
    for (size_t k = 0; k < n; k++) {
        dxg::Element& e = res.elements[k];
        e.id = G.id[k];
        e.index = (uint32_t)k;
        if (pos) e.pos[0] = pos->v[k].x, e.pos[1] = pos->v[k].y, e.pos[2] = pos->v[k].z;
        if (rot)
            for (int i = 0; i < 9; i++) e.basis[i] = rot->r[k].m[i];
        if (scl) e.scale[0] = scl->v[k].x, e.scale[1] = scl->v[k].y, e.scale[2] = scl->v[k].z;
        if (op) e.opacity = std::clamp(op->f[k], 0.0, 1.0);
        if (src) e.source = (int)src->i[k];
        if (vis) e.visible = vis->i[k] != 0;
        if (wt) e.weight = wt->f[k];
        if (q) e.q = q->f[k];
        if (s) e.s = (int)s->i[k];
    }
    return res;
}

}  // namespace dx5
