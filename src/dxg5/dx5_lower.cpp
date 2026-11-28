// DupliX 2 (design v5, 4.2, 5.1, 8, 9, 11): code -> graph. A call is a node, an argument a wire; an expression
// made of operators, math functions and input values (attributes, macros, time, variables) is one Expression node.
#include <algorithm>
#include <functional>
#include <map>
#include <set>

#include "dxg5/eval_internal.h"
#include "dxg5/lang.h"

namespace dx5 {

namespace {

struct Ref {
    uint32_t node = 0;
    int output = 0;
    Type type = Type::Float;
    bool field = false;
    bool constant = false;
    Value value;
    bool fromFormula = false;  // a formula or an attribute read (not a node call): may not go in a variable bare
    bool geo() const { return type == Type::Geometry; }
};

const char* kReaders[] = {"position", "rotation", "scale", "opacity", "source", "visible", "weight", "q", "s",
                          "index",    "id",       "count", "time",    "seed"};
bool IsReader(const std::string& s)
{
    for (const char* r : kReaders)
        if (s == r) return true;
    return false;
}
Type ReaderType(const std::string& s, bool* field)
{
    *field = !(s == "count" || s == "time" || s == "seed");
    if (const BuiltinAttr* b = FindBuiltin(s)) return b->type;
    if (s == "time") return Type::Float;
    return Type::Int;  // index, id, count, seed
}

bool MacroName(const std::string& s, bool* isInt, int* k)
{
    if (s.size() < 2 || (s[0] != 'm' && s[0] != 'i') || s[1] == '0') return false;
    int v = 0;
    for (size_t j = 1; j < s.size(); j++) {
        if (s[j] < '0' || s[j] > '9' || v > 100) return false;
        v = v * 10 + (s[j] - '0');
    }
    if (v < 1 || v > 32) return false;
    *isInt = s[0] == 'i', *k = v - 1;
    return true;
}

bool Compatible(Type from, Type to)
{
    if (from == to || to == Type::Any || from == Type::Any) return from != Type::Geometry || to == Type::Geometry;
    if (from == Type::Geometry || to == Type::Geometry) return false;
    const bool scalarFrom = from == Type::Float || from == Type::Int || from == Type::Bool;
    if (to == Type::Float || to == Type::Int) return scalarFrom;
    if (to == Type::Vector) return from == Type::Float || from == Type::Int;
    if (to == Type::Bool) return false;
    return false;
}

Value Dummy(Type t)
{
    switch (t) {
    case Type::Int: return Value::I(1);
    case Type::Bool: return Value::B(true);
    case Type::Vector: return Value::V({1, 1, 1});
    case Type::Rotation: return Value::R(Rot{});
    default: return Value::F(1);
    }
}

std::unique_ptr<Expr> Clone(const Expr& e)
{
    auto c = std::make_unique<Expr>();
    c->kind = e.kind, c->pos = e.pos, c->text = e.text;
    for (const auto& a : e.args) c->args.push_back(Clone(*a));
    for (const auto& a : e.named) c->named.emplace_back(a.first, Clone(*a.second));
    return c;
}

// the named attributes known on a geometry (design v5 4.2 (5)), with their types
using Attrs = std::map<std::string, Type>;

const Attrs& GroupAttrs(const std::string& type)
{
    static const std::map<std::string, Attrs> m = {
        {"grid", {{"ix", Type::Int}, {"iy", Type::Int}, {"iz", Type::Int}, {"u", Type::Float}, {"v", Type::Float}, {"w", Type::Float}}},
        {"circle", {{"angle", Type::Float}, {"t", Type::Float}, {"normal", Type::Vector}}},
        {"line", {{"t", Type::Float}, {"normal", Type::Vector}}},
    };
    static const Attrs none;
    auto it = m.find(type);
    return it == m.end() ? none : it->second;
}

struct Env {
    const Attrs* attrs = nullptr;  // the named attributes of the geometry a field here is evaluated over
    std::string lambda;            // inside (e) => ...: the parameter's name
};

struct Zone {
    uint32_t each = 0;
    std::string key;
    Ref cur;  // the chain inside so far
    std::map<std::string, Ref> locals;
    Ref where;
    bool hasWhere = false;
    int counter = 0;
};

class Lowerer {
public:
    Lowerer(const Graph& prev, std::vector<Diag>* errs, std::vector<Diag>* notes) : prev_(prev), errs_(errs), notes_(notes) {}

    bool Run(const Program& p, Graph* out)
    {
        for (const Stmt& s : p.stmts) {
            if (!Top(s)) return false;
        }
        Finish();
        *out = g_;
        return true;
    }

private:
    const Graph& prev_;
    std::vector<Diag>* errs_;
    std::vector<Diag>* notes_;
    Graph g_;
    std::map<std::string, Ref> vars_;
    std::map<std::pair<uint32_t, int>, Attrs> geoAttrs_;
    Zone* zone_ = nullptr;
    bool failed_ = false;

