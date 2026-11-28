// DupliX 2 (design v5 12, 12.1): the editor's operations on the user's groups: copy a built-in group for yourself,
// group a selection (Ctrl+G) and back, an Expression node to a group.
#include <algorithm>
#include <functional>
#include <map>
#include <set>

#include "dxg5/edit.h"
#include "dxg5/lang.h"

namespace dx5 {

namespace {

bool NameTaken(const Graph& g, const std::string& n) { return FindDef(n) || FindUserGroup(g, n); }

std::string FreeName(const Graph& g, const std::string& want, bool numberFirst)
{
    if (!numberFirst && !NameTaken(g, want)) return want;
    for (int k = numberFirst ? 1 : 2;; k++) {
        const std::string s = want + (numberFirst ? "" : "_") + std::to_string(k);
        if (!NameTaken(g, s)) return s;
    }
}

std::string Unique(std::set<std::string>& used, const std::string& want)
{
    std::string s = want;
    for (int k = 2; used.count(s); k++) s = want + "_" + std::to_string(k);
    used.insert(s);
    return s;
}

bool IsGeoOut(const Graph& g, const Node& n, int output) { return OutputType(g, n, output) == Type::Geometry; }

// the group's table entry from its body: its parameters as given, structural where they reach a structural input,
// its outputs from the group_output node
NodeDef DefFromBody(const std::string& name, const Graph& body, std::vector<InputDef> params)
{
    std::set<uint32_t> structural;
    std::function<void(uint32_t)> up = [&](uint32_t id) {
        if (!structural.insert(id).second) return;
        const Node* n = body.Find(id);
        if (!n) return;
        for (const InputSlot& in : n->inputs)
            if (in.link.from) {
                const Node* f = body.Find(in.link.from);
                if (f && !IsGeoOut(body, *f, in.link.output)) up(in.link.from);
            }
    };
    for (const Node& n : body.nodes) {
        const NodeDef* d = DefFor(body, n.type);
        if (!d) continue;
        for (const InputSlot& in : n.inputs) {
            const int k = d->Input(in.name);
            if (k >= 0 && d->inputs[k].structural && in.link.from) up(in.link.from);
        }
    }
    std::string geoParam;
    for (const InputDef& p : params)
        if (p.type == Type::Geometry && geoParam.empty()) geoParam = p.name;
    for (InputDef& p : params) {
        if (p.type == Type::Geometry) continue;
        for (const Node& n : body.nodes)
            if (n.type == "param" && n.text == p.name && structural.count(n.id)) p.structural = true;
        p.field = !p.structural;
        p.context = p.structural ? "" : (geoParam.empty() ? "make" : geoParam);
    }
    std::vector<OutputDef> outs;
    for (const Node& n : body.nodes)
        if (n.type == "group_output")
            for (const InputSlot& in : n.inputs) {
                const Node* f = in.link.from ? body.Find(in.link.from) : nullptr;
                const Type t = f ? OutputType(body, *f, in.link.output) : Type::Float;
                outs.push_back({in.name, t == Type::Any ? Type::Float : t, t != Type::Geometry});
            }
    return UserGroupDef(name, params, outs);
}

bool Used(const Graph& g, const std::string& type)
{
    for (const Node& n : g.nodes)
        if (n.type == type) return true;
    for (const UserGroup& u : g.groups)
        if (u.body)
            for (const Node& n : u.body->nodes)
                if (n.type == type) return true;
    return false;
}

}  // namespace

bool CopyGroupForSelf(Graph& g, uint32_t node, std::string* newName)
{
    Node* n = g.Find(node);
    const NodeDef* d = n ? FindDef(n->type) : nullptr;
    const std::string* src = d && d->group ? GroupSource(n->type) : nullptr;
    if (!src) return false;
    auto body = std::make_shared<Graph>();
    std::string nm;
    std::vector<Diag> errs;
    if (!GroupToGraph(*src, body.get(), &nm, &errs)) return false;
    // the definition's Output is the group's output
    for (Node& x : body->nodes)
        if (x.type == "output") {
            x.type = "group_output";
            x.key = "return";  // as the code reader names it
            std::vector<InputSlot> keep;
            for (const InputSlot& s : x.inputs)
                if (s.name == "geometry") keep.push_back(s);
            x.inputs = keep;
        }
    body->groups = g.groups;
    UserGroup u;
    u.name = FreeName(g, n->type + "_copy", false);
    u.body = body;
    u.def = *d;
    u.def.name = u.def.label = u.name;
    u.def.category = "Group";
    g.groups.push_back(u);
    n = g.Find(node);
    n->type = u.name;
    if (newName) *newName = u.name;
    return true;
}

bool GroupSelection(Graph& g, const std::vector<uint32_t>& nodes, const std::string& name, uint32_t* groupNode)
{
    std::set<uint32_t> S(nodes.begin(), nodes.end());
    if (S.empty()) return false;
    double cx = 0, cy = 0;
    for (uint32_t id : S) {
        const Node* n = g.Find(id);
        if (!n || n->zone || n->type == "output" || n->type == "each" || n->type == "param" || n->type == "param_geo" ||
            n->type == "group_output")
            return false;
        for (const Node& z : g.nodes)
            if (z.zone == id) return false;
        cx += n->x, cy += n->y;
    }
    cx /= (double)S.size(), cy /= (double)S.size();
    const std::string gname = name.empty() ? FreeName(g, "group", true) : name;
    if (NameTaken(g, gname)) return false;
    // inputs: each wire from outside (one parameter per distinct source)
    std::map<std::pair<uint32_t, int>, std::string> paramOf;
    std::vector<InputDef> params;
    std::set<std::string> pnames;
    for (const Node& n : g.nodes) {
        if (!S.count(n.id)) continue;
        for (const InputSlot& s : n.inputs) {
            if (!s.link.from || S.count(s.link.from) || paramOf.count({s.link.from, s.link.output})) continue;
            const Node* f = g.Find(s.link.from);
            if (!f) continue;
            const bool geo = IsGeoOut(g, *f, s.link.output);
            InputDef p;
            p.name = Unique(pnames, geo ? "geometry" : s.name);
            const Type t = OutputType(g, *f, s.link.output);
            p.type = geo ? Type::Geometry : (t == Type::Any ? Type::Float : t);
            p.dflt = geo ? Value{} : Value::F(0);
            if (!geo) {
                Value c;
                if (Convert(Value::F(0), p.type, &c)) p.dflt = c;
            }
            p.softMin = 0, p.softMax = 100;
            params.push_back(p);
            paramOf[{s.link.from, s.link.output}] = p.name;
        }
    }
    // outputs: each output of the selection used outside, and who uses it
    std::vector<std::pair<uint32_t, int>> outs;
    std::vector<std::string> outNames;
    std::set<std::string> onames;
    struct Use {
        uint32_t node;
        std::string input;
        int output;
    };
    std::vector<Use> uses;
    for (const Node& n : g.nodes) {
        if (S.count(n.id)) continue;
        for (const InputSlot& s : n.inputs) {
            if (!s.link.from || !S.count(s.link.from)) continue;
            const std::pair<uint32_t, int> o = {s.link.from, s.link.output};
            auto it = std::find(outs.begin(), outs.end(), o);
            int k = (int)(it - outs.begin());
            if (it == outs.end()) {
                outs.push_back(o);
                outNames.push_back(Unique(onames, IsGeoOut(g, *g.Find(o.first), o.second) ? "geometry" : "value"));
            }
            uses.push_back({n.id, s.name, k});
        }
    }
    // the body: the nodes (same ids and keys), a parameter node per input, the group's output
    auto body = std::make_shared<Graph>();
    body->groups = g.groups;
    body->nextId = g.nextId;
    for (const Node& n : g.nodes)
        if (S.count(n.id)) body->nodes.push_back(n);
    std::map<std::string, uint32_t> pnode;
    for (const InputDef& p : params) {
        const uint32_t id = AddNode(*body, p.type == Type::Geometry ? "param_geo" : "param", p.name, cx - 300, cy);
        body->Find(id)->text = p.name;
        pnode[p.name] = id;
    }
    for (Node& n : body->nodes)
        for (InputSlot& s : n.inputs) {
            auto it = paramOf.find({s.link.from, s.link.output});
            if (s.link.from && !S.count(s.link.from) && it != paramOf.end()) s.link = {pnode[it->second], 0};
        }
    const uint32_t ret = AddNode(*body, "group_output", "return", cx + 300, cy);
    for (size_t k = 0; k < outs.size(); k++) {
        InputSlot s;
        s.name = outNames[k];
        s.link = {outs[k].first, outs[k].second};
        body->Find(ret)->inputs.push_back(s);
    }
    UserGroup u;
    u.name = gname;
    u.def = DefFromBody(gname, *body, params);
    u.body = body;
    // the graph: the selection goes, the group node takes its wires
    for (uint32_t id : S) RemoveNode(g, id);
    g.groups.push_back(u);
    const uint32_t gn = AddNode(g, gname, "", cx, cy);
    Node* G = g.Find(gn);
    for (const auto& kv : paramOf) {
        InputSlot* s = G->In(kv.second);
        if (s) s->link = {kv.first.first, kv.first.second};
    }
    for (const Use& x : uses)
        if (Node* m = g.Find(x.node))
            if (InputSlot* s = m->In(x.input)) s->link = {gn, x.output};
    if (groupNode) *groupNode = gn;
    return true;
}

bool UngroupNode(Graph& g, uint32_t node)
{
    const Node* n0 = g.Find(node);
    const UserGroup* u0 = n0 ? FindUserGroup(g, n0->type) : nullptr;
    if (!u0 || !u0->body) return false;
    const Node G = *n0;
    const UserGroup u = *u0;
    const Graph& body = *u.body;
    // who uses the group node's outputs
    struct Use {
        uint32_t node;
        std::string input;
        int output;
    };
    std::vector<Use> uses;
    for (const Node& m : g.nodes)
        for (const InputSlot& s : m.inputs)
            if (s.link.from == node) uses.push_back({m.id, s.name, s.link.output});
    RemoveNode(g, node);
    // the body's nodes come in with new ids (their keys kept when free)
    std::map<uint32_t, uint32_t> remap;
    for (const Node& b : body.nodes) {
        if (b.type == "param" || b.type == "param_geo" || b.type == "group_output") continue;
        Node c = b;
        c.id = g.nextId++;
        c.key = UniqueKey(g, b.key);
        remap[b.id] = c.id;
        g.nodes.push_back(c);
    }
    // a wire from a parameter is what fed the group node's input
    auto outer = [&](const Link& l, InputSlot* into) {
        const Node* p = body.Find(l.from);
        if (p && (p->type == "param" || p->type == "param_geo")) {
            const InputSlot* gs = G.In(p->text);
            if (gs && gs->link.from) into->link = gs->link;
            else {
                into->link = {};
                if (gs) into->value = gs->value;
            }
            return;
        }
        auto it = remap.find(l.from);
        into->link = it == remap.end() ? Link{} : Link{it->second, l.output};
    };
    for (Node& c : g.nodes) {
        if (!std::any_of(remap.begin(), remap.end(), [&](const std::pair<const uint32_t, uint32_t>& kv) { return kv.second == c.id; }))
            continue;
        for (InputSlot& s : c.inputs)
            if (s.link.from) outer(Link(s.link), &s);
    }
    const Node* ret = nullptr;
    for (const Node& b : body.nodes)
        if (b.type == "group_output") ret = &b;
    for (const Use& x : uses) {
        Node* m = g.Find(x.node);
        InputSlot* s = m ? m->In(x.input) : nullptr;
        if (!s) continue;
        if (!ret || x.output < 0 || x.output >= (int)ret->inputs.size()) {
            s->link = {};
            continue;
        }
        const InputSlot& r = ret->inputs[x.output];
        if (r.link.from) outer(r.link, s);
        else s->link = {}, s->value = r.value;
    }
    if (!Used(g, u.name))
        g.groups.erase(std::remove_if(g.groups.begin(), g.groups.end(), [&](const UserGroup& x) { return x.name == u.name; }), g.groups.end());
    return true;
}

bool ExpressionToGroup(Graph& g, uint32_t exprNode, std::string* name)
{
    const Node* e = g.Find(exprNode);
    if (!e || e->type != "expr") return false;
    uint32_t gn = 0;
    if (!GroupSelection(g, {exprNode}, "", &gn)) return false;
    if (name) *name = g.Find(gn)->type;
    return true;
}

std::vector<int> UnusedNamedMacros(const Graph& g, bool isInt)
{
    const uint32_t used = isInt ? UsedIntMacros(g) : UsedMacros(g);
    const MacroSetting* ms = isInt ? g.intMacros : g.macros;
    std::vector<int> r;
    for (int k = 0; k < 32; k++)
        if (!(used >> k & 1) && !ms[k].name.empty()) r.push_back(k);
    return r;
}

std::string ExposeRefusal(const Graph& g, bool isInt)
{
    return FreeMacro(g, isInt) < 0 ? "No free macro (clear the name of an unused macro to use it)" : "";
}

}  // namespace dx5
