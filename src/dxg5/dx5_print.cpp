// DupliX 2 (design v5, 8.2, 13): graph -> code, and comparing graphs. Geometry nodes are statements named by
// their keys; a value node used once whose key is the one the reader would give it is written inside the
// expression that uses it, other value nodes are statements of their own (a formula over elements as (e) => ...).
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

#include "dxg5/lang.h"

namespace dx5 {

std::string GraphToCode(const Graph& g);
std::string ExprText(const Graph& g, uint32_t node, int output);
bool SameGraph(const Graph& a, const Graph& b, std::string* why);

namespace {

using Out = std::pair<uint32_t, int>;

bool IsGeoNode(const Graph& g, const Node& n)
{
    const NodeDef* d = DefFor(g, n.type);
    return d && !d->outputs.empty() && d->outputs[0].type == Type::Geometry;
}

bool IsReaderType(const std::string& t)
{
    static const char* r[] = {"position", "rotation", "scale", "opacity", "source", "visible", "weight", "q", "s",
                              "index",    "id",       "count", "time",    "seed"};
    for (const char* x : r)
        if (t == x) return true;
    return false;
}

std::string ConstText(const Value& v)
{
    switch (v.type) {
    case Type::Int: return std::to_string(v.i);
    case Type::Bool: return v.i ? "true" : "false";
    case Type::Vector: return "vec(" + Num(v.v.x) + ", " + Num(v.v.y) + ", " + Num(v.v.z) + ")";
    case Type::Float: return Num(v.f);
    default: return "0";
    }
}

bool ValueEqual(const Value& a, const Value& b)
{
    const bool sa = a.type == Type::Float || a.type == Type::Int || a.type == Type::Bool;
    const bool sb = b.type == Type::Float || b.type == Type::Int || b.type == Type::Bool;
    if (sa && sb) return AsFloat(a) == AsFloat(b);
    if (a.type != b.type) return false;
    if (a.type == Type::Vector) return a.v.x == b.v.x && a.v.y == b.v.y && a.v.z == b.v.z;
    return true;
}

bool IsDefault(const NodeDef* d, const InputSlot& s)
{
    if (!d) return false;
    const int k = d->Input(s.name);
    return k >= 0 && !s.link.from && ValueEqual(d->inputs[k].dflt, s.value) && d->inputs[k].dflt.type == s.value.type;
}

class Printer {
public:
    explicit Printer(const Graph& g) : g_(g)
    {
        for (const Node& n : g.nodes)
            for (const InputSlot& s : n.inputs)
                if (s.link.from) uses_[{s.link.from, s.link.output}]++, consumers_[s.link.from].push_back({n.id, s.name});
    }

    std::string Code()
    {
        std::string out;
        for (const UserGroup& u : g_.groups) out += GroupCode(u);
        for (uint32_t id : Order()) out += Statement(*g_.Find(id));
        return out;
    }

    // group name(params) { body; return ... } (design v5 12.1)
    static std::string GroupCode(const UserGroup& u)
    {
        if (!u.body) return "";
        std::string head = "group " + u.name + "(";
        for (size_t k = 0; k < u.def.inputs.size(); k++) {
            const InputDef& in = u.def.inputs[k];
            head += (k ? ", " : "") + in.name + ": " + TypeWord(in.type);
            if (in.type == Type::Geometry) continue;
            if (in.structural) head += " structural";
            head += " = " + ConstText(in.dflt);
        }
        head += ") {\n";
        Graph body = *u.body;
        Printer bp(body);
        bp.groupBody_ = true;
        std::string inner;
        for (uint32_t id : bp.Order()) inner += bp.Statement(*body.Find(id));
        const Node* ret = nullptr;
        for (const Node& n : body.nodes)
            if (n.type == "group_output") ret = &n;
        if (ret) {
            if (ret->inputs.size() == 1 && (ret->inputs[0].name == "geometry" || ret->inputs[0].name == "value")) {
                inner += "return " + bp.Arg(*ret, ret->inputs[0], {0, 0}, "") + "\n";
            } else {
                std::string r = "return (";
                for (size_t k = 0; k < ret->inputs.size(); k++)
                    r += (k ? ", " : "") + ret->inputs[k].name + ": " + bp.Arg(*ret, ret->inputs[k], {0, 0}, "");
                inner += r + ")\n";
            }
        }
        std::string out = head;
        size_t a = 0;
        while (a < inner.size()) {
            size_t b = inner.find('\n', a);
            if (b == std::string::npos) b = inner.size();
            out += "  " + inner.substr(a, b - a) + "\n";
            a = b + 1;
        }
        return out + "}\n";
    }

