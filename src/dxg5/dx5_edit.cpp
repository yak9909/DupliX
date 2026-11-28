// DupliX 2 (design v5, 8.3): the editor's operations, the presets and the test scripts.
#include "dxg5/edit.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <map>
#include <set>

#include "dxg5/lang.h"

namespace dx5 {

namespace {

bool Num(const std::string& s, double* v)
{
    auto r = std::from_chars(s.data(), s.data() + s.size(), *v);
    return r.ec == std::errc() && r.ptr == s.data() + s.size() && std::isfinite(*v);
}

bool IsReaderName(const std::string& t)
{
    static const char* r[] = {"position", "rotation", "scale", "opacity", "source", "visible", "weight", "q", "s",
                              "index",    "id",       "count", "time",    "seed"};
    for (const char* x : r)
        if (t == x) return true;
    return false;
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

const InputDef* DefOf(const Graph& g, uint32_t id, const std::string& input)
{
    const Node* n = g.Find(id);
    const NodeDef* d = n ? FindDef(n->type) : nullptr;
    const int k = d ? d->Input(input) : -1;
    return k >= 0 ? &d->inputs[k] : nullptr;
}

std::string Label(const std::string& input)
{
    static const std::map<std::string, std::string> l = {
        {"cx", "Count X"},           {"cy", "Count Y"},           {"cz", "Count Z"},           {"sx", "Spacing X"},
        {"sy", "Spacing Y"},         {"sz", "Spacing Z"},         {"rotation_x", "Rotation X"}, {"rotation_y", "Rotation Y"},
        {"rotation_z", "Rotation Z"}, {"copy_scale", "Copy Scale"}, {"start_angle", "Start Angle"}};
    auto it = l.find(input);
    if (it != l.end()) return it->second;
    std::string s;
    bool up = true;
    for (char c : input) {
        if (c == '_') {
            s += ' ', up = true;
            continue;
        }
        s += up && c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
        up = false;
    }
    return s;
}

uint32_t MacroNode(Graph& g, bool isInt, int k, double nearX, double nearY)
{
    const std::string key = std::string(isInt ? "i" : "m") + std::to_string(k + 1);
    if (const Node* n = g.FindKey(key))
        if (n->type == (isInt ? "int_macro" : "macro")) return n->id;
    const uint32_t id = AddNode(g, isInt ? "int_macro" : "macro", key, nearX - 160, nearY);
    g.Find(id)->setting = k + 1;
    return id;
}

std::set<std::string> NamesIn(const std::string& text)
{
    std::set<std::string> s;
    std::unique_ptr<Expr> e;
    Diag d;
    if (!ParseExpr(text, &e, &d)) return s;
    std::function<void(const Expr&)> walk = [&](const Expr& x) {
        if (x.kind == Expr::Name) s.insert(x.text);
        for (const auto& a : x.args) walk(*a);
        for (const auto& a : x.named) walk(*a.second);
    };
    walk(*e);
    return s;
}

bool Feeds(const Graph& g, uint32_t from, uint32_t to)
{
    // does `from` feed `to` already (walking up from `to`)?
    std::vector<uint32_t> todo = {to};
    std::set<uint32_t> seen;
    while (!todo.empty()) {
        const uint32_t n = todo.back();
        todo.pop_back();
        if (n == from) return true;
        if (!seen.insert(n).second) continue;
        if (const Node* x = g.Find(n)) {
            for (const InputSlot& s : x->inputs)
                if (s.link.from) todo.push_back(s.link.from);
            if (x->type == "each")
                for (const Node& z : g.nodes)
                    if (z.zone == n) todo.push_back(z.id);
        }
    }
    return false;
}

std::string V1Input(const Node& n, const std::string& key)
{
    static const std::map<std::string, std::string> m = {
        {"countX", "cx"},       {"countY", "cy"},          {"countZ", "cz"},          {"spacingX", "sx"},
        {"spacingY", "sy"},     {"spacingZ", "sz"},        {"startAngle", "start_angle"}, {"rotX", "rotation_x"},
        {"rotY", "rotation_y"}, {"rotZ", "rotation_z"},    {"copyScale", "copy_scale"}};
    if (n.type == "source_layers" && key == "count") return "layers";
    auto it = m.find(key);
    return it == m.end() ? key : it->second;
}

}  // namespace

bool IsField(const Graph& g, uint32_t node, int output)
{
    const Node* n = g.Find(node);
    if (!n) return false;
    const std::string& t = n->type;
    if (t == "count" || t == "time" || t == "seed" || t == "macro" || t == "int_macro" || t == "bounds" || t == "reduce" ||
        t == "layer_set")
        return false;
    if (IsReaderName(t) || t == "attr" || t == "random" || t == "at" || t == "accumulate") return true;
    if (t == "capture") return output == 1;
    if (t == "param") return true;
    if (t == "expr") {
        for (const std::string& s : NamesIn(n->text)) {
            if (const InputSlot* in = n->In(s)) {
                if (in->link.from && IsField(g, in->link.from, in->link.output)) return true;
                continue;
            }
            bool isInt = false;
            int k = 0;
            if (s == "true" || s == "false" || s == "count" || s == "time" || s == "seed" || MacroName(s, &isInt, &k)) continue;
            return true;  // an attribute, index, id, parent.*
        }
        return false;
    }
    if (t.rfind("math_", 0) == 0) {
        for (const InputSlot& s : n->inputs)
            if (s.link.from && IsField(g, s.link.from, s.link.output)) return true;
        return false;
    }
    return false;
}

uint32_t AddNodeAt(Graph& g, const std::string& type, double x, double y)
{
    if (!FindDef(type)) return 0;
    if (type == "output" && g.OutputNode()) return 0;
    const uint32_t id = AddNode(g, type, "", x, y);
    if (type == "random") g.Find(id)->salt = (HashStr(g.Find(id)->key) ^ (uint64_t)g.revision * 0x9E3779B97F4A7C15ull) & 0xffffffull;
    return id;
}

bool RemoveNode(Graph& g, uint32_t id)
{
    if (!g.Find(id)) return false;
    std::set<uint32_t> gone = {id};
    for (const Node& n : g.nodes)
        if (n.zone == id) gone.insert(n.id);
    g.nodes.erase(std::remove_if(g.nodes.begin(), g.nodes.end(), [&](const Node& n) { return gone.count(n.id) > 0; }), g.nodes.end());
    for (Node& n : g.nodes)
        for (InputSlot& s : n.inputs)
            if (gone.count(s.link.from)) s.link = {};
    return true;
}

bool MoveNode(Graph& g, uint32_t id, double x, double y)
{
    Node* n = g.Find(id);
    if (!n) return false;
    n->x = x, n->y = y;
    return true;
}

bool Connect(Graph& g, uint32_t from, int output, uint32_t to, const std::string& input, std::string* why)
{
    auto no = [&](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    const Node* a = g.Find(from);
    Node* b = g.Find(to);
    if (!a || !b || from == to) return no("no such node");
    const NodeDef* da = FindDef(a->type);
    const InputDef* in = DefOf(g, to, input);
    InputSlot* slot = b->In(input);
    if (!slot) return no(b->key + " has no input " + input);
    if (!da || output < 0 || output >= (int)da->outputs.size()) return no(a->key + " has no output " + std::to_string(output));
    const Type ot = da->outputs[output].type;
    const Type it = in ? in->type : Type::Any;
    if ((ot == Type::Geometry) != (it == Type::Geometry)) return no(std::string("type mismatch: ") + input + " wants " + TypeName(it));
    if (ot != Type::Geometry && it != Type::Any && ot != Type::Any && ot != it) {
        Value probe;
        const bool scalar = ot == Type::Float || ot == Type::Int || ot == Type::Bool;
        const bool ok = (it == Type::Float || it == Type::Int) ? scalar : it == Type::Vector ? (ot == Type::Float || ot == Type::Int) : false;
        if (!ok) return no(std::string("type mismatch: ") + input + " wants " + TypeName(it) + ", got " + TypeName(ot));
    }
    if (in && (in->structural || !in->field) && ot != Type::Geometry && IsField(g, from, output))
        return no(input + " cannot vary per element");
    if (Feeds(g, to, from)) return no("that would make a loop");
    slot->link = {from, output};
    return true;
}

bool Disconnect(Graph& g, uint32_t to, const std::string& input)
{
    Node* n = g.Find(to);
    InputSlot* s = n ? n->In(input) : nullptr;
    if (!s || !s->link.from) return false;
    s->link = {};
    return true;
}

bool SetConst(Graph& g, uint32_t id, const std::string& input, double v)
{
    Node* n = g.Find(id);
    InputSlot* s = n ? n->In(input) : nullptr;
    if (!s || !std::isfinite(v)) return false;
    const InputDef* d = DefOf(g, id, input);
    Value x = Value::F(v);
    if (d) {
        if (!d->choices.empty()) {
            const int64_t cnt = 1 + std::count(d->choices.begin(), d->choices.end(), '|');
            x = Value::I(std::clamp<int64_t>(RoundHalfAway(v), 0, cnt - 1));
        } else if (d->type == Type::Int || d->structural) {
            x = Value::I(RoundHalfAway(v));
        } else if (d->type == Type::Vector) {
            x = Value::V({v, v, v});
        } else if (d->type == Type::Bool) {
            x = Value::B(v != 0);
        }
    }
    s->value = x;
    s->link = {};
    return true;
}

bool SetExpression(Graph& g, uint32_t id, const std::string& input, const std::string& text, std::string* why)
{
    auto no = [&](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    Node* n = g.Find(id);
    if (!n || !n->In(input)) return no("no such input");
    std::unique_ptr<Expr> e;
    Diag d;
    if (!ParseExpr(text, &e, &d)) return no("col " + std::to_string(d.pos.col) + ": " + d.msg);
    Value v;
    if (EvalConst(*e, &v)) {
        const InputDef* def = DefOf(g, id, input);
        if (v.type == Type::Vector) {
            InputSlot* s = n->In(input);
            Value c;
            if (def && !Convert(v, def->type, &c)) return no(std::string("type mismatch: ") + input + " wants " + TypeName(def->type));
            s->value = def ? c : v;
            s->link = {};
            return true;
        }
        return SetConst(g, id, input, AsFloat(v)) || no("cannot set " + input);
    }
    // names: macros, attributes, index ...; node calls belong in the code view
    bool field = false;
    std::function<bool(const Expr&)> check = [&](const Expr& x) -> bool {
        if (x.kind == Expr::Call && !IsMathFunction(x.text)) return no(x.text + "(): use the code view for node calls");
        if (x.kind == Expr::Str || x.kind == Expr::Lambda || x.kind == Expr::MemberExpr) return no("not a formula");
        if (x.kind == Expr::Name) {
            bool isInt = false;
            int k = 0;
            const std::string& s = x.text;
            if (!(s == "true" || s == "false" || s == "count" || s == "time" || s == "seed" || MacroName(s, &isInt, &k))) field = true;
        }
        for (const auto& a : x.args)
            if (!check(*a)) return false;
        return true;
    };
    if (!check(*e)) return false;
    const InputDef* def = DefOf(g, id, input);
    if (field && def && (def->structural || !def->field)) return no(input + " cannot vary per element");
    const std::string key = UniqueKey(g, n->key + "." + input);
    const double x = n->x - 200, y = n->y;
    const uint32_t ex = AddNode(g, "expr", key, x, y);
    g.Find(ex)->text = PrintExpr(*e);
    Node* m = g.Find(id);
    InputSlot* s = m->In(input);
    // the Expression node it replaces goes (it fed only this input)
    const uint32_t old = s->link.from;
    s->link = {ex, 0};
    if (old) {
        const Node* o = g.Find(old);
        int uses = 0;
        for (const Node& q : g.nodes)
            for (const InputSlot& t : q.inputs) uses += t.link.from == old;
        if (o && o->type == "expr" && !uses) RemoveNode(g, old);
    }
    return true;
}

bool LinkMacro(Graph& g, uint32_t id, const std::string& input, bool isInt, int k)
{
    Node* n = g.Find(id);
    if (!n || !n->In(input) || k < 0 || k >= 32) return false;
    const InputDef* d = DefOf(g, id, input);
    if (d && (!d->choices.empty() || d->type == Type::Geometry)) return false;
    const double x = n->x, y = n->y;
    const uint32_t m = MacroNode(g, isInt, k, x, y);
    g.Find(id)->In(input)->link = {m, 0};
    return true;
}

uint32_t UsedMacros(const Graph& g)
{
    uint32_t m = 0;
    for (const Node& n : g.nodes) {
        if (n.type == "expr")
            for (const std::string& s : NamesIn(n.text)) {
                bool isInt = false;
                int k = 0;
                if (MacroName(s, &isInt, &k) && !isInt && !n.In(s)) m |= 1u << k;
            }
        for (const InputSlot& s : n.inputs) {
            const Node* f = s.link.from ? g.Find(s.link.from) : nullptr;
            if (f && f->type == "macro" && f->setting >= 1 && f->setting <= 32) m |= 1u << (f->setting - 1);
        }
    }
    return m;
}

uint32_t UsedIntMacros(const Graph& g)
{
    uint32_t m = 0;
    for (const Node& n : g.nodes) {
        if (n.type == "expr")
            for (const std::string& s : NamesIn(n.text)) {
                bool isInt = false;
                int k = 0;
                if (MacroName(s, &isInt, &k) && isInt && !n.In(s)) m |= 1u << k;
            }
        for (const InputSlot& s : n.inputs) {
            const Node* f = s.link.from ? g.Find(s.link.from) : nullptr;
            if (f && f->type == "int_macro" && f->setting >= 1 && f->setting <= 32) m |= 1u << (f->setting - 1);
        }
    }
    return m;
}

uint32_t UsedLayers(const Graph& g)
{
    uint32_t m = 0;
    for (const Node& n : g.nodes) {
        if (n.type == "layer_set") return (1u << kNumLayers) - 1;
        if (n.type != "source_layers") continue;
        const InputSlot* f = n.In("first");
        const InputSlot* c = n.In("layers");
        if (!f || !c) continue;
        if (f->link.from || c->link.from) return (1u << kNumLayers) - 1;
        const int64_t first = AsInt(f->value), count = AsInt(c->value);
        for (int64_t i = 0; i < count; i++) {
            const int64_t k = first - 1 + i;
            if (k >= 0 && k < kNumLayers) m |= 1u << k;
        }
    }
    return m;
}

int FreeMacro(const Graph& g, bool isInt)
{
    const uint32_t used = isInt ? UsedIntMacros(g) : UsedMacros(g);
    const MacroSetting* ms = isInt ? g.intMacros : g.macros;
    for (int k = 0; k < 32; k++) {
        if (used >> k & 1 || !ms[k].name.empty()) continue;
        // a Macro node with no wire still holds the number
        if (g.FindKey(std::string(isInt ? "i" : "m") + std::to_string(k + 1))) continue;
        return k;
    }
    return -1;
}

bool Expose(Graph& g, uint32_t id, const std::string& input, MacroWrite* write)
{
    Node* n = g.Find(id);
    InputSlot* s = n ? n->In(input) : nullptr;
    const InputDef* d = DefOf(g, id, input);
    if (!s || !d || s->link.from || !d->choices.empty() || d->type == Type::Geometry || d->type == Type::Vector ||
        d->type == Type::Bool || d->type == Type::Rotation)
        return false;
    const bool isInt = d->type == Type::Int || d->structural;
    const int k = FreeMacro(g, isInt);
    if (k < 0) return false;
    const double v = AsFloat(s->value);
    MacroSetting& m = isInt ? g.intMacros[k] : g.macros[k];
    m.name = Label(input);
    m.sliderMin = std::min(d->softMin, v), m.sliderMax = std::max(d->softMax, v);
    LinkMacro(g, id, input, isInt, k);
    if (write) *write = {isInt, k, v};
    return true;
}

bool RenameMacro(Graph& g, bool isInt, int k, const std::string& name)
{
    if (k < 0 || k >= 32) return false;
    (isInt ? g.intMacros : g.macros)[k].name = name;
    return true;
}

bool SetMacroRange(Graph& g, bool isInt, int k, double lo, double hi)
{
    if (k < 0 || k >= 32 || !(lo < hi)) return false;
    MacroSetting& m = (isInt ? g.intMacros : g.macros)[k];
    m.sliderMin = lo, m.sliderMax = hi;
    return true;
}

// ---- Expression <-> Math nodes ----
bool ExpandExpression(Graph& g, uint32_t exprNode)
{
    const Node* e0 = g.Find(exprNode);
    if (!e0 || e0->type != "expr") return false;
    const Node ex = *e0;
    std::unique_ptr<Expr> ast;
    Diag d;
    if (!ParseExpr(ex.text, &ast, &d)) return false;
    int made = 0;
    double row = 0;
    struct R {
        bool constant = false;
        Value v;
        Link link;
    };
    std::function<bool(const Expr&, int, R*)> build = [&](const Expr& x, int depth, R* out) -> bool {
        auto node = [&](const std::string& type) {
            const uint32_t id = AddNode(g, type, ex.key + "." + std::to_string(++made), ex.x - 180.0 * (depth + 1), ex.y + 60 * row++);
            return id;
        };
        auto setIn = [&](uint32_t id, const char* name, const R& r) {
            InputSlot* s = g.Find(id)->In(name);
            if (r.constant) s->value = r.v;
            else s->link = r.link;
        };
        if (x.kind == Expr::Num || (x.kind == Expr::Name && (x.text == "true" || x.text == "false"))) {
            out->constant = true;
            return EvalConst(x, &out->v);
        }
        if (x.kind == Expr::Name) {
            if (const InputSlot* s = ex.In(x.text)) {
                if (s->link.from) out->link = s->link;
                else out->constant = true, out->v = s->value;
                return true;
            }
            bool isInt = false;
            int k = 0;
            if (MacroName(x.text, &isInt, &k)) {
                out->link = {MacroNode(g, isInt, k, ex.x - 180.0 * (depth + 1), ex.y), 0};
                return true;
            }
            const uint32_t id = node(IsReaderName(x.text) ? x.text : "attr");
            if (!IsReaderName(x.text)) g.Find(id)->text = x.text;
            out->link = {id, 0};
            return true;
        }
        if (x.kind == Expr::Member) {
            if (x.args[0]->kind == Expr::Name && x.args[0]->text == "parent") {
                const uint32_t id = node("attr");
                g.Find(id)->text = "parent." + x.text;
                out->link = {id, 0};
                return true;
            }
            R a;
            if (!build(*x.args[0], depth + 1, &a)) return false;
            const uint32_t id = node("math_sep_" + x.text);
            setIn(id, "a", a);
            out->link = {id, 0};
            return true;
        }
        std::string op;
        if (x.kind == Expr::Unary || x.kind == Expr::Binary) op = OpName(x.text, x.kind == Expr::Unary);
        else if (x.kind == Expr::Cond) op = "switch";
        else if (x.kind == Expr::Call && IsMathFunction(x.text)) op = x.text;
        else return false;
        if (!FindDef("math_" + op)) return false;
        const uint32_t id = node("math_" + op);
        static const char* names[3] = {"a", "b", "c"};
        for (size_t k = 0; k < x.args.size() && k < 3; k++) {
            R a;
            if (!build(*x.args[k], depth + 1, &a)) return false;
            setIn(id, names[k], a);
        }
        out->link = {id, 0};
        return true;
    };
    R root;
    const Graph before = g;
    if (!build(*ast, 0, &root)) {
        g = before;
        return false;
    }
    for (Node& n : g.nodes)
        for (InputSlot& s : n.inputs)
            if (s.link.from == ex.id) {
                if (root.constant) s.link = {}, s.value = root.v;
                else s.link = root.link;
            }
    // the top Math node takes the Expression's key (and place)
    if (!root.constant) {
        Node* r = g.Find(root.link.from);
        if (r && r->key.rfind(ex.key + ".", 0) == 0) {
            RemoveNode(g, ex.id);
            r = g.Find(root.link.from);
            r->key = ex.key, r->x = ex.x, r->y = ex.y;
            return true;
        }
    }
    RemoveNode(g, ex.id);
    return true;
}

bool CollapseToExpression(Graph& g, const std::vector<uint32_t>& nodes)
{
    std::set<uint32_t> S(nodes.begin(), nodes.end());
    for (uint32_t id : S) {
        const Node* n = g.Find(id);
        if (!n) return false;
        const std::string& t = n->type;
        if (!(t.rfind("math_", 0) == 0 || t == "expr" || IsReaderName(t) || t == "attr")) return false;
    }
    // the one output used outside the set
    uint32_t exit = 0;
    for (const Node& n : g.nodes) {
        if (S.count(n.id)) continue;
        for (const InputSlot& s : n.inputs)
            if (s.link.from && S.count(s.link.from)) {
                if (exit && exit != s.link.from) return false;
                exit = s.link.from;
            }
    }
    if (!exit) return false;
    std::map<std::string, Link> inputs;
    std::map<std::string, Value> consts;
    int next = 1;
    static const std::map<std::string, std::string> ops = {
        {"add", "+"}, {"sub", "-"}, {"mul", "*"}, {"div", "/"}, {"mod", "%"}, {"pow", "**"}, {"eq", "=="}, {"ne", "!="},
        {"lt", "<"},  {"le", "<="}, {"gt", ">"},  {"ge", ">="}, {"and", "&&"}, {"or", "||"}};
    std::function<std::string(const InputSlot&)> slot;
    std::function<std::string(const Node&)> text = [&](const Node& n) -> std::string {
        const std::string& t = n.type;
        if (IsReaderName(t)) return t;
        if (t == "attr") return n.text.rfind("parent.", 0) == 0 ? n.text : n.text;
        if (t == "expr") {
            std::unique_ptr<Expr> e;
            Diag d;
            if (!ParseExpr(n.text, &e, &d)) return "0";
            std::function<void(Expr&)> sub = [&](Expr& x) {
                if (x.kind == Expr::Name)
                    if (const InputSlot* s = n.In(x.text)) {
                        std::unique_ptr<Expr> r;
                        if (ParseExpr(slot(*s), &r, &d)) x = std::move(*r);
                        return;
                    }
                for (auto& a : x.args) sub(*a);
            };
            sub(*e);
            return "(" + PrintExpr(*e) + ")";
        }
        const std::string op = t.substr(5);
        const NodeDef* d = FindDef(t);
        std::vector<std::string> a;
        for (size_t k = 0; d && k < d->inputs.size(); k++) a.push_back(slot(*n.In(d->inputs[k].name)));
        auto it = ops.find(op);
        if (it != ops.end() && a.size() == 2) return "(" + a[0] + " " + it->second + " " + a[1] + ")";
        if (op == "neg") return "(-" + a[0] + ")";
        if (op == "not") return "(!" + a[0] + ")";
        if (op == "switch") return "(" + a[0] + " ? " + a[1] + " : " + a[2] + ")";
        if (op == "sep_x" || op == "sep_y" || op == "sep_z") return "(" + a[0] + ")." + op.substr(4);
        std::string s = op + "(";
        for (size_t k = 0; k < a.size(); k++) s += (k ? ", " : "") + a[k];
        return s + ")";
    };
    slot = [&](const InputSlot& s) -> std::string {
        if (!s.link.from) {
            const Value& v = s.value;
            if (v.type == Type::Vector) return "vec(" + Num(v.v.x) + ", " + Num(v.v.y) + ", " + Num(v.v.z) + ")";
            if (v.type == Type::Bool) return v.i ? "true" : "false";
            if (v.type == Type::Int) return std::to_string(v.i);
            return Num(AsFloat(v));
        }
        const Node* f = g.Find(s.link.from);
        if (!f) return "0";
        if (f->type == "macro" || f->type == "int_macro") return std::string(f->type == "macro" ? "m" : "i") + std::to_string(f->setting);
        if (S.count(f->id)) return text(*f);
        for (const auto& kv : inputs)
            if (kv.second.from == s.link.from && kv.second.output == s.link.output) return kv.first;
        const std::string name = "_" + std::to_string(next++);
        inputs[name] = s.link;
        return name;
    };
    const Node exitNode = *g.Find(exit);
    std::unique_ptr<Expr> e;
    Diag d;
    if (!ParseExpr(text(exitNode), &e, &d)) return false;
    const std::string formula = PrintExpr(*e);
    // the users of the exit (to wire to the Expression)
    std::vector<std::pair<uint32_t, std::string>> users;
    for (const Node& n : g.nodes)
        if (!S.count(n.id))
            for (const InputSlot& s : n.inputs)
                if (s.link.from == exit) users.push_back({n.id, s.name});
    // the set goes, the Expression takes the exit's key and place
    for (uint32_t id : S) RemoveNode(g, id);
    const uint32_t ex = AddNode(g, "expr", exitNode.key, exitNode.x, exitNode.y);
    Node* en = g.Find(ex);
    en->text = formula;
    for (const auto& kv : inputs) {
        InputSlot s;
        s.name = kv.first;
        s.link = kv.second;
        en->inputs.push_back(s);
    }
    for (const auto& u : users)
        if (Node* n = g.Find(u.first))
            if (InputSlot* s = n->In(u.second)) s->link = {ex, 0};
    return true;
}

bool ApplyCode(Graph& g, const std::string& code, std::vector<Diag>* errors, std::vector<Diag>* notes)
{
    Graph out;
    if (!CodeToGraph(code, g, &out, errors, notes)) return false;
    out.revision = g.revision;
    g = out;
    return true;
}

// ---- presets ----
double MacroDefault(int k)
{
    switch (k) {
    case 0: return 5;
    case 1: return 100;
    case 2: return 100;
    default: return 0;
    }
}
int64_t IntMacroDefault(int k) { return k == 0 ? 5 : 0; }

static Preset FromCode(const char* code)
{
    Preset p;
    std::vector<Diag> e, n;
    CodeToGraph(code, Graph(), &p.graph, &e, &n);
    return p;
}

Preset DefaultPreset()
{
    Preset p = FromCode("g = grid(cx: i1, cy: i1, sx: m2, sy: m2)\noutput(g, copy_scale: m3)\n");
    p.graph.intMacros[0] = {"Count", 1, 50};
    p.graph.macros[1] = {"Spacing", 0, 400};
    p.graph.macros[2] = {"Copy Scale", 0, 200};
    p.macros = {{true, 0, (double)IntMacroDefault(0)}, {false, 1, MacroDefault(1)}, {false, 2, MacroDefault(2)}};
    return p;
}

Preset CirclePreset()
{
    Preset p = FromCode("c = circle(count: i1, radius: m2)\noutput(c, copy_scale: m3)\n");
    p.graph.intMacros[0] = {"Count", 1, 100};
    p.graph.macros[1] = {"Radius", 0, 1000};
    p.graph.macros[2] = {"Copy Scale", 0, 200};
    p.macros = {{true, 0, 12}, {false, 1, 300}, {false, 2, 100}};
    return p;
}

const std::vector<PresetInfo>& Presets()
{
    static const std::vector<PresetInfo> list = {{"Grid", DefaultPreset}, {"Circle", CirclePreset}};
    return list;
}

// ---- scripts ----
bool RunScript(Graph& g, const std::string& script, std::vector<MacroWrite>* writes, std::string* error)
{
    std::map<std::string, uint32_t> alias;
    size_t pos = 0;
    int lineNo = 0;
    std::string code;
    bool inCode = false;
    while (pos < script.size()) {
        size_t e = script.find('\n', pos);
        if (e == std::string::npos) e = script.size();
        std::string line = script.substr(pos, e - pos);
        pos = e + 1;
        lineNo++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto fail = [&](const std::string& why) {
            if (error) *error = "script line " + std::to_string(lineNo) + ": " + why + " (" + line + ")";
            return false;
        };
        if (inCode) {
            if (line == "end") {
                std::vector<Diag> errs, notes;
                inCode = false;
                if (!ApplyCode(g, code, &errs, &notes))
                    return fail("code: line " + std::to_string(errs.empty() ? 0 : errs[0].pos.line) + ": " + (errs.empty() ? "?" : errs[0].msg));
                code.clear();
            } else {
                code += line + "\n";
            }
            continue;
        }
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::vector<std::string> t;
        {
            size_t i = 0;
            while (i < line.size()) {
                while (i < line.size() && line[i] == ' ') i++;
                if (i >= line.size()) break;
                size_t j = i;
                while (j < line.size() && line[j] != ' ') j++;
                t.push_back(line.substr(i, j - i));
                i = j;
            }
        }
        if (t.empty()) continue;
        auto node = [&](const std::string& s, uint32_t* id) {
            auto it = alias.find(s);
            if (it != alias.end()) return *id = it->second, true;
            if (const Node* n = g.FindKey(s)) return *id = n->id, true;
            double v = 0;
            if (!Num(s, &v) || v < 1) return false;
            *id = (uint32_t)v;
            return g.Find(*id) != nullptr;
        };
        auto tail = [&](size_t from) {
            std::string s;
            for (size_t i = from; i < t.size(); i++) s += (i > from ? " " : "") + t[i];
            return s;
        };
        auto input = [&](uint32_t id, const std::string& k) { return V1Input(*g.Find(id), k); };
        const std::string& c = t[0];
        uint32_t a = 0, b = 0;
        double x = 0, y = 0;
        bool isInt = false;
        int k = 0;
        std::string why;
        if (c == "code") {
            inCode = true;
        } else if (c == "clear") {
            const uint32_t rev = g.revision;
            g = Graph();
            g.revision = rev;
        } else if (c == "preset" && t.size() >= 2) {
            bool found = false;
            for (const PresetInfo& pi : Presets())
                if (t[1] == pi.name) {
                    const uint32_t rev = g.revision;
                    Preset p = pi.make();
                    g = p.graph;
                    g.revision = rev;
                    if (writes) writes->insert(writes->end(), p.macros.begin(), p.macros.end());
                    found = true;
                }
            if (!found) return fail("no such preset");
        } else if (c == "add" && t.size() >= 4) {
            std::string type;
            for (char ch : t[1]) type += (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
            if (type == "source") type = "source_layers";
            if (!Num(t[2], &x) || !Num(t[3], &y)) return fail("bad add");
            const uint32_t id = AddNodeAt(g, type, x, y);
            if (!id) return fail("cannot add");
            if (t.size() >= 6 && t[4] == "as") alias[t[5]] = id;
        } else if (c == "remove" && t.size() >= 2) {
            if (!node(t[1], &a) || !RemoveNode(g, a)) return fail("bad remove");
        } else if (c == "move" && t.size() >= 4) {
            if (!node(t[1], &a) || !Num(t[2], &x) || !Num(t[3], &y) || !MoveNode(g, a, x, y)) return fail("bad move");
        } else if (c == "wire" && t.size() >= 3) {
            if (!node(t[1], &a) || !node(t[2], &b)) return fail("bad wire");
            if (!Connect(g, a, 0, b, "geometry", &why)) return fail("cannot wire: " + why);
        } else if (c == "unwire" && t.size() >= 2) {
            if (!node(t[1], &a)) return fail("bad unwire");
            Disconnect(g, a, "geometry");
        } else if (c == "set" && t.size() >= 4) {
            if (!node(t[1], &a) || !Num(t[3], &x) || !SetConst(g, a, input(a, t[2]), x)) return fail("bad set");
        } else if (c == "expr" && t.size() >= 4) {
            if (!node(t[1], &a) || !SetExpression(g, a, input(a, t[2]), tail(3), &why)) return fail("bad expr: " + why);
        } else if (c == "link" && t.size() >= 4) {
            if (!node(t[1], &a) || !MacroName(t[3], &isInt, &k) || !LinkMacro(g, a, input(a, t[2]), isInt, k)) return fail("bad link");
        } else if (c == "unlink" && t.size() >= 3) {
            if (!node(t[1], &a) || !Disconnect(g, a, input(a, t[2]))) return fail("bad unlink");
        } else if (c == "expose" && t.size() >= 3) {
            MacroWrite w;
            if (!node(t[1], &a) || !Expose(g, a, input(a, t[2]), &w)) return fail("cannot expose");
            if (writes) writes->push_back(w);
        } else if (c == "name" && t.size() >= 2) {
            if (!MacroName(t[1], &isInt, &k) || !RenameMacro(g, isInt, k, tail(2))) return fail("bad name");
        } else if (c == "range" && t.size() >= 4) {
            if (!MacroName(t[1], &isInt, &k) || !Num(t[2], &x) || !Num(t[3], &y) || !SetMacroRange(g, isInt, k, x, y))
                return fail("bad range");
        } else if (c == "value" && t.size() >= 3) {
            if (!MacroName(t[1], &isInt, &k) || !Num(t[2], &x)) return fail("bad value");
            if (writes) writes->push_back({isInt, k, x});
        } else {
            return fail("unknown command");
        }
    }
    if (inCode) {
        if (error) *error = "code without end";
        return false;
    }
    return true;
}

}  // namespace dx5