    bool Err(Pos p, const std::string& m)
    {
        if (!failed_ && errs_) errs_->push_back({p, m});
        failed_ = true;
        return false;
    }
    void Note(Pos p, const std::string& m)
    {
        if (notes_) notes_->push_back({p, m});
    }

    uint32_t NewNode(const std::string& type, const std::string& key)
    {
        const uint32_t id = AddNode(g_, type, key, 0, 0);
        if (zone_) g_.Find(id)->zone = zone_->each;
        return id;
    }

    Attrs AttrsOf(const Ref& r) const
    {
        auto it = geoAttrs_.find({r.node, r.output});
        return it == geoAttrs_.end() ? Attrs{} : it->second;
    }

    uint32_t MacroNode(bool isInt, int k)
    {
        const std::string key = std::string(isInt ? "i" : "m") + std::to_string(k + 1);
        if (const Node* n = g_.FindKey(key))
            if (n->type == (isInt ? "int_macro" : "macro")) return n->id;
        Zone* z = zone_;
        zone_ = nullptr;  // shared by the whole graph, never inside a zone
        const uint32_t id = AddNode(g_, isInt ? "int_macro" : "macro", key, 0, 0);
        zone_ = z;
        g_.Find(id)->setting = k + 1;
        return id;
    }

    // ---- names ----
    const Ref* Var(const std::string& n) const
    {
        if (zone_) {
            auto l = zone_->locals.find(n);
            if (l != zone_->locals.end()) return &l->second;
        }
        auto v = vars_.find(n);
        return v == vars_.end() ? nullptr : &v->second;
    }

    // Type of a name inside a formula; false if unknown. `input` is set when it reads a variable (an input).
    bool NameType(const std::string& s, const Env& env, Type* t, bool* field, bool* input, Pos pos)
    {
        *input = false;
        if (const Ref* r = Var(s)) {
            if (r->geo()) return Err(pos, "'" + s + "' is a geometry, not a value");
            *t = r->type, *field = r->field, *input = true;
            return true;
        }
        if (s == "true" || s == "false") return *t = Type::Bool, *field = false, true;
        bool isInt = false;
        int k = 0;
        if (MacroName(s, &isInt, &k)) return *t = isInt ? Type::Int : Type::Float, *field = false, true;
        if (IsReader(s)) {
            *t = ReaderType(s, field);
            if (!env.lambda.empty() && *field)
                return Err(pos, "inside (" + env.lambda + ") => write " + env.lambda + "." + s);
            return true;
        }
        if (env.attrs) {
            auto a = env.attrs->find(s);
            if (a != env.attrs->end()) {
                if (!env.lambda.empty()) return Err(pos, "inside (" + env.lambda + ") => write " + env.lambda + "." + s);
                *t = a->second, *field = true;
                return true;
            }
        }
        return Err(pos, "unknown name '" + s + "'");
    }

    bool Pure(const Expr& e, const Env& env) const
    {
        switch (e.kind) {
        case Expr::Num:
        case Expr::Name: return true;
        case Expr::Unary:
        case Expr::Binary:
        case Expr::Cond:
            for (const auto& a : e.args)
                if (!Pure(*a, env)) return false;
            return true;
        case Expr::Call:
            if (!env.lambda.empty() && e.args.size() == 1 && e.named.empty() && e.args[0]->kind == Expr::Name &&
                e.args[0]->text == env.lambda && Var(e.text))
                return true;  // p(e)
            if (!IsMathFunction(e.text) || !e.named.empty()) return false;
            for (const auto& a : e.args)
                if (!Pure(*a, env)) return false;
            return true;
        case Expr::Member:
            if (e.args[0]->kind == Expr::Name && (e.args[0]->text == "parent" || e.args[0]->text == env.lambda)) return true;
            if (e.text == "x" || e.text == "y" || e.text == "z") return Pure(*e.args[0], env);
            return false;
        default: return false;
        }
    }

    // ---- values ----
    // Rewrites a pure formula for an Expression node: variables become inputs, node calls inside become inputs
    // _1, _2, ...; returns the static type.
    struct Formula {
        uint32_t node = 0;
        std::map<std::string, Ref> inputs;
        int next = 1;
        bool field = false;
        bool names = false;
    };

    // A part that is a node of its own (a node call, a member of one, a string, a lambda), not a formula to go into.
    bool Atomic(const Expr& e, const Env& env) const
    {
        switch (e.kind) {
        case Expr::Num:
        case Expr::Name:
        case Expr::Unary:
        case Expr::Binary:
        case Expr::Cond: return false;
        case Expr::Call:
            if (!env.lambda.empty() && e.args.size() == 1 && e.named.empty() && e.args[0]->kind == Expr::Name &&
                e.args[0]->text == env.lambda && Var(e.text))
                return false;
            return !(IsMathFunction(e.text) && e.named.empty());
        case Expr::Member:
            if (e.args[0]->kind == Expr::Name && (e.args[0]->text == "parent" || e.args[0]->text == env.lambda)) return false;
            return !(e.text == "x" || e.text == "y" || e.text == "z");
        default: return true;
        }
    }