    // the text of a value output as an expression (inline, even when it is a statement)
    std::string Inline(uint32_t node, int output, Out ctx)
    {
        const Node* n = g_.Find(node);
        return n ? InlineText(*n, output, ctx, "") : "0";
    }

private:
    const Graph& g_;
    std::map<Out, int> uses_;
    std::map<uint32_t, std::vector<std::pair<uint32_t, std::string>>> consumers_;
    std::map<Out, std::string> name_;

    bool IsMacro(const Node& n) const { return n.type == "macro" || n.type == "int_macro"; }
    bool IsParam(const Node& n) const { return n.type == "param" || n.type == "param_geo" || n.type == "group_output"; }
    bool groupBody_ = false;

    int Uses(uint32_t id) const
    {
        int u = 0;
        for (const auto& kv : uses_)
            if (kv.first.first == id) u += kv.second;
        return u;
    }

    // expr(...) { ... }: an Expression node with several outputs, a named output, or an unwired input (design v5 8.2)
    bool Block(const Node& n) const
    {
        if (n.type != "expr") return false;
        const std::vector<OutputDef> outs = OutputsOf(g_, n);
        if (outs.size() != 1 || outs[0].name != "value") return true;
        std::vector<FormulaLine> lines;
        Diag d;
        if (!ParseFormula(n.text, &lines, &d) || lines.size() != 1 || lines[0].name != "value") return true;
        if (n.text.find('=') != std::string::npos) {
            // "value = ..." written out, or a private line: a block
            for (const FormulaLine& l : lines)
                if (l.name[0] == '_') return true;
        }
        for (const InputSlot& s : n.inputs)
            if (!s.link.from) return true;
        return false;
    }

    static bool Ident(const std::string& k)
    {
        if (k.empty() || !(std::isalpha((unsigned char)k[0]) || k[0] == '_')) return false;
        for (char c : k)
            if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
        return true;
    }
    static std::string IdentOf(std::string k)
    {
        for (char& c : k)
            if (!(std::isalnum((unsigned char)c) || c == '_')) c = '_';
        if (k.empty() || std::isdigit((unsigned char)k[0])) k = "e_" + k;
        return k;
    }

    // a value node written inside the expression that uses it
    bool Inlined(const Node& n) const
    {
        if (IsGeoNode(g_, n) || IsMacro(n) || n.type == "capture" || Block(n) || IsParam(n)) return false;
        auto c = consumers_.find(n.id);
        if (c == consumers_.end() || c->second.size() != 1) return false;
        const Node* user = g_.Find(c->second[0].first);
        if (!user || user->zone != n.zone) return false;
        return n.key == user->key + "." + c->second[0].second;
    }

    // ---- the named attributes known on a geometry output (for bare names vs attr("...")) ----
    std::set<std::string> AttrsAt(Out geo) const
    {
        std::set<std::string> a;
        std::set<uint32_t> seen;
        std::function<void(Out)> walk = [&](Out o) {
            const Node* n = g_.Find(o.first);
            if (!n || seen.count(n->id)) return;
            seen.insert(n->id);
            if (n->type == "store" && !n->text.empty()) a.insert(n->text);
            if (n->type == "grid") a.insert({"ix", "iy", "iz", "u", "v", "w"});
            if (n->type == "circle") a.insert({"angle", "t", "normal"});
            if (n->type == "line") a.insert({"t", "normal"});
            const char* in = n->type == "each" ? (o.second == 1 ? "geometry" : "body") : "geometry";
            if (const InputSlot* s = n->In(in))
                if (s->link.from) walk({s->link.from, s->link.output});
        };
        walk(geo);
        return a;
    }

    // ---- expressions ----
    std::string Arg(const Node&, const InputSlot& s, Out ctx, const std::string& lambda)
    {
        if (!s.link.from) return ConstText(s.value);
        return Val({s.link.from, s.link.output}, ctx, lambda);
    }

    std::string Val(Out o, Out ctx, const std::string& lambda)
    {
        const Node* n = g_.Find(o.first);
        if (!n) return "0";
        if (IsMacro(*n)) return std::string(n->type == "macro" ? "m" : "i") + std::to_string(n->setting);
        auto nm = name_.find(o);
        if (nm != name_.end()) return nm->second;
        if (Inlined(*n)) return InlineText(*n, o.second, ctx, lambda);
        return n->key;
    }

    std::string MacroAnnotated(const InputSlot& s)
    {
        const Node* n = s.link.from ? g_.Find(s.link.from) : nullptr;
        if (!n || !IsMacro(*n)) return "";
        const bool isInt = n->type == "int_macro";
        const int k = n->setting - 1;
        if (k < 0 || k >= 32) return "";
        const MacroSetting& m = isInt ? g_.intMacros[k] : g_.macros[k];
        return m.name.empty() ? "" : " /*" + m.name + "*/";
    }

