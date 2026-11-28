// DupliX 2 (design v4): node types, the stored form of the graph (text), and lookups.
#include "graph/graph.h"

#include <charconv>
#include <cmath>
#include <cstring>

namespace dxg {

namespace {

// Changing a key or a node name breaks saved graphs: add new ones instead.
const std::vector<NodeSpec>& Specs()
{
    static const std::vector<NodeSpec> specs = {
        {NodeType::Grid, "Grid", 0, true,
         {{"countX", "Count X", 5, 1, 50, true, nullptr},
          {"countY", "Count Y", 5, 1, 50, true, nullptr},
          {"countZ", "Count Z", 1, 1, 20, true, nullptr},
          {"spacingX", "Spacing X", 100, 0, 400, false, nullptr},
          {"spacingY", "Spacing Y", 100, 0, 400, false, nullptr},
          {"spacingZ", "Spacing Z", 100, 0, 400, false, nullptr}}},
        {NodeType::Circle, "Circle", 0, true,
         {{"count", "Count", 12, 1, 100, true, nullptr},
          {"radius", "Radius", 300, 0, 1000, false, nullptr},
          {"startAngle", "Start Angle", 0, -180, 180, false, nullptr},
          {"sweep", "Sweep", 360, 0, 360, false, nullptr},
          {"align", "Align", 0, 0, 1, true, "Off|Outward"}}},
        {NodeType::Line, "Line", 0, true,
         {{"count", "Count", 10, 1, 100, true, nullptr},
          {"length", "Length", 800, 0, 2000, false, nullptr},
          {"angle", "Angle", 0, -180, 180, false, nullptr}}},
        {NodeType::Transform, "Transform", 1, true,
         {{"posX", "Position X", 0, -1000, 1000, false, nullptr},
          {"posY", "Position Y", 0, -1000, 1000, false, nullptr},
          {"posZ", "Position Z", 0, -1000, 1000, false, nullptr},
          {"rotX", "Rotation X", 0, -180, 180, false, nullptr},
          {"rotY", "Rotation Y", 0, -180, 180, false, nullptr},
          {"rotZ", "Rotation Z", 0, -180, 180, false, nullptr},
          {"scale", "Scale", 100, 0, 200, false, nullptr},
          {"pivot", "Pivot", 0, 0, 1, true, "Center|Origin"}}},
        {NodeType::Modify, "Modify", 1, true,
         {{"posX", "Position X", 0, -500, 500, false, nullptr},
          {"posY", "Position Y", 0, -500, 500, false, nullptr},
          {"posZ", "Position Z", 0, -500, 500, false, nullptr},
          {"rotX", "Rotation X", 0, -180, 180, false, nullptr},
          {"rotY", "Rotation Y", 0, -180, 180, false, nullptr},
          {"rotZ", "Rotation Z", 0, -180, 180, false, nullptr},
          {"scale", "Scale", 0, -100, 200, false, nullptr},
          {"opacity", "Opacity", 0, -100, 100, false, nullptr}}},
        {NodeType::Source, "Source", 1, true,
         {{"first", "First Layer", 1, 1, 8, true, nullptr},
          {"count", "Layer Count", 1, 1, 8, true, nullptr},
          {"assign", "Assign", 0, 0, 1, true, "Sequential|Random"}}},
        {NodeType::Output, "Output", 1, false,
         {{"copyScale", "Copy Scale", 100, 0, 200, false, nullptr}}},
    };
    return specs;
}

bool ParseNum(const std::string& s, double* v)
{
    const char* b = s.data();
    const char* e = b + s.size();
    auto r = std::from_chars(b, e, *v);
    return r.ec == std::errc() && r.ptr == e && std::isfinite(*v);
}
bool ParseU32(const std::string& s, uint32_t* v)
{
    const char* b = s.data();
    const char* e = b + s.size();
    auto r = std::from_chars(b, e, *v);
    return r.ec == std::errc() && r.ptr == e;
}
bool ParseInt(const std::string& s, int* v)
{
    const char* b = s.data();
    const char* e = b + s.size();
    auto r = std::from_chars(b, e, *v);
    return r.ec == std::errc() && r.ptr == e;
}

// shortest text that reads back to the same double, never locale dependent
std::string Num(double v)
{
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, r.ptr);
}

// splits on spaces; `rest` (if given) gets the text after the first `restAfter` tokens, as written
std::vector<std::string> Split(const std::string& line, size_t restAfter = 0, std::string* rest = nullptr)
{
    std::vector<std::string> t;
    size_t i = 0;
    const size_t n = line.size();
    while (i < n) {
        while (i < n && line[i] == ' ') i++;
        if (i >= n) break;
        if (rest && restAfter && t.size() == restAfter) {
            *rest = line.substr(i);
            return t;
        }
        size_t j = i;
        while (j < n && line[j] != ' ') j++;
        t.push_back(line.substr(i, j - i));
        i = j;
    }
    if (rest) rest->clear();
    return t;
}

}  // namespace