    bool Rewrite(const Expr& e, const Env& env, const std::string& key, Formula& f, std::unique_ptr<Expr>* out, Type* type)
    {
        if (Atomic(e, env)) {
            const std::string in = "_" + std::to_string(f.next++);
            Ref r;
            if (!Value_(e, key + "." + in, env, &r)) return false;
            if (r.geo()) return Err(e.pos, "a geometry cannot be used in a formula");
            f.names = true;
            f.field |= r.field;
            *type = r.type;
            if (r.constant) {
                // a constant from a node-free call (cannot happen for impure, but keep it exact)
                auto c = std::make_unique<Expr>();
                c->kind = Expr::Num, c->pos = e.pos, c->text = r.value.type == Type::Int ? std::to_string(r.value.i) : Num(AsFloat(r.value));
                *out = std::move(c);
                return true;
            }
            f.inputs[in] = r;
            auto n = std::make_unique<Expr>();
            n->kind = Expr::Name, n->pos = e.pos, n->text = in;
            *out = std::move(n);
            return true;
        }
        auto c = std::make_unique<Expr>();
        c->kind = e.kind, c->pos = e.pos, c->text = e.text;
        switch (e.kind) {
        case Expr::Num:
            *type = NumberValue(e.text).type;
            *out = std::move(c);
            return true;
        case Expr::Name: {
            bool field = false, input = false;
            if (!NameType(e.text, env, type, &field, &input, e.pos)) return false;
            if (input) f.inputs[e.text] = *Var(e.text);
            if (e.text != "true" && e.text != "false") f.names = true;
            f.field |= field;
            *out = std::move(c);
            return true;
        }
        case Expr::Member: {
            const Expr& b = *e.args[0];
            if (b.kind == Expr::Name && b.text == env.lambda && !env.lambda.empty()) {
                // e.position -> position (the formula is read where it is used)
                bool field = true;
                *type = IsReader(e.text) ? ReaderType(e.text, &field) : (env.attrs && env.attrs->count(e.text) ? env.attrs->at(e.text) : Type::Any);
                f.field |= field, f.names = true;
                c->kind = Expr::Name;
                *out = std::move(c);
                return true;
            }
            if (b.kind == Expr::Name && b.text == "parent") {
                *type = Type::Any;
                f.field = true, f.names = true;
                c->args.push_back(Clone(b));
                *out = std::move(c);
                return true;
            }
            std::unique_ptr<Expr> a;
            Type at;
            if (!Rewrite(b, env, key, f, &a, &at)) return false;
            if (at != Type::Vector && at != Type::Any) return Err(e.pos, "." + e.text + " needs a Vector, not " + TypeName(at));
            *type = Type::Float;
            c->args.push_back(std::move(a));
            *out = std::move(c);
            return true;
        }
        case Expr::Call:
            if (!env.lambda.empty() && Var(e.text) && e.args.size() == 1 && e.args[0]->kind == Expr::Name && e.args[0]->text == env.lambda) {
                // p(e): the function in p, read here
                c->kind = Expr::Name;
                f.inputs[e.text] = *Var(e.text);
                f.field |= Var(e.text)->field, f.names = true;
                *type = Var(e.text)->type;
                *out = std::move(c);
                return true;
            }
            [[fallthrough]];
        default: {
            std::vector<Value> dummies;
            for (const auto& a : e.args) {
                std::unique_ptr<Expr> r;
                Type t;
                if (!Rewrite(*a, env, key, f, &r, &t)) return false;
                dummies.push_back(Dummy(t));
                c->args.push_back(std::move(r));
            }
            const std::string op = e.kind == Expr::Call ? e.text : e.kind == Expr::Cond ? "switch" : OpName(e.text, e.kind == Expr::Unary);
            Value res;
            std::string err;
            if (!MathOp(op, dummies.data(), (int)dummies.size(), &res, &err)) return Err(e.pos, err);
            *type = res.type;
            *out = std::move(c);
            return true;
        }
        }
    }

    bool ReaderNode(const std::string& s, const std::string& key, Ref* r)
    {
        bool field = false;
        const Type t = ReaderType(s, &field);
        r->node = NewNode(s, key);
        r->output = 0, r->type = t, r->field = field, r->fromFormula = field;
        return true;
    }