    // geometry inputs positional, then the other inputs that are wired or not at their default, by name
    std::string Args(const Node& n, Out ctxOfFields, bool skipValueOfAt = false)
    {
        const NodeDef* d = DefFor(g_, n.type);
        std::vector<std::string> parts;
        if (d) {
            for (const InputDef& in : d->inputs) {
                if (in.type != Type::Geometry) continue;
                const InputSlot* s = n.In(in.name);
                if (s && s->link.from) parts.push_back(GeoName({s->link.from, s->link.output}));
            }
        }
        if (d && d->text == "name") parts.push_back("\"" + n.text + "\"");
        for (const InputSlot& s : n.inputs) {
            const int k = d ? d->Input(s.name) : -1;
            if (k >= 0 && d->inputs[k].type == Type::Geometry) continue;
            if (skipValueOfAt && s.name == "value") continue;
            if (IsDefault(d, s)) continue;
            Out ctx = ctxOfFields;
            if (k >= 0 && d->inputs[k].context == "make") ctx = {0, 0};
            parts.push_back(s.name + ": " + Arg(n, s, ctx, "") + MacroAnnotated(s));
        }
        if (n.type == "random" && n.salt) {
            char b[40];
            std::snprintf(b, sizeof(b), "salt: 0x%llx", (unsigned long long)n.salt);
            parts.push_back(b);
        }
        if (n.type == "reduce" && n.setting) parts.push_back(std::string("op: \"") + (n.setting == 1 ? "min" : n.setting == 2 ? "max" : "mean") + "\"");
        std::string s;
        for (size_t k = 0; k < parts.size(); k++) s += (k ? ", " : "") + parts[k];
        return s;
    }

    Out FieldCtx(const Node& n) const
    {
        const InputSlot* s = n.In("geometry");
        return s && s->link.from ? Out{s->link.from, s->link.output} : Out{0, 0};
    }

    std::string InlineText(const Node& n, int output, Out ctx, const std::string& lambda)
    {
        const std::string& t = n.type;
        if (IsReaderType(t)) {
            const bool field = !(t == "count" || t == "time" || t == "seed");
            return (!lambda.empty() && field ? lambda + "." : "") + t;
        }
        if (t == "attr") {
            if (!lambda.empty()) return lambda + "." + n.text;
            if (ctx.first && AttrsAt(ctx).count(n.text)) return n.text;
            return "attr(\"" + n.text + "\")";
        }
        if (t == "expr") {
            std::unique_ptr<Expr> e;
            Diag d;
            if (!ParseExpr(n.text, &e, &d)) return "0";
            Substitute(*e, n, ctx, lambda);
            return PrintExpr(*e);
        }
        if (IsMathFamily(t)) {
            const MathOpDef* m = MathOpOf(n);
            if (!m) return "0";
            std::string s = m->CodeName() + "(";
            std::string extra;
            for (int k = 0; k < (int)n.inputs.size(); k++) {
                const InputSlot& in = n.inputs[k];
                const bool active = LinkActive(g_, n, k);
                if (k < m->arity) {
                    InputSlot shown = in;
                    if (!active) shown.link = {};
                    s += (k ? ", " : "") + Arg(n, shown, ctx, lambda);
                    if (!active) extra += ", _" + in.name + ": " + Arg(n, in, ctx, lambda);
                } else if (in.link.from || !(in.value.type == Type::Float && in.value.f == 0)) {
                    // a hidden input keeps its wire and value (design v5 6.2, 13)
                    extra += ", _" + in.name + ": " + Arg(n, in, ctx, lambda);
                }
            }
            if (m->arity == 0 && !extra.empty()) extra = extra.substr(2);
            return s + extra + ")";
        }
        if (t == "random") {
            char b[48];
            std::snprintf(b, sizeof(b), "random(salt: 0x%llx)", (unsigned long long)n.salt);
            return n.salt ? b : "random()";
        }
        if (t == "at") {
            const InputSlot* v = n.In("value");
            const Out h = FieldCtx(n);
            const Node* vn = v && v->link.from ? g_.Find(v->link.from) : nullptr;
            const InputSlot* idx = n.In("index");
            const std::string i = idx ? Arg(n, *idx, ctx, lambda) : "0";
            if (vn && Inlined(*vn) && (IsReaderType(vn->type) || (vn->type == "attr" && AttrsAt(h).count(vn->text))))
                return "at(" + GeoName(h) + ", " + i + ")." + (vn->type == "attr" ? vn->text : vn->type);
            return "at(" + GeoName(h) + ", " + i + ", value: " + (v ? Arg(n, *v, h, "") : "0") + ")";
        }
        if (t == "accumulate") {
            const InputSlot* v = n.In("value");
            return "accumulate(" + (v ? Arg(n, *v, ctx, lambda) : std::string("0")) + ")";
        }
        if (t == "bounds" || t == "reduce" || t == "layer_set") {
            const NodeDef* d = DefFor(g_, t);
            std::string s = t + "(" + Args(n, FieldCtx(n)) + ")";
            if (t == "bounds" && d && output < (int)d->outputs.size()) s += "." + d->outputs[output].name;
            return s;
        }
        if (t == "capture") return n.key;
        return n.key;
    }

