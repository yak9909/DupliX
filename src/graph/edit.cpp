// DupliX 2 (design v4, 1.2 and 3): the editor's operations, the presets and the test scripts.
#include "graph/edit.h"

#include <algorithm>
#include <charconv>
#include <cmath>

#include "graph/eval.h"

namespace dxg {

namespace {

Param* FindParamOf(Graph& g, uint32_t id, const std::string& key)
{
    Node* n = g.FindNode(id);
    return n ? n->Find(key) : nullptr;
}

const ParamSpec* SpecOf(const Graph& g, uint32_t id, const std::string& key)
{
    const Node* n = g.FindNode(id);
    return n ? FindParam(Spec(n->type), key) : nullptr;
}

bool Num(const std::string& s, double* v)
{
    auto r = std::from_chars(s.data(), s.data() + s.size(), *v);
    return r.ec == std::errc() && r.ptr == s.data() + s.size() && std::isfinite(*v);
}

}  // namespace

uint32_t AddNode(Graph& g, NodeType type, double x, double y)
{
    if (!Spec(type)) return 0;
    if (type == NodeType::Output && g.OutputNode()) return 0;
    Node n = MakeNode(type);
    n.id = g.nextId++;
    n.x = x, n.y = y;
    g.nodes.push_back(n);
    return n.id;
}

bool RemoveNode(Graph& g, uint32_t id)
{
    auto it = std::find_if(g.nodes.begin(), g.nodes.end(), [&](const Node& n) { return n.id == id; });
    if (it == g.nodes.end()) return false;
    g.nodes.erase(it);
    g.wires.erase(std::remove_if(g.wires.begin(), g.wires.end(), [&](const Wire& w) { return w.from == id || w.to == id; }),
                  g.wires.end());
    return true;
}

bool MoveNode(Graph& g, uint32_t id, double x, double y)
{
    Node* n = g.FindNode(id);
    if (!n) return false;
    n->x = x, n->y = y;
    return true;
}

bool MakesLoop(const Graph& g, uint32_t from, uint32_t to)
{
    // a loop if `to` already feeds `from` (walk upstream from `from`)
    std::vector<uint32_t> todo = {from}, seen;
    while (!todo.empty()) {
        const uint32_t n = todo.back();
        todo.pop_back();
        if (n == to) return true;
        if (std::find(seen.begin(), seen.end(), n) != seen.end()) continue;
        seen.push_back(n);
        for (const Wire& w : g.wires)
            if (w.to == n) todo.push_back(w.from);
    }
    return false;
}

bool Connect(Graph& g, uint32_t from, uint32_t to, int port)
{
    const Node* a = g.FindNode(from);
    const Node* b = g.FindNode(to);
    if (!a || !b || from == to) return false;
    const NodeSpec* sa = Spec(a->type);
    const NodeSpec* sb = Spec(b->type);
    if (!sa || !sb || !sa->hasOutput || port < 0 || port >= sb->numInputs) return false;
    if (MakesLoop(g, from, to)) return false;
    Disconnect(g, to, port);
    g.wires.push_back({from, to, port});
    return true;
}

bool Disconnect(Graph& g, uint32_t to, int port)
{
    const size_t n = g.wires.size();
    g.wires.erase(std::remove_if(g.wires.begin(), g.wires.end(), [&](const Wire& w) { return w.to == to && w.port == port; }),
                  g.wires.end());
    return g.wires.size() != n;
}

bool SetValue(Graph& g, uint32_t id, const std::string& key, double v)
{
    Param* p = FindParamOf(g, id, key);
    if (!p || !std::isfinite(v)) return false;
    const ParamSpec* s = SpecOf(g, id, key);
    if (s && s->choices) {
        int n = 1;
        for (const char* c = s->choices; *c; c++) n += *c == '|';
        v = RoundCount(v, 0, n - 1);
    } else if (s && s->structural) {
        v = std::round(v);
    }
    p->in.base = v;
    return true;
}

bool LinkMacro(Graph& g, uint32_t id, const std::string& key, int k, double depth)
{
    Param* p = FindParamOf(g, id, key);
    const ParamSpec* s = SpecOf(g, id, key);
    if (!p || k < 0 || k >= kNumMacros || !std::isfinite(depth)) return false;
    if (s && s->choices) return false;  // a choice takes constants only (design v4 2: choices do not vary)
    const std::string src = MacroSrc(k);
    for (Link& l : p->in.links)
        if (l.src == src) {
            l.depth = depth;
            return true;
        }
    p->in.links.push_back({src, depth});
    return true;
}

bool UnlinkMacro(Graph& g, uint32_t id, const std::string& key, int k)
{
    Param* p = FindParamOf(g, id, key);
    if (!p) return false;
    const std::string src = MacroSrc(k);
    const size_t n = p->in.links.size();
    p->in.links.erase(std::remove_if(p->in.links.begin(), p->in.links.end(), [&](const Link& l) { return l.src == src; }),
                      p->in.links.end());
    return p->in.links.size() != n;
}

int FreeMacro(const Graph& g)
{
    const uint32_t used = UsedMacros(g);
    for (int k = 0; k < kNumMacros; k++)
        if (!(used >> k & 1) && g.macros[k].name.empty()) return k;
    return -1;
}

bool Expose(Graph& g, uint32_t id, const std::string& key, const std::string& name, MacroWrite* write)
{
    Param* p = FindParamOf(g, id, key);
    const ParamSpec* s = SpecOf(g, id, key);
    if (!p || (s && s->choices)) return false;
    const int k = FreeMacro(g);
    if (k < 0) return false;
    const double v = p->in.base;
    p->in.base = 0;
    p->in.links.push_back({MacroSrc(k), 1});
    Macro& m = g.macros[k];
    m.name = !name.empty() ? name : (s ? s->label : key);
    m.sliderMin = s ? std::min(s->softMin, v) : std::min(0.0, v);
    m.sliderMax = s ? std::max(s->softMax, v) : std::max(100.0, v);
    if (write) *write = {k, v};
    return true;
}

bool RenameMacro(Graph& g, int k, const std::string& name)
{
    if (k < 0 || k >= kNumMacros) return false;
    g.macros[k].name = name;
    return true;
}

bool SetMacroRange(Graph& g, int k, double lo, double hi)
{
    if (k < 0 || k >= kNumMacros || !(lo < hi)) return false;
    g.macros[k].sliderMin = lo, g.macros[k].sliderMax = hi;
    return true;
}

double MacroDefault(int k)
{
    switch (k) {
    case 0: return 5;    // Count
    case 1: return 100;  // Spacing
    case 2: return 100;  // Copy Scale
    default: return 0;
    }
}

Preset DefaultPreset()
{
    Preset p;
    Graph& g = p.graph;
    const uint32_t grid = AddNode(g, NodeType::Grid, 40, 60);
    const uint32_t out = AddNode(g, NodeType::Output, 300, 60);
    Connect(g, grid, out, 0);
    for (const char* k : {"countX", "countY"}) SetValue(g, grid, k, 0), LinkMacro(g, grid, k, 0, 1);
    for (const char* k : {"spacingX", "spacingY"}) SetValue(g, grid, k, 0), LinkMacro(g, grid, k, 1, 1);
    SetValue(g, out, "copyScale", 0), LinkMacro(g, out, "copyScale", 2, 1);
    g.macros[0] = {"Count", 1, 50};
    g.macros[1] = {"Spacing", 0, 400};
    g.macros[2] = {"Copy Scale", 0, 200};
    p.macros = {{0, MacroDefault(0)}, {1, MacroDefault(1)}, {2, MacroDefault(2)}};
    return p;
}

Preset CirclePreset()
{
    Preset p;
    Graph& g = p.graph;
    const uint32_t c = AddNode(g, NodeType::Circle, 40, 60);
    const uint32_t out = AddNode(g, NodeType::Output, 300, 60);
    Connect(g, c, out, 0);
    SetValue(g, c, "count", 0), LinkMacro(g, c, "count", 0, 1);
    SetValue(g, c, "radius", 0), LinkMacro(g, c, "radius", 1, 1);
    SetValue(g, out, "copyScale", 0), LinkMacro(g, out, "copyScale", 2, 1);
    g.macros[0] = {"Count", 1, 100};
    g.macros[1] = {"Radius", 0, 1000};
    g.macros[2] = {"Copy Scale", 0, 200};
    p.macros = {{0, 12}, {1, 300}, {2, 100}};
    return p;
}

const std::vector<PresetInfo>& Presets()
{
    static const std::vector<PresetInfo> list = {{"Grid", DefaultPreset}, {"Circle", CirclePreset}};
    return list;
}

bool RunScript(Graph& g, const std::string& script, std::vector<MacroWrite>* writes, std::string* error)
{
    std::map<std::string, uint32_t> alias;
    size_t pos = 0;
    int lineNo = 0;
    while (pos < script.size()) {
        size_t e = script.find('\n', pos);
        if (e == std::string::npos) e = script.size();
        std::string line = script.substr(pos, e - pos);
        pos = e + 1;
        lineNo++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::vector<std::string> t;
        std::string rest;  // the text after the command's fixed words (names may have spaces)
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
        auto fail = [&](const char* why) {
            if (error) *error = "script line " + std::to_string(lineNo) + ": " + why + " (" + line + ")";
            return false;
        };
        auto node = [&](const std::string& s, uint32_t* id) {
            auto it = alias.find(s);
            if (it != alias.end()) return *id = it->second, true;
            double v = 0;
            if (!Num(s, &v) || v < 1) return false;
            *id = (uint32_t)v;
            return g.FindNode(*id) != nullptr;
        };
        auto macro = [&](const std::string& s) { return MacroIndex(s); };
        auto tail = [&](size_t from) {
            std::string s;
            for (size_t i = from; i < t.size(); i++) s += (i > from ? " " : "") + t[i];
            return s;
        };
        const std::string& c = t[0];
        uint32_t a = 0, b = 0;
        double x = 0, y = 0;
        if (c == "clear") {
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
            const NodeSpec* s = SpecByName(t[1]);
            if (!s || !Num(t[2], &x) || !Num(t[3], &y)) return fail("bad add");
            const uint32_t id = AddNode(g, s->type, x, y);
            if (!id) return fail("cannot add");
            if (t.size() >= 6 && t[4] == "as") alias[t[5]] = id;
        } else if (c == "remove" && t.size() >= 2) {
            if (!node(t[1], &a) || !RemoveNode(g, a)) return fail("bad remove");
        } else if (c == "move" && t.size() >= 4) {
            if (!node(t[1], &a) || !Num(t[2], &x) || !Num(t[3], &y) || !MoveNode(g, a, x, y)) return fail("bad move");
        } else if (c == "wire" && t.size() >= 3) {
            if (!node(t[1], &a) || !node(t[2], &b)) return fail("bad wire");
            if (t.size() >= 4 && !Num(t[3], &x)) return fail("bad port");
            if (!Connect(g, a, b, (int)x)) return fail("cannot wire");
        } else if (c == "unwire" && t.size() >= 2) {
            if (!node(t[1], &a)) return fail("bad unwire");
            if (t.size() >= 3 && !Num(t[2], &x)) return fail("bad port");
            Disconnect(g, a, (int)x);
        } else if (c == "set" && t.size() >= 4) {
            if (!node(t[1], &a) || !Num(t[3], &x) || !SetValue(g, a, t[2], x)) return fail("bad set");
        } else if (c == "link" && t.size() >= 5) {
            if (!node(t[1], &a) || !Num(t[4], &x) || !LinkMacro(g, a, t[2], macro(t[3]), x)) return fail("bad link");
        } else if (c == "unlink" && t.size() >= 4) {
            if (!node(t[1], &a) || !UnlinkMacro(g, a, t[2], macro(t[3]))) return fail("bad unlink");
        } else if (c == "expose" && t.size() >= 3) {
            MacroWrite w;
            if (!node(t[1], &a) || !Expose(g, a, t[2], tail(3), &w)) return fail("cannot expose");
            if (writes) writes->push_back(w);
        } else if (c == "name" && t.size() >= 2) {
            if (!RenameMacro(g, macro(t[1]), tail(2))) return fail("bad name");
        } else if (c == "range" && t.size() >= 4) {
            if (!Num(t[2], &x) || !Num(t[3], &y) || !SetMacroRange(g, macro(t[1]), x, y)) return fail("bad range");
        } else if (c == "value" && t.size() >= 3) {  // a macro's value in AE (the test sets it with the graph)
            const int k = macro(t[1]);
            if (k < 0 || !Num(t[2], &x)) return fail("bad value");
            if (writes) writes->push_back({k, x});
        } else {
            return fail("unknown command");
        }
    }
    return true;
}

}  // namespace dxg