    // Any value expression -> a Ref (a node's output, or a constant).
    bool Value_(const Expr& e, const std::string& key, const Env& env, Ref* r)
    {
        if (e.kind == Expr::Num) {
            r->constant = true, r->value = NumberValue(e.text), r->type = r->value.type;
            return true;
        }
        if (e.kind == Expr::Str) return Err(e.pos, "a string is only a name (store, attr)");
        if (e.kind == Expr::Lambda) return Err(e.pos, "put (e) => ... in a variable of its own");
        if (e.kind == Expr::MemberExpr) return Err(e.pos, "write b = g.(...) on its own line");
        if (e.kind == Expr::Name) {
            const std::string& s = e.text;
            if (const Ref* v = Var(s)) return *r = *v, true;
            if (s == "true" || s == "false") return r->constant = true, r->value = Value::B(s == "true"), r->type = Type::Bool, true;
            bool isInt = false;
            int k = 0;
            if (MacroName(s, &isInt, &k)) {
                r->node = MacroNode(isInt, k), r->output = 0, r->type = isInt ? Type::Int : Type::Float, r->field = false;
                return true;
            }
            if (IsReader(s)) {
                bool field = false;
                ReaderType(s, &field);
                if (!env.lambda.empty() && field) return Err(e.pos, "inside (" + env.lambda + ") => write " + env.lambda + "." + s);
                return ReaderNode(s, key, r);
            }
            if (env.attrs && env.attrs->count(s)) {
                if (!env.lambda.empty()) return Err(e.pos, "inside (" + env.lambda + ") => write " + env.lambda + "." + s);
                r->node = NewNode("attr", key);
                g_.Find(r->node)->text = s;
                r->type = env.attrs->at(s), r->field = true, r->fromFormula = true;
                return true;
            }
            return Err(e.pos, "unknown name '" + s + "'");
        }
        if (e.kind == Expr::Call && !IsMathFunction(e.text) && FindDef(e.text)) return Call(e, key, env, r);
        if (e.kind == Expr::Member && e.args[0]->kind == Expr::Call && !IsMathFunction(e.args[0]->text)) {
            const Expr& call = *e.args[0];
            const NodeDef* d = FindDef(call.text);
            if (d && d->name == "at") return Call(call, key, env, r, e.text);
            if (d) {
                if (!Call(call, key, env, r)) return false;
                const int o = d->Output(e.text);
                if (o < 0) return Err(e.pos, call.text + " has no output '" + e.text + "'");
                r->output = o, r->type = d->outputs[o].type, r->field = d->outputs[o].field;
                return true;
            }
        }
        if (e.kind == Expr::Member && e.args[0]->kind == Expr::Name) {
            const Ref* v = Var(e.args[0]->text);
            if (v && v->geo()) return Err(e.pos, "write " + e.args[0]->text + "." + e.text + " on its own line: b = " + e.args[0]->text + "." + e.text);
        }
        if (e.kind == Expr::Call && !IsMathFunction(e.text) && !Pure(e, env)) return Err(e.pos, "unknown function '" + e.text + "'");
        // a formula
        Formula f;
        std::unique_ptr<Expr> rw;
        Type t;
        if (!Rewrite(e, env, key, f, &rw, &t)) return false;
        if (!f.names && f.inputs.empty()) {
            Value v;
            if (EvalConst(*rw, &v)) {
                r->constant = true, r->value = v, r->type = v.type;
                return true;
            }
        }
        if (rw->kind == Expr::Name && f.inputs.empty()) {
            // (e) => e.position is the Position node; (e) => e.ix reads the attribute
            if (IsReader(rw->text)) return ReaderNode(rw->text, key, r);
            bool isInt = false;
            int mk = 0;
            if (!MacroName(rw->text, &isInt, &mk) && rw->text != "true" && rw->text != "false") {
                r->node = NewNode("attr", key);
                g_.Find(r->node)->text = rw->text;
                r->type = t, r->field = true, r->fromFormula = true;
                return true;
            }
        }
        r->node = NewNode("expr", key);
        Node* n = g_.Find(r->node);
        n->text = PrintExpr(*rw);
        for (const auto& in : f.inputs) {
            InputSlot s;
            s.name = in.first;
            s.link = {in.second.node, in.second.output};
            if (in.second.constant) s.link = {}, s.value = in.second.value;
            g_.Find(r->node)->inputs.push_back(s);
        }
        r->output = 0, r->type = t, r->field = f.field, r->fromFormula = true;
        return true;
    }

    bool GeoArg(const Expr& e, const std::string& key, Ref* r)
    {
        if (e.kind == Expr::Name) {
            const Ref* v = Var(e.text);
            if (!v) return Err(e.pos, "unknown name '" + e.text + "'");
            if (!v->geo()) return Err(e.pos, "type mismatch: '" + e.text + "' is a " + TypeName(v->type) + ", a Geometry is wanted");
            *r = *v;
            return true;
        }
        if (e.kind == Expr::Call && FindDef(e.text)) {
            Env none;
            if (!Call(e, key, none, r)) return false;
            if (!r->geo()) return Err(e.pos, "type mismatch: " + e.text + "() gives a " + TypeName(r->type) + ", a Geometry is wanted");
            return true;
        }
        return Err(e.pos, "type mismatch: a Geometry is wanted");
    }