    void Substitute(Expr& e, const Node& n, Out ctx, const std::string& lambda)
    {
        if (e.kind == Expr::Name) {
            if (const InputSlot* s = n.In(e.text)) {
                const std::string text = Arg(n, *s, ctx, lambda);
                std::unique_ptr<Expr> r;
                Diag d;
                if (ParseExpr(text, &r, &d)) e = std::move(*r);
                return;
            }
            if (!lambda.empty() && IsField(e.text, ctx)) {
                auto base = std::make_unique<Expr>();
                base->kind = Expr::Name, base->text = lambda;
                e.kind = Expr::Member;
                e.args.clear();
                e.args.push_back(std::move(base));
            }
            return;
        }
        if (e.kind == Expr::Member && e.args[0]->kind == Expr::Name && e.args[0]->text == "parent") return;
        if (e.kind == Expr::Call && e.text == "attr" && e.args.size() == 1 && e.args[0]->kind == Expr::Str) {
            const std::string name = e.args[0]->text;
            if (!lambda.empty()) {
                auto base = std::make_unique<Expr>();
                base->kind = Expr::Name, base->text = lambda;
                e.kind = Expr::Member, e.text = name;
                e.args.clear();
                e.args.push_back(std::move(base));
            } else if (ctx.first && AttrsAt(ctx).count(name)) {
                e.kind = Expr::Name, e.text = name;
                e.args.clear();
            }
            return;
        }
        for (auto& a : e.args) Substitute(*a, n, ctx, lambda);
        for (auto& a : e.named) Substitute(*a.second, n, ctx, lambda);
    }

    bool IsField(const std::string& name, Out) const
    {
        if (name == "true" || name == "false" || name == "count" || name == "time" || name == "seed") return false;
        if (name.size() >= 2 && (name[0] == 'm' || name[0] == 'i') && std::isdigit((unsigned char)name[1])) return false;
        return true;
    }

    // a formula over elements (written as (e) => ... in a variable)
    bool FieldNode(const Node& n) const
    {
        const std::string& t = n.type;
        if (IsReaderType(t)) return !(t == "count" || t == "time" || t == "seed");
        if (t == "attr" || t == "random" || t == "at" || t == "accumulate") return true;
        if (IsMacro(n) || t == "bounds" || t == "reduce" || t == "layer_set") return false;
        if (t == "expr") {
            std::unique_ptr<Expr> e;
            Diag d;
            if (!ParseExpr(n.text, &e, &d)) return false;
            bool f = false;
            std::function<void(const Expr&)> walk = [&](const Expr& x) {
                if (x.kind == Expr::Name) {
                    if (const InputSlot* s = n.In(x.text)) {
                        if (s->link.from && g_.Find(s->link.from)) f |= FieldNode(*g_.Find(s->link.from)) || g_.Find(s->link.from)->type == "capture";
                    } else if (IsField(x.text, {0, 0})) {
                        f = true;
                    }
                }
                if (x.kind == Expr::Member && x.args[0]->kind == Expr::Name && x.args[0]->text == "parent") f = true;
                if (x.kind == Expr::Call && (x.text == "attr" || x.text == "random")) f = true;
                for (const auto& a : x.args) walk(*a);
            };
            walk(*e);
            return f;
        }
        if (IsMathFamily(t)) {
            for (int k = 0; k < (int)n.inputs.size(); k++) {
                const InputSlot& s = n.inputs[k];
                if (s.link.from && LinkActive(g_, n, k) && g_.Find(s.link.from)) {
                    const Node& m = *g_.Find(s.link.from);
                    if (FieldNode(m) || m.type == "capture") return true;
                }
            }
            return false;
        }
        return false;
    }

    std::string GeoName(Out o)
    {
        auto it = name_.find(o);
        if (it != name_.end()) return it->second;
        const Node* n = g_.Find(o.first);
        return n ? n->key : "?";
    }