const NodeSpec* Spec(NodeType t)
{
    for (const NodeSpec& s : Specs())
        if (s.type == t) return &s;
    return nullptr;
}
const NodeSpec* SpecByName(const std::string& name)
{
    for (const NodeSpec& s : Specs())
        if (name == s.name) return &s;
    return nullptr;
}
const ParamSpec* FindParam(const NodeSpec* s, const std::string& key)
{
    if (!s) return nullptr;
    for (const ParamSpec& p : s->params)
        if (key == p.key) return &p;
    return nullptr;
}

Param* Node::Find(const std::string& key)
{
    for (Param& p : params)
        if (p.key == key) return &p;
    return nullptr;
}
const Param* Node::Find(const std::string& key) const
{
    for (const Param& p : params)
        if (p.key == key) return &p;
    return nullptr;
}

Node* Graph::FindNode(uint32_t id)
{
    for (Node& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}
const Node* Graph::FindNode(uint32_t id) const
{
    for (const Node& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}
const Wire* Graph::WireInto(uint32_t to, int port) const
{
    for (const Wire& w : wires)
        if (w.to == to && w.port == port) return &w;
    return nullptr;
}
const Node* Graph::OutputNode() const
{
    for (const Node& n : nodes)
        if (n.type == NodeType::Output) return &n;
    return nullptr;
}

Node MakeNode(NodeType type)
{
    Node n;
    n.type = type;
    const NodeSpec* s = Spec(type);
    if (s) {
        n.typeName = s->name;
        for (const ParamSpec& p : s->params) n.params.push_back({p.key, {p.dflt, {}}});
    }
    return n;
}

int MacroIndex(const std::string& src)
{
    if (src.size() < 2 || src[0] != 'm') return -1;
    int k = 0;
    if (!ParseInt(src.substr(1), &k) || k < 1 || k > kNumMacros) return -1;
    return k - 1;
}
std::string MacroSrc(int k) { return "m" + std::to_string(k + 1); }

std::string MacroLabel(const Graph& g, int k)
{
    if (k < 0 || k >= kNumMacros) return "";
    return g.macros[k].name.empty() ? "Macro " + std::to_string(k + 1) : g.macros[k].name;
}

uint32_t UsedMacros(const Graph& g)
{
    uint32_t m = 0;
    for (const Node& n : g.nodes)
        for (const Param& p : n.params)
            for (const Link& l : p.in.links) {
                const int k = MacroIndex(l.src);
                if (k >= 0) m |= 1u << k;
            }
    return m;
}

uint32_t UsedLayers(const Graph& g)
{
    uint32_t m = 0;
    for (const Node& n : g.nodes) {
        if (n.type != NodeType::Source) continue;
        // the structural inputs take constants (and macros, which may change over time: count every layer then)
        const Param* f = n.Find("first");
        const Param* c = n.Find("count");
        if (!f || !c) continue;
        if (!f->in.links.empty() || !c->in.links.empty()) return (1u << kNumLayers) - 1;
        const int first = (int)std::lround(f->in.base), count = (int)std::lround(c->in.base);
        for (int i = 0; i < count; i++) {
            const int k = first - 1 + i;
            if (k >= 0 && k < kNumLayers) m |= 1u << k;
        }
    }
    return m;
}

std::string Serialize(const Graph& g)
{
    std::string s = "DupliX graph " + std::to_string(kFormatVersion) + "\n";
    s += "revision " + std::to_string(g.revision) + "\n";
    s += "nextid " + std::to_string(g.nextId) + "\n";
    for (int k = 0; k < kNumMacros; k++) {
        const Macro& m = g.macros[k];
        if (m.name.empty() && m.sliderMin == 0 && m.sliderMax == 100) continue;
        s += "macro " + std::to_string(k + 1) + " " + Num(m.sliderMin) + " " + Num(m.sliderMax);
        if (!m.name.empty()) s += " " + m.name;
        s += "\n";
    }
    for (const Node& n : g.nodes) {
        s += "node " + std::to_string(n.id) + " " + n.typeName + " " + Num(n.x) + " " + Num(n.y) + "\n";
        for (const Param& p : n.params) {
            s += "param " + std::to_string(n.id) + " " + p.key + " " + Num(p.in.base);
            for (const Link& l : p.in.links) s += " " + l.src + " " + Num(l.depth);
            s += "\n";
        }
    }
    for (const Wire& w : g.wires)
        s += "wire " + std::to_string(w.from) + " " + std::to_string(w.to) + " " + std::to_string(w.port) + "\n";
    for (const std::string& l : g.unknownLines) s += l + "\n";
    return s;
}

bool Parse(const std::string& text, Graph* out, std::string* error)
{
    Graph g;
    size_t pos = 0;
    bool header = false;
    int lineNo = 0;
    auto fail = [&](const std::string& why) {
        if (error) *error = "line " + std::to_string(lineNo) + ": " + why;
        return false;
    };
    while (pos <= text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        lineNo++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            if (e >= text.size()) break;
            continue;
        }
        if (!header) {
            const auto t = Split(line);
            int ver = 0;
            if (t.size() < 3 || t[0] != "DupliX" || t[1] != "graph" || !ParseInt(t[2], &ver) || ver < 1)
                return fail("not a DupliX graph");
            header = true;
            continue;
        }
        std::string rest;
        auto t = Split(line);
        const std::string& w = t[0];
        bool known = true;
        if (w == "revision" && t.size() >= 2) {
            known = ParseU32(t[1], &g.revision);
        } else if (w == "nextid" && t.size() >= 2) {
            known = ParseU32(t[1], &g.nextId);
        } else if (w == "macro" && t.size() >= 4) {
            t = Split(line, 4, &rest);
            int k = 0;
            double lo = 0, hi = 0;
            if (ParseInt(t[1], &k) && k >= 1 && k <= kNumMacros && ParseNum(t[2], &lo) && ParseNum(t[3], &hi)) {
                g.macros[k - 1] = {rest, lo, hi};
            } else {
                known = false;
            }
        } else if (w == "node" && t.size() >= 5) {
            Node n;
            if (ParseU32(t[1], &n.id) && ParseNum(t[3], &n.x) && ParseNum(t[4], &n.y) && n.id && !g.FindNode(n.id)) {
                const NodeSpec* s = SpecByName(t[2]);
                if (s) {
                    n = [&] {
                        Node m = MakeNode(s->type);
                        m.id = n.id, m.x = n.x, m.y = n.y;
                        return m;
                    }();
                } else {
                    n.type = NodeType::Unknown;
                    n.typeName = t[2];
                }
                g.nodes.push_back(n);
            } else {
                known = false;
            }
        } else if (w == "param" && t.size() >= 4 && (t.size() - 4) % 2 == 0) {
            uint32_t id = 0;
            Input in;
            bool ok = ParseU32(t[1], &id) && ParseNum(t[3], &in.base);
            for (size_t i = 4; ok && i + 1 < t.size(); i += 2) {
                Link l;
                l.src = t[i];
                ok = ParseNum(t[i + 1], &l.depth);
                in.links.push_back(l);
            }
            Node* n = ok ? g.FindNode(id) : nullptr;
            if (n) {
                Param* p = n->Find(t[2]);
                if (p) p->in = in;
                else n->params.push_back({t[2], in});  // a param this build does not know: kept
            } else {
                known = false;
            }
        } else if (w == "wire" && t.size() >= 4) {
            Wire wi;
            known = ParseU32(t[1], &wi.from) && ParseU32(t[2], &wi.to) && ParseInt(t[3], &wi.port) && wi.port >= 0;
            if (known) g.wires.push_back(wi);
        } else {
            known = false;
        }
        if (!known) g.unknownLines.push_back(line);
        if (e >= text.size()) break;
    }
    if (!header) return fail("empty");
    // wires to nodes that are not there are dropped (a node of a type the writer did not keep, a damaged file)
    std::vector<Wire> wires;
    for (const Wire& wi : g.wires)
        if (g.FindNode(wi.from) && g.FindNode(wi.to)) wires.push_back(wi);
    g.wires = wires;
    for (const Node& n : g.nodes)
        if (n.id >= g.nextId) g.nextId = n.id + 1;
    *out = g;
    return true;
}

}  // namespace dxg