    // A node call. `atMember`: at(h, i).<name> reads <name> on h.
    bool Call(const Expr& e, const std::string& key, const Env& env, Ref* r, const std::string& atMember = "")
    {
        const NodeDef* d = FindDef(e.text);
        if (!d) return Err(e.pos, "unknown function '" + e.text + "'");
        if (d->name == "output") {
            if (g_.OutputNode()) return Err(e.pos, "only one Output");
        }
        const uint32_t id = NewNode(d->name, key);
        const std::string nkey = g_.Find(id)->key;
        // the arguments to inputs
        std::vector<std::pair<int, const Expr*>> args;
        size_t pos = 0;
        for (const auto& a : e.args) {
            if (a->kind == Expr::Str && d->text == "name") {
                g_.Find(id)->text = a->text;
                continue;
            }
            if (pos >= d->inputs.size()) return Err(a->pos, e.text + "() takes " + std::to_string(d->inputs.size()) + " arguments");
            args.push_back({(int)pos++, a.get()});
        }
        for (const auto& a : e.named) {
            if (a.first == "salt" && d->name == "random") {
                Value v;
                if (!EvalConst(*a.second, &v)) return Err(a.second->pos, "salt must be a number");
                g_.Find(id)->salt = (uint64_t)AsInt(v);
                continue;
            }
            if (a.first == "name" && d->text == "name" && a.second->kind == Expr::Str) {
                g_.Find(id)->text = a.second->text;
                continue;
            }
            if (a.first == "op" && d->setting == "op" && a.second->kind == Expr::Str) {
                const std::string& o = a.second->text;
                g_.Find(id)->setting = o == "min" ? 1 : o == "max" ? 2 : o == "mean" ? 3 : 0;
                continue;
            }
            const int k = d->Input(a.first);
            if (k < 0) return Err(a.second->pos, e.text + "() has no input '" + a.first + "'");
            for (const auto& x : args)
                if (x.first == k) return Err(a.second->pos, "'" + a.first + "' is given twice");
            args.push_back({k, a.second.get()});
        }
        if (d->name == "at" && !atMember.empty()) {
            bool given = false;
            for (const auto& x : args) given |= x.first == 2;
            if (given) return Err(e.pos, "at(h, i, value).name: give the value once");
        }
        // geometry inputs first: the fields of the others are evaluated over them
        std::map<std::string, Ref> geos;
        for (const auto& x : args) {
            const InputDef& in = d->inputs[x.first];
            if (in.type != Type::Geometry) continue;
            Ref gr;
            if (!GeoArg(*x.second, nkey + "_" + in.name, &gr)) return false;
            geos[in.name] = gr;
            g_.Find(id)->In(in.name)->link = {gr.node, gr.output};
        }
        Type outType = d->outputs.empty() ? Type::Float : d->outputs[0].type;
        Type storedType = Type::Any;
        for (const auto& x : args) {
            const InputDef& in = d->inputs[x.first];
            if (in.type == Type::Geometry) continue;
            Env sub = env;
            Attrs ctxAttrs;
            if (in.context == "make") {
                sub.attrs = nullptr;
            } else if (!in.context.empty()) {
                auto gi = geos.find(in.context);
                if (gi != geos.end()) ctxAttrs = AttrsOf(gi->second);
                sub.attrs = &ctxAttrs;
            }
            if (!in.context.empty()) sub.lambda.clear();
            Ref v;
            if (!Value_(*x.second, nkey + "." + in.name, sub, &v)) return false;
            if (v.geo()) return Err(x.second->pos, "type mismatch: " + in.name + " wants " + TypeName(in.type) + ", got a Geometry");
            if (!Compatible(v.type, in.type))
                return Err(x.second->pos, "type mismatch: " + in.name + " wants " + TypeName(in.type) + ", got " + TypeName(v.type));
            if (v.field && (in.structural || !in.field)) return Err(x.second->pos, in.name + " cannot vary per element");
            InputSlot* s = g_.Find(id)->In(in.name);
            if (v.constant) {
                Value c;
                if (!Convert(v.value, in.type == Type::Any ? v.value.type : in.type, &c)) c = v.value;
                s->value = c;
            } else {
                s->link = {v.node, v.output};
            }
            if (in.name == "value") storedType = v.type;
            if (in.type == Type::Any && (d->name == "at" || d->name == "accumulate" || d->name == "capture" || d->name == "reduce") &&
                in.name == "value")
                outType = v.type;
        }
        if (d->name == "at" && !atMember.empty()) {
            // the attribute read over h
            Env sub;
            Attrs ctxAttrs;
            auto gi = geos.find("geometry");
            if (gi == geos.end()) return Err(e.pos, "at() needs a geometry");
            ctxAttrs = AttrsOf(gi->second);
            sub.attrs = &ctxAttrs;
            Expr name;
            name.kind = Expr::Name, name.pos = e.pos, name.text = atMember;
            Ref v;
            if (!Value_(name, nkey + ".value", sub, &v)) return false;
            InputSlot* s = g_.Find(id)->In("value");
            if (v.constant) s->value = v.value;
            else s->link = {v.node, v.output};
            outType = v.type;
        }
        // the geometry outputs' attributes
        if (!d->outputs.empty() && d->outputs[0].type == Type::Geometry) {
            Attrs a;
            auto gi = geos.find("geometry");
            if (gi != geos.end()) a = AttrsOf(gi->second);
            for (const auto& x : GroupAttrs(d->name)) a[x.first] = x.second;
            if (d->name == "store" && !g_.Find(id)->text.empty()) {
                if (FindBuiltin(g_.Find(id)->text)) return Err(e.pos, "store() cannot use the built-in name '" + g_.Find(id)->text + "'");
                a[g_.Find(id)->text] = storedType;
            }
            geoAttrs_[{id, 0}] = a;
        }
        r->node = id, r->output = 0, r->type = outType, r->constant = false;
        r->field = !d->outputs.empty() && d->outputs[0].field;
        if (d->name == "capture") r->type = Type::Geometry, r->field = false;
        captureType_[id] = outType;
        return true;
    }