    // ---- statements ----
    std::vector<uint32_t> Order()
    {
        // top-level statements: geometry nodes, values not written inline (not macros)
        std::vector<uint32_t> todo;
        for (const Node& n : g_.nodes) {
            if (n.type == "param" || n.type == "param_geo") name_[{n.id, 0}] = n.text;
            if (!n.zone && !IsMacro(n) && !Inlined(n) && !IsParam(n)) todo.push_back(n.id);
        }
        // dependencies through inlined nodes and zones
        std::map<uint32_t, std::set<uint32_t>> deps;
        std::set<uint32_t> stmt(todo.begin(), todo.end());
        std::function<void(uint32_t, std::set<uint32_t>&, std::set<uint32_t>&)> collect = [&](uint32_t id, std::set<uint32_t>& out,
                                                                                              std::set<uint32_t>& seen) {
            if (seen.count(id)) return;
            seen.insert(id);
            const Node* n = g_.Find(id);
            if (!n) return;
            for (const InputSlot& s : n->inputs) {
                if (!s.link.from) continue;
                const Node* m = g_.Find(s.link.from);
                if (!m) continue;
                if (stmt.count(m->id)) out.insert(m->id);
                else if (!IsMacro(*m)) collect(m->id, out, seen);
            }
            for (const Node& z : g_.nodes)
                if (z.zone == id) collect(z.id, out, seen);
        };
        for (uint32_t id : todo) {
            std::set<uint32_t> d, seen;
            collect(id, d, seen);
            d.erase(id);
            deps[id] = d;
        }
        std::vector<uint32_t> order;
        std::set<uint32_t> done;
        while (order.size() < todo.size()) {
            uint32_t best = 0;
            for (uint32_t id : todo) {
                if (done.count(id)) continue;
                bool ready = true;
                for (uint32_t d : deps[id]) ready &= done.count(d) > 0;
                if (!ready) continue;
                const Node* a = g_.Find(id);
                const Node* b = best ? g_.Find(best) : nullptr;
                // the output last; otherwise top to bottom, left to right, then by id
                auto rank = [](const Node* x) { return std::make_tuple(x->type == "output", x->y, x->x, x->id); };
                if (!b || rank(a) < rank(b)) best = id;
            }
            if (!best) {  // a loop: write the rest in any order
                for (uint32_t id : todo)
                    if (!done.count(id)) order.push_back(id), done.insert(id);
                break;
            }
            order.push_back(best);
            done.insert(best);
        }
        return order;
    }

    std::string Statement(const Node& n)
    {
        const std::string& t = n.type;
        if (t == "output") return "output(" + Args(n, FieldCtx(n)) + ")\n";
        if (t == "each") return Each(n);
        if (t == "capture") {
            const InputSlot* gi = n.In("geometry");
            const InputSlot* v = n.In("value");
            const Out in = gi && gi->link.from ? Out{gi->link.from, gi->link.output} : Out{0, 0};
            const std::string gname = GeoName(in);
            name_[{n.id, 1}] = n.key;
            if (in.first && uses_[in] == 1) {
                name_[{n.id, 0}] = gname;
                const Node* vn = v && v->link.from ? g_.Find(v->link.from) : nullptr;
                if (vn && Inlined(*vn) && (IsReaderType(vn->type) || (vn->type == "attr" && AttrsAt(in).count(vn->text))))
                    return n.key + " = " + gname + "." + (vn->type == "attr" ? vn->text : vn->type) + "\n";
                return n.key + " = " + gname + ".(" + (v ? Arg(n, *v, in, "") : "0") + ")\n";
            }
            name_[{n.id, 0}] = n.key + "_g";
            return n.key + "_g, " + n.key + " = capture(" + gname + ", value: " + (v ? Arg(n, *v, in, "") : "0") + ")\n";
        }
        if (IsGeoNode(g_, n)) {
            name_[{n.id, 0}] = n.key;
            if (const UserGroup* u = FindUserGroup(g_, n.type))
                if (u->def.outputs.size() > 1)
                    for (size_t k = 0; k < u->def.outputs.size(); k++) name_[{n.id, (int)k}] = n.key + "." + u->def.outputs[k].name;
            return n.key + " = " + n.type + "(" + Args(n, FieldCtx(n)) + ")\n";
        }
        if (Block(n)) {
            const std::string name = Ident(n.key) ? n.key : IdentOf(n.key);
            const std::vector<OutputDef> outs = OutputsOf(g_, n);
            for (size_t k = 0; k < outs.size(); k++)
                name_[{n.id, (int)k}] = outs.size() == 1 && outs[0].name == "value" ? name : name + "." + outs[k].name;
            std::string args;
            for (const InputSlot& s : n.inputs) args += (args.empty() ? "" : ", ") + s.name + ": " + Arg(n, s, {0, 0}, "");
            std::vector<FormulaLine> lines;
            Diag d;
            std::string body;
            if (ParseFormula(n.text, &lines, &d))
                for (const FormulaLine& l : lines) body += "  " + l.name + " = " + PrintExpr(*l.expr) + "\n";
            else
                body = "  // " + n.text + "\n";
            return name + " = expr" + (args.empty() ? std::string("") : "(" + args + ")") + " {\n" + body + "}\n";
        }
        // a value statement
        const NodeDef* d = DefFor(g_, t);
        // a value node's key may be written as an identifier of its own (design v5 11: its key is not its ids')
        const std::string nm = Ident(n.key) ? n.key : IdentOf(n.key);
        for (size_t k = 0; d && k < d->outputs.size(); k++) name_[{n.id, (int)k}] = nm;
        // the output the variable names (bounds(...).center): the one used
        int used = 0;
        for (const auto& kv : uses_)
            if (kv.first.first == n.id) used = kv.first.second;
        const bool formula = t == "expr" || IsReaderType(t) || t == "attr" || IsMathFamily(t);
        if (formula && FieldNode(n)) return nm + " = (e) => " + InlineText(n, used, {0, 0}, "e") + "\n";
        return nm + " = " + InlineText(n, used, {0, 0}, "") + "\n";
    }