    std::map<uint32_t, Type> captureType_;  // the type a capture() call's value has

    // ---- statements ----
    std::string KeyFor(const std::string& name, Pos p)
    {
        const std::string k = UniqueKey(g_, name);
        if (k != name) Note(p, "renamed " + name + " to " + k + ": write distinct names to keep random values");
        return k;
    }

    bool Bind(const std::string& name, const Ref& r)
    {
        vars_[name] = r;
        return true;
    }

    bool Top(const Stmt& s)
    {
        switch (s.kind) {
        case Stmt::Assign: return Assign(s);
        case Stmt::Each: return EachStmt(s);
        case Stmt::ExprStmt: {
            const Expr& e = *s.value;
            if (e.kind != Expr::Call || e.text != "output") return Err(s.pos, "a line must name its result (x = ...) or be output(...)");
            Ref r;
            const Node* old = prev_.OutputNode();
            return Call(e, old ? old->key : "out", Env{}, &r);
        }
        case Stmt::If: return Err(s.pos, "if is only inside each { }");
        case Stmt::Compound: return Err(s.pos, s.op + " is only inside each { }");
        case Stmt::Group: return Err(s.pos, "groups cannot be defined in a graph's code yet");
        case Stmt::Return: return Err(s.pos, "return is only inside a group");
        }
        return false;
    }

    bool Assign(const Stmt& s)
    {
        const Expr& e = *s.value;
        if (s.names.size() == 2) {
            if (e.kind != Expr::Call || e.text != "capture") return Err(s.pos, "two names are only for capture(...)");
            Ref r;
            if (!Call(e, KeyFor(s.names[1], s.pos), Env{}, &r)) return false;
            Ref v = r;
            v.output = 1, v.type = captureType_[r.node], v.field = true;
            Bind(s.names[0], r);
            return Bind(s.names[1], v);
        }
        const std::string& name = s.names[0];
        // b = g.position / b = g.(expr): a capture on g's flow
        if ((e.kind == Expr::Member || e.kind == Expr::MemberExpr) && e.args[0]->kind == Expr::Name) {
            const Ref* g = Var(e.args[0]->text);
            if (g && g->geo()) {
                const std::string gname = e.args[0]->text;
                const uint32_t id = NewNode("capture", KeyFor(name, s.pos));
                g_.Find(id)->In("geometry")->link = {g->node, g->output};
                Attrs a = AttrsOf(*g);
                Env sub;
                sub.attrs = &a;
                Ref v;
                if (e.kind == Expr::Member) {
                    Expr n;
                    n.kind = Expr::Name, n.pos = e.pos, n.text = e.text;
                    if (!Value_(n, g_.Find(id)->key + ".value", sub, &v)) return false;
                } else if (!Value_(*e.args[1], g_.Find(id)->key + ".value", sub, &v)) {
                    return false;
                }
                InputSlot* in = g_.Find(id)->In("value");
                if (v.constant) in->value = v.value;
                else in->link = {v.node, v.output};
                Ref geo{id, 0, Type::Geometry};
                geoAttrs_[{id, 0}] = a;
                Bind(gname, geo);
                Ref val{id, 1, v.type, true};
                return Bind(name, val);
            }
        }
        if (e.kind == Expr::Lambda) {
            Env lam;
            lam.lambda = e.text;
            Ref r;
            if (!Value_(*e.args[0], KeyFor(name, s.pos), lam, &r)) return false;
            r.fromFormula = false;
            return Bind(name, r);
        }
        Ref r;
        const std::string key = KeyFor(name, s.pos);
        if (e.kind == Expr::Call && FindDef(e.text) && !IsMathFunction(e.text)) {
            if (!Call(e, key, Env{}, &r)) return false;
            return Bind(name, r);
        }
        if (!Value_(e, key, Env{}, &r)) return false;
        if (r.field && r.fromFormula)
            return Err(s.pos, "a field in a variable needs (e) => ...: write " + name + " = (e) => e.position; to copy values now write " +
                                  name + " = g.position");
        return Bind(name, r);
    }

    // ---- each ----
    bool EachStmt(const Stmt& s)
    {
        if (zone_) return Err(s.pos, "each cannot be inside each");
        const Ref* in = Var(s.input);
        if (!in) return Err(s.pos, "unknown name '" + s.input + "'");
        if (!in->geo()) return Err(s.pos, "'" + s.input + "' is not a geometry");
        const Ref input = *in;
        const std::string key = s.names.empty() ? UniqueKey(g_, s.input) : KeyFor(s.names[0], s.pos);
        const uint32_t id = NewNode("each", key);
        g_.Find(id)->In("geometry")->link = {input.node, input.output};
        geoAttrs_[{id, 1}] = AttrsOf(input);
        Zone z;
        z.each = id, z.key = key;
        z.cur = Ref{id, 1, Type::Geometry};
        zone_ = &z;
        for (const Stmt& b : s.body)
            if (!Inner(b)) {
                zone_ = nullptr;
                return false;
            }
        zone_ = nullptr;
        g_.Find(id)->In("body")->link = {z.cur.node, z.cur.output};
        geoAttrs_[{id, 0}] = AttrsOf(z.cur);
        return Bind(s.names.empty() ? s.input : s.names[0], Ref{id, 0, Type::Geometry});
    }

    std::string InnerKey() { return zone_->key + "." + std::to_string(++zone_->counter); }

    bool SetAttr(const std::string& attr, const Expr& value, Pos pos)
    {
        Zone& z = *zone_;
        Attrs a = AttrsOf(z.cur);
        Env env;
        env.attrs = &a;
        const bool builtin = FindBuiltin(attr) != nullptr;
        if (!builtin && !a.count(attr)) return Err(pos, "internal: not an attribute");
        const std::string type = builtin ? "set_" + attr : "store";
        const uint32_t id = NewNode(type, InnerKey());
        Node* n = g_.Find(id);
        if (!builtin) n->text = attr;
        n->In("geometry")->link = {z.cur.node, z.cur.output};
        const std::string inName = builtin ? attr : "value";
        Ref v;
        if (!Value_(value, n->key + "." + inName, env, &v)) return false;
        const InputDef& def = FindDef(type)->inputs[FindDef(type)->Input(inName)];
        if (v.geo() || !Compatible(v.type, def.type))
            return Err(value.pos, "type mismatch: " + attr + " wants " + TypeName(def.type) + ", got " + TypeName(v.type));
        n = g_.Find(id);
        if (v.constant) {
            Value c;
            n->In(inName)->value = Convert(v.value, def.type == Type::Any ? v.value.type : def.type, &c) ? c : v.value;
        } else {
            n->In(inName)->link = {v.node, v.output};
        }
        if (z.hasWhere) n->In("where")->link = {z.where.node, z.where.output};
        Attrs na = a;
        if (!builtin) na[attr] = v.type;
        geoAttrs_[{id, 0}] = na;
        z.cur = Ref{id, 0, Type::Geometry};
        return true;
    }

    // a value captured on the chain inside the zone (a local, an if's condition)
    bool CaptureHere(const Expr& value, Ref* out)
    {
        Zone& z = *zone_;
        Attrs a = AttrsOf(z.cur);
        Env env;
        env.attrs = &a;
        const uint32_t id = NewNode("capture", InnerKey());
        g_.Find(id)->In("geometry")->link = {z.cur.node, z.cur.output};
        Ref v;
        if (!Value_(value, g_.Find(id)->key + ".value", env, &v)) return false;
        if (v.geo()) return Err(value.pos, "a geometry cannot be a local");
        InputSlot* in = g_.Find(id)->In("value");
        if (v.constant) in->value = v.value;
        else in->link = {v.node, v.output};
        geoAttrs_[{id, 0}] = a;
        z.cur = Ref{id, 0, Type::Geometry};
        *out = Ref{id, 1, v.type, true};
        return true;
    }

    std::unique_ptr<Expr> Bin(const char* op, std::unique_ptr<Expr> a, std::unique_ptr<Expr> b, Pos p)
    {
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Binary, e->pos = p, e->text = op;
        e->args.push_back(std::move(a));
        e->args.push_back(std::move(b));
        return e;
    }