    // ---- each ----
    std::string Each(const Node& z)
    {
        const InputSlot* in = z.In("geometry");
        const Out input = in && in->link.from ? Out{in->link.from, in->link.output} : Out{0, 0};
        name_[{z.id, 0}] = z.key;
        // the chain inside, first to last
        std::vector<const Node*> chain;
        const InputSlot* body = z.In("body");
        Out o = body && body->link.from ? Out{body->link.from, body->link.output} : Out{0, 0};
        std::set<uint32_t> seen;
        while (o.first && !(o.first == z.id && o.second == 1) && !seen.count(o.first)) {
            seen.insert(o.first);
            const Node* n = g_.Find(o.first);
            if (!n) break;
            chain.push_back(n);
            const InputSlot* gi = n->In("geometry");
            o = gi && gi->link.from ? Out{gi->link.from, gi->link.output} : Out{0, 0};
        }
        std::reverse(chain.begin(), chain.end());
        std::string s = z.key + " = each " + GeoName(input) + " {\n";
        Out cur{z.id, 1};
        for (size_t k = 0; k < chain.size(); k++) {
            const Node& n = *chain[k];
            // if: a captured condition whose value only feeds the wheres of the sets that follow (or their not)
            if (n.type == "capture" && n.text.empty()) {
                const InputSlot* v = n.In("value");
                const std::string cond = v ? Arg(n, *v, cur, "") : "false";
                name_[{n.id, 1}] = "?";
                std::string thenPart, elsePart;
                size_t j = k + 1;
                Out c2{n.id, 0};
                for (; j < chain.size(); j++) {
                    const Node& m = *chain[j];
                    const InputSlot* w = m.In("where");
                    if (!w || !w->link.from) break;
                    const Node* wn = g_.Find(w->link.from);
                    const bool then = w->link.from == n.id && w->link.output == 1;
                    const bool other = wn && wn->type == "boolmath" && wn->text == "not" && wn->In("a") && wn->In("a")->link.from == n.id;
                    if (!then && !other) break;
                    (then ? thenPart : elsePart) += "    " + InnerLine(m, c2);
                    c2 = {m.id, 0};
                }
                s += "  if " + cond + " {\n" + thenPart + "  }";
                if (!elsePart.empty()) s += " else {\n" + elsePart + "  }";
                s += "\n";
                k = j - 1;
                cur = c2;
                continue;
            }
            s += "  " + InnerLine(n, cur);
            cur = {n.id, 0};
        }
        return s + "}\n";
    }

    std::string InnerLine(const Node& n, Out ctx)
    {
        const std::string& t = n.type;
        if (t == "capture") {
            const std::string local = n.text.empty() ? n.key : n.text;
            name_[{n.id, 1}] = local;
            const InputSlot* v = n.In("value");
            return local + " = " + (v ? Arg(n, *v, ctx, "") : "0") + "\n";
        }
        if (t == "store") {
            const InputSlot* v = n.In("value");
            return "store(\"" + n.text + "\", " + (v ? Arg(n, *v, ctx, "") : "0") + ")\n";
        }
        if (t.rfind("set_", 0) == 0) {
            const std::string attr = t.substr(4);
            const InputSlot* v = n.In(attr);
            return attr + " = " + (v ? Arg(n, *v, ctx, "") : "0") + "\n";
        }
        return "// " + t + " cannot be written inside each\n";
    }
};

// Folds an Expression node into the Expression that is its only user (they then read as one formula).
Graph Normalized(const Graph& in)
{
    Graph g = in;
    for (bool changed = true; changed;) {
        changed = false;
        for (Node& user : g.nodes) {
            if (user.type != "expr") continue;
            for (size_t k = 0; k < user.inputs.size(); k++) {
                InputSlot s = user.inputs[k];
                const Node* src = s.link.from ? g.Find(s.link.from) : nullptr;
                if (!src || src->type != "expr") continue;
                int uses = 0;
                for (const Node& n : g.nodes)
                    for (const InputSlot& x : n.inputs) uses += x.link.from == src->id;
                if (uses != 1) continue;
                std::unique_ptr<Expr> outer, inner;
                Diag d;
                if (!ParseExpr(user.text, &outer, &d) || !ParseExpr(src->text, &inner, &d)) continue;
                const Node srcCopy = *src;
                std::function<void(Expr&)> sub = [&](Expr& e) {
                    if (e.kind == Expr::Name && e.text == s.name) {
                        std::unique_ptr<Expr> c;
                        ParseExpr(srcCopy.text, &c, &d);
                        e = std::move(*c);
                        return;
                    }
                    for (auto& a : e.args) sub(*a);
                };
                sub(*outer);
                user.text = PrintExpr(*outer);
                user.inputs.erase(user.inputs.begin() + (long)k);
                for (const InputSlot& x : srcCopy.inputs) user.inputs.push_back(x);
                g.nodes.erase(std::remove_if(g.nodes.begin(), g.nodes.end(), [&](const Node& n) { return n.id == srcCopy.id; }), g.nodes.end());
                changed = true;
                break;
            }
            if (changed) break;
        }
    }
    // an input wired from a Macro or a Position-like node is that name in the formula (the writer writes m1, the
    // reader reads it in place: design v5 13)
    auto uses = [&](uint32_t id) {
        int u = 0;
        for (const Node& n : g.nodes)
            for (const InputSlot& x : n.inputs) u += x.link.from == id;
        return u;
    };
    auto rename = [](Node& n, const std::map<std::string, std::string>& to) {
        std::vector<FormulaLine> lines;
        Diag d;
        if (!ParseFormula(n.text, &lines, &d)) return;
        std::string text;
        for (const FormulaLine& l : lines) {
            std::unique_ptr<Expr> c;
            ParseExpr(PrintExpr(*l.expr), &c, &d);
            std::function<void(Expr&)> sub = [&](Expr& e) {
                if (e.kind == Expr::Name) {
                    auto it = to.find(e.text);
                    if (it != to.end()) e.text = it->second;
                }
                if (e.kind == Expr::Member && e.args[0]->kind == Expr::Name && e.args[0]->text == "parent") return;
                for (auto& a : e.args) sub(*a);
                for (auto& a : e.named) sub(*a.second);
            };
            if (c) sub(*c);
            if (!text.empty()) text += "\n";
            const std::string body = c ? PrintExpr(*c) : PrintExpr(*l.expr);
            text += lines.size() == 1 && l.name == "value" ? body : l.name + " = " + body;
        }
        n.text = text;
    };
    for (Node& n : g.nodes) {
        if (n.type != "expr") continue;
        std::map<std::string, std::string> to;
        for (size_t k = 0; k < n.inputs.size();) {
            const InputSlot& in = n.inputs[k];
            const Node* src = in.link.from ? g.Find(in.link.from) : nullptr;
            std::string name;
            if (src && (src->type == "macro" || src->type == "int_macro")) name = std::string(src->type == "macro" ? "m" : "i") + std::to_string(src->setting);
            else if (src && IsReaderType(src->type) && in.link.output == 0) name = src->type;
            if (name.empty()) {
                k++;
                continue;
            }
            to[in.name] = name;
            n.inputs.erase(n.inputs.begin() + (long)k);
        }
        if (!to.empty()) rename(n, to);
    }
    for (bool again = true; again;) {
        again = false;
        for (size_t k = 0; k < g.nodes.size(); k++) {
            const Node& n = g.nodes[k];
            if ((n.type == "macro" || n.type == "int_macro" || IsReaderType(n.type)) && !uses(n.id)) {
                g.nodes.erase(g.nodes.begin() + (long)k);
                again = true;
                break;
            }
        }
    }
    // an Expression's inputs are named by where they first appear (their names are not compared)
    for (Node& n : g.nodes) {
        if (n.type != "expr") continue;
        const FormulaSockets fs = ReadFormula(n.text);
        std::map<std::string, std::string> to;
        int k = 0;
        for (const std::string& name : fs.inputs) to[name] = "_i" + std::to_string(++k);
        for (InputSlot& in : n.inputs) {
            auto it = to.find(in.name);
            if (it != to.end()) in.name = it->second;
        }
        rename(n, to);
    }
    // value nodes are named by where they are used (their keys are not compared: design v5 13)
    std::map<uint32_t, std::string> canon;
    auto valueNode = [&](const Node& n) {
        return !IsGeoNode(g, n) && n.type != "output" && n.type != "each" && n.type != "group_output" && n.type != "param" &&
               n.type != "param_geo";
    };
    for (const Node& n : g.nodes)
        if (!valueNode(n)) canon[n.id] = n.key;
    for (size_t round = 0; round <= g.nodes.size(); round++) {
        bool changed = false;
        for (const Node& user : g.nodes) {
            auto cu = canon.find(user.id);
            if (cu == canon.end()) continue;
            for (const InputSlot& in : user.inputs) {
                const Node* src = in.link.from ? g.Find(in.link.from) : nullptr;
                if (!src || !valueNode(*src)) continue;
                const std::string c = cu->second + "." + in.name;
                auto have = canon.find(src->id);
                if (have == canon.end() || c < have->second) {
                    canon[src->id] = c;
                    changed = true;
                }
            }
        }
        if (!changed) break;
    }
    for (Node& n : g.nodes) {
        auto c = canon.find(n.id);
        n.key = c != canon.end() ? c->second : "~" + n.key;
    }
    // formulas in one written form
    for (Node& n : g.nodes) {
        if (n.type != "expr") continue;
        rename(n, {});
    }
    return g;
}

}  // namespace

std::string GraphToCode(const Graph& g)
{
    Printer p(g);
    return p.Code();
}

std::string ExprText(const Graph& g, uint32_t node, int output)
{
    Printer p(g);
    return p.Inline(node, output, {0, 0});
}

bool SameGraph(const Graph& a0, const Graph& b0, std::string* why)
{
    const Graph a = Normalized(a0), b = Normalized(b0);
    auto fail = [&](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    // the user's groups: the same names, inputs, outputs and bodies
    if (a0.groups.size() != b0.groups.size()) return fail("group count");
    for (size_t k = 0; k < a0.groups.size(); k++) {
        const UserGroup &x = a0.groups[k], &y = b0.groups[k];
        if (x.name != y.name) return fail("group " + x.name + " vs " + y.name);
        if (x.def.inputs.size() != y.def.inputs.size() || x.def.outputs.size() != y.def.outputs.size()) return fail("group " + x.name + ": its inputs or outputs");
        for (size_t i = 0; i < x.def.inputs.size(); i++) {
            const InputDef &p = x.def.inputs[i], &q = y.def.inputs[i];
            if (p.name != q.name || p.type != q.type || p.structural != q.structural || !ValueEqual(p.dflt, q.dflt))
                return fail("group " + x.name + ": input " + p.name);
        }
        for (size_t i = 0; i < x.def.outputs.size(); i++)
            if (x.def.outputs[i].name != y.def.outputs[i].name) return fail("group " + x.name + ": output " + x.def.outputs[i].name);
        std::string w;
        if (!x.body || !y.body || !SameGraph(*x.body, *y.body, &w)) return fail("group " + x.name + ": " + w);
    }
    if (a.nodes.size() != b.nodes.size())
        return fail("node count " + std::to_string(a.nodes.size()) + " vs " + std::to_string(b.nodes.size()));
    auto keyOf = [](const Graph& g, uint32_t id) {
        const Node* n = g.Find(id);
        return n ? n->key : std::string();
    };
    for (const Node& x : a.nodes) {
        const Node* y = b.FindKey(x.key);
        if (!y) return fail("no node " + x.key);
        if (x.type != y->type) return fail(x.key + ": type " + x.type + " vs " + y->type);
        if (x.text != y->text) return fail(x.key + ": text '" + x.text + "' vs '" + y->text + "'");
        if (x.setting != y->setting || x.salt != y->salt) return fail(x.key + ": setting or salt");
        if (keyOf(a, x.zone) != keyOf(b, y->zone)) return fail(x.key + ": zone");
        for (const InputSlot& s : x.inputs) {
            const InputSlot* t = y->In(s.name);
            if (!t) return fail(x.key + ": no input " + s.name);
            if (s.link.from || t->link.from) {
                if (keyOf(a, s.link.from) != keyOf(b, t->link.from) || s.link.output != t->link.output)
                    return fail(x.key + "." + s.name + ": wired from " + keyOf(a, s.link.from) + " vs " + keyOf(b, t->link.from));
            } else if (!ValueEqual(s.value, t->value)) {
                return fail(x.key + "." + s.name + ": " + ConstText(s.value) + " vs " + ConstText(t->value));
            }
        }
        if (x.inputs.size() != y->inputs.size()) return fail(x.key + ": input count");
    }
    return true;
}

}  // namespace dx5