    bool Inner(const Stmt& s)
    {
        Zone& z = *zone_;
        switch (s.kind) {
        case Stmt::Assign: {
            if (s.names.size() != 1) return Err(s.pos, "one name per line inside each");
            const std::string& name = s.names[0];
            if (name == "index" || name == "id" || name == "count") return Err(s.pos, name + " cannot be set");
            Attrs a = AttrsOf(z.cur);
            if (FindBuiltin(name) || (a.count(name) && !z.locals.count(name))) return SetAttr(name, *s.value, s.pos);
            Ref r;
            if (!CaptureHere(*s.value, &r)) return false;
            g_.Find(r.node)->text = name;  // the local's name (written back by the printer)
            z.locals[name] = r;
            return true;
        }
        case Stmt::Compound: {
            const std::string& name = s.names[0];
            Attrs a = AttrsOf(z.cur);
            if (!FindBuiltin(name) && !a.count(name)) return Err(s.pos, s.op + " needs an attribute; '" + name + "' is not one");
            auto target = std::make_unique<Expr>();
            target->kind = Expr::Name, target->pos = s.pos, target->text = name;
            const std::string op(1, s.op[0]);
            auto v = Bin(op.c_str(), std::move(target), Clone(*s.value), s.pos);
            return SetAttr(name, *v, s.pos);
        }
        case Stmt::If: {
            Ref cond;
            if (!CaptureHere(*s.value, &cond)) return false;
            if (cond.type != Type::Bool && cond.type != Type::Any) return Err(s.value->pos, "if needs a true / false condition");
            const Ref outer = z.where;
            const bool had = z.hasWhere;
            for (int branch = 0; branch < 2; branch++) {
                const std::vector<Stmt>& body = branch ? s.elseBody : s.body;
                if (body.empty()) continue;
                Ref sel = cond;
                if (branch) {
                    const uint32_t nn = NewNode("math_not", InnerKey());
                    g_.Find(nn)->In("a")->link = {cond.node, cond.output};
                    sel = Ref{nn, 0, Type::Bool, true};
                }
                if (had) {
                    const uint32_t an = NewNode("math_and", InnerKey());
                    g_.Find(an)->In("a")->link = {outer.node, outer.output};
                    g_.Find(an)->In("b")->link = {sel.node, sel.output};
                    sel = Ref{an, 0, Type::Bool, true};
                }
                z.where = sel, z.hasWhere = true;
                for (const Stmt& b : body)
                    if (!Inner(b)) return false;
            }
            z.where = outer, z.hasWhere = had;
            return true;
        }
        case Stmt::ExprStmt: {
            const Expr& e = *s.value;
            if (e.kind == Expr::Call && e.text == "store" && e.args.size() == 2 && e.args[0]->kind == Expr::Str && e.named.empty()) {
                const std::string name = e.args[0]->text;
                if (FindBuiltin(name)) return Err(s.pos, "store() cannot use the built-in name '" + name + "'");
                const uint32_t id = NewNode("store", InnerKey());
                g_.Find(id)->text = name;
                g_.Find(id)->In("geometry")->link = {z.cur.node, z.cur.output};
                Attrs a = AttrsOf(z.cur);
                Env env;
                env.attrs = &a;
                Ref v;
                if (!Value_(*e.args[1], g_.Find(id)->key + ".value", env, &v)) return false;
                InputSlot* in = g_.Find(id)->In("value");
                if (v.constant) in->value = v.value;
                else in->link = {v.node, v.output};
                if (z.hasWhere) g_.Find(id)->In("where")->link = {z.where.node, z.where.output};
                a[name] = v.type;
                geoAttrs_[{id, 0}] = a;
                z.cur = Ref{id, 0, Type::Geometry};
                return true;
            }
            return Err(s.pos, "inside each: set an attribute (position = ...), name a value, if, or store(\"name\", value)");
        }
        default: return Err(s.pos, "not inside each");
        }
    }

    // ---- ids, positions ----
    void Finish()
    {
        // nodes whose key (and type) were in `previous` keep their id, place, salt and legacy id
        std::map<uint32_t, uint32_t> remap;
        uint32_t next = prev_.nextId;
        std::set<uint32_t> used;
        for (const Node& n : g_.nodes) {
            const Node* p = prev_.FindKey(n.key);
            if (p && p->type == n.type) used.insert(p->id);
        }
        for (uint32_t u : used) next = std::max(next, u + 1);
        for (Node& n : g_.nodes) {
            const Node* p = prev_.FindKey(n.key);
            if (p && p->type == n.type) {
                remap[n.id] = p->id;
                n.x = p->x, n.y = p->y;
                if (!n.salt) n.salt = p->salt;
                n.legacyId = p->legacyId;
            } else {
                remap[n.id] = next++;
                n.x = -1e9;  // placed below
            }
        }
        for (Node& n : g_.nodes) {
            n.id = remap[n.id];
            if (n.zone) n.zone = remap[n.zone];
            for (InputSlot& s : n.inputs)
                if (s.link.from) s.link.from = remap[s.link.from];
        }
        g_.nextId = next;
        g_.revision = prev_.revision;
        for (int k = 0; k < kNumMacros; k++) g_.macros[k] = prev_.macros[k];
        for (int k = 0; k < kNumIntMacros; k++) g_.intMacros[k] = prev_.intMacros[k];
        // new nodes: columns by depth (sources on the left)
        std::map<uint32_t, int> depth;
        std::function<int(uint32_t)> D = [&](uint32_t id) -> int {
            auto it = depth.find(id);
            if (it != depth.end()) return it->second;
            depth[id] = 0;
            int d = 0;
            if (const Node* n = g_.Find(id))
                for (const InputSlot& s : n->inputs)
                    if (s.link.from) d = std::max(d, D(s.link.from) + 1);
            return depth[id] = d;
        };
        std::map<int, int> rows;
        for (Node& n : g_.nodes) {
            if (n.x != -1e9) continue;
            const int d = D(n.id);
            n.x = 40 + d * 230.0;
            n.y = 40 + (rows[d]++) * 120.0;
        }
    }
};

}  // namespace

bool CodeToGraph(const std::string& code, const Graph& previous, Graph* out, std::vector<Diag>* errors, std::vector<Diag>* notes)
{
    Program p;
    Diag d;
    if (!ParseProgram(code, &p, &d)) {
        if (errors) errors->push_back(d);
        return false;
    }
    Lowerer l(previous, errors, notes);
    Graph g;
    if (!l.Run(p, &g)) return false;
    *out = g;
    return true;
}

}  // namespace dx5
