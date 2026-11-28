// DupliX 2 (design v4, section 2): the nodes' work.
#include "graph/eval.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace dxg {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

void Mul3(const double a[9], const double b[9], double o[9])
{
    double r[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    for (int i = 0; i < 9; i++) o[i] = r[i];
}
void Apply3(const double m[9], const double v[3], double o[3])
{
    const double x = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
    const double y = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
    const double z = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
    o[0] = x, o[1] = y, o[2] = z;
}

struct Evaluator {
    const Graph& g;
    const EvalContext& ctx;
    EvalResult& res;
    std::map<uint32_t, std::vector<Element>> memo;  // a node's output, once per evaluation
    std::vector<uint32_t> stack;                    // to stop at a loop (the editor does not make one)

    double Val(const Node& n, const char* key)
    {
        const Param* p = n.Find(key);
        if (!p) {
            const ParamSpec* s = FindParam(Spec(n.type), key);
            return s ? s->dflt : 0;
        }
        return InputValue(p->in, ctx, ctx.time);
    }
    int Count(const Node& n, const char* key, int hi) { return RoundCount(Val(n, key), 0, hi); }

    std::vector<Element> In(const Node& n, int port)
    {
        const Wire* w = g.WireInto(n.id, port);
        if (!w) return {};
        const Node* from = g.FindNode(w->from);
        return from ? Out(*from) : std::vector<Element>{};
    }

    void Cap(std::vector<Element>& v)
    {
        if (v.size() > ctx.maxElements) {
            v.resize(ctx.maxElements);
            res.warnings.push_back("cut to " + std::to_string(ctx.maxElements) + " elements");
        }
    }

    Element Gen(const Node& n, uint32_t i, double x, double y, double z)
    {
        Element e;
        e.id = MixId(n.id, i);
        e.pos[0] = x, e.pos[1] = y, e.pos[2] = z;
        return e;
    }

    std::vector<Element> Grid(const Node& n)
    {
        const int cap = (int)std::min<size_t>(ctx.maxElements, 1000000);
        const int cx = Count(n, "countX", cap), cy = Count(n, "countY", cap), cz = Count(n, "countZ", cap);
        const double sx = Val(n, "spacingX"), sy = Val(n, "spacingY"), sz = Val(n, "spacingZ");
        std::vector<Element> v;
        const double total = (double)cx * cy * cz;
        if (total <= 0) return v;
        v.reserve((size_t)std::min<double>(total, (double)ctx.maxElements + 1));
        uint32_t i = 0;
        for (int c = 0; c < cz; c++)
            for (int b = 0; b < cy; b++)
                for (int a = 0; a < cx; a++, i++) {
                    if (v.size() > ctx.maxElements) goto done;
                    v.push_back(Gen(n, i, (a - (cx - 1) * 0.5) * sx, (b - (cy - 1) * 0.5) * sy, (c - (cz - 1) * 0.5) * sz));
                }
    done:
        return v;
    }

    std::vector<Element> Circle(const Node& n)
    {
        const int cnt = Count(n, "count", (int)std::min<size_t>(ctx.maxElements + 1, 1000000));
        const double r = Val(n, "radius"), a0 = Val(n, "startAngle"), sweep = Val(n, "sweep");
        const bool align = RoundCount(Val(n, "align"), 0, 1) == 1;
        // a closed circle (|sweep| >= 360) does not put the last copy on the first; an arc includes both ends
        const bool closed = std::fabs(sweep) >= 360 - 1e-9;
        const double step = cnt <= 1 ? 0 : (closed ? sweep / cnt : sweep / (cnt - 1));
        std::vector<Element> v;
        v.reserve(cnt);
        for (int i = 0; i < cnt; i++) {
            const double a = a0 + step * i;  // degrees: 0 = up, clockwise on screen
            Element e = Gen(n, (uint32_t)i, r * std::sin(a * kDeg), -r * std::cos(a * kDeg), 0);
            if (align) RotationXYZ(0, 0, a, e.basis);
            v.push_back(e);
        }
        return v;
    }

    std::vector<Element> Line(const Node& n)
    {
        const int cnt = Count(n, "count", (int)std::min<size_t>(ctx.maxElements + 1, 1000000));
        const double len = Val(n, "length"), ang = Val(n, "angle") * kDeg;
        std::vector<Element> v;
        v.reserve(cnt);
        for (int i = 0; i < cnt; i++) {
            const double u = cnt <= 1 ? 0 : (double)i / (cnt - 1) - 0.5;
            v.push_back(Gen(n, (uint32_t)i, u * len * std::cos(ang), u * len * std::sin(ang), 0));
        }
        return v;
    }

    std::vector<Element> Transform(const Node& n)
    {
        std::vector<Element> v = In(n, 0);
        const double P[3] = {Val(n, "posX"), Val(n, "posY"), Val(n, "posZ")};
        double R[9];
        RotationXYZ(Val(n, "rotX"), Val(n, "rotY"), Val(n, "rotZ"), R);
        const double s = Val(n, "scale") / 100.0;
        double piv[3] = {0, 0, 0};
        if (RoundCount(Val(n, "pivot"), 0, 1) == 0 && !v.empty()) {  // Center: the middle of the bounding box
            double lo[3], hi[3];
            for (int k = 0; k < 3; k++) lo[k] = hi[k] = v[0].pos[k];
            for (const Element& e : v)
                for (int k = 0; k < 3; k++) lo[k] = std::min(lo[k], e.pos[k]), hi[k] = std::max(hi[k], e.pos[k]);
            for (int k = 0; k < 3; k++) piv[k] = (lo[k] + hi[k]) * 0.5;
        }
        // design v3 2.3: Position is where the pivot goes; the layout turns and spreads about it. The copies turn
        // with the layout (like children of a parent) but keep their size (Scale spreads the layout only).
        for (Element& e : v) {
            double d[3] = {(e.pos[0] - piv[0]) * s, (e.pos[1] - piv[1]) * s, (e.pos[2] - piv[2]) * s};
            Apply3(R, d, d);
            for (int k = 0; k < 3; k++) e.pos[k] = P[k] + d[k];
            Mul3(R, e.basis, e.basis);
        }
        return v;
    }

    std::vector<Element> Modify(const Node& n)
    {
        std::vector<Element> v = In(n, 0);
        const double dp[3] = {Val(n, "posX"), Val(n, "posY"), Val(n, "posZ")};
        const double dr[3] = {Val(n, "rotX"), Val(n, "rotY"), Val(n, "rotZ")};
        const double ds = Val(n, "scale") / 100.0, dop = Val(n, "opacity") / 100.0;
        // design v3 5.0: position and rotation add; scale and opacity multiply by (1 + value), never below 0.
        // The amount is times the element's Weight.
        for (Element& e : v) {
            const double w = e.weight;
            for (int k = 0; k < 3; k++) e.pos[k] += dp[k] * w;
            if (dr[0] || dr[1] || dr[2]) {
                double R[9];
                RotationXYZ(dr[0] * w, dr[1] * w, dr[2] * w, R);
                Mul3(e.basis, R, e.basis);  // about the copy's own axes
            }
            const double ks = std::max(0.0, 1 + ds * w);
            for (int k = 0; k < 3; k++) e.scale[k] *= ks;
            e.opacity = std::clamp(e.opacity * std::max(0.0, 1 + dop * w), 0.0, 1.0);
        }
        return v;
    }

    std::vector<Element> Source(const Node& n)
    {
        std::vector<Element> v = In(n, 0);
        const int first = RoundCount(Val(n, "first"), 1, kNumLayers), count = RoundCount(Val(n, "count"), 1, kNumLayers);
        const bool random = RoundCount(Val(n, "assign"), 0, 1) == 1;
        std::vector<int> slots;  // the chosen layers that are set, in order
        for (int i = 0; i < count; i++) {
            const int k = first - 1 + i;
            if (k < kNumLayers && (ctx.layersSet >> k & 1)) slots.push_back(k);
        }
        const size_t m = slots.size();
        for (size_t i = 0; i < v.size(); i++) {
            Element& e = v[i];
            if (!m) {
                e.source = -1;
                continue;
            }
            const size_t j = random ? (size_t)(Rand01(e.id, ctx.seed, 0x5352u) * m) % m : i % m;
            e.source = slots[j];
        }
        return v;
    }

    std::vector<Element> Out(const Node& n)
    {
        auto it = memo.find(n.id);
        if (it != memo.end()) return it->second;
        if (std::find(stack.begin(), stack.end(), n.id) != stack.end()) {
            res.warnings.push_back("a loop at node " + std::to_string(n.id));
            return {};
        }
        stack.push_back(n.id);
        std::vector<Element> v;
        switch (n.type) {
        case NodeType::Grid: v = Grid(n); break;
        case NodeType::Circle: v = Circle(n); break;
        case NodeType::Line: v = Line(n); break;
        case NodeType::Transform: v = Transform(n); break;
        case NodeType::Modify: v = Modify(n); break;
        case NodeType::Source: v = Source(n); break;
        case NodeType::Output: v = In(n, 0); break;
        default: break;  // an unknown node passes nothing
        }
        Cap(v);
        stack.pop_back();
        memo[n.id] = v;
        return v;
    }
};

}  // namespace

double InputValue(const Input& in, const EvalContext& ctx, double t)
{
    double v = in.base;
    for (const Link& l : in.links) {
        const int k = MacroIndex(l.src);
        if (k >= 0 && ctx.macro) v += l.depth * ctx.macro(k, t);
    }
    return std::isfinite(v) ? v : 0;
}

int RoundCount(double v, int lo, int hi)
{
    if (!(v == v)) return lo;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return std::clamp((int)std::lround(v), lo, hi);
}

uint64_t MixId(uint64_t a, uint64_t b)
{
    uint64_t z = a * 0x9E3779B97F4A7C15ull ^ (b + 0x632BE59BD9B4E019ull + (a << 6) + (a >> 2));
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

double Rand01(uint64_t id, int seed, uint32_t salt)
{
    const uint64_t h = MixId(MixId(id, (uint64_t)(uint32_t)seed), salt);
    return (double)(h >> 11) * (1.0 / 9007199254740992.0);
}

void RotationXYZ(double rx, double ry, double rz, double out[9])
{
    const double cx = std::cos(rx * kDeg), sx = std::sin(rx * kDeg);
    const double cy = std::cos(ry * kDeg), sy = std::sin(ry * kDeg);
    const double cz = std::cos(rz * kDeg), sz = std::sin(rz * kDeg);
    const double X[9] = {1, 0, 0, 0, cx, -sx, 0, sx, cx};
    const double Y[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};
    const double Z[9] = {cz, -sz, 0, sz, cz, 0, 0, 0, 1};
    double t[9];
    Mul3(Z, Y, t);
    Mul3(t, X, out);
}

std::string FormatElements(const EvalResult& r)
{
    std::string s;
    char b[512];
    std::snprintf(b, sizeof(b), "elements %zu copyScale %.6f\n", r.elements.size(), r.copyScale);
    s += b;
    for (const Element& e : r.elements) {
        std::snprintf(b, sizeof(b),
                      "e %u %016llx pos %.4f %.4f %.4f basis %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f scale %.6f %.6f %.6f "
                      "op %.6f src %d\n",
                      e.index, (unsigned long long)e.id, e.pos[0], e.pos[1], e.pos[2], e.basis[0], e.basis[1], e.basis[2],
                      e.basis[3], e.basis[4], e.basis[5], e.basis[6], e.basis[7], e.basis[8], e.scale[0], e.scale[1],
                      e.scale[2], e.opacity, e.source);
        s += b;
    }
    return s;
}

EvalResult Evaluate(const Graph& g, const EvalContext& ctx)
{
    EvalResult res;
    const Node* out = g.OutputNode();
    if (!out) return res;
    res.hasOutput = true;
    Evaluator ev{g, ctx, res, {}, {}};
    const double cs = ev.Val(*out, "copyScale") / 100.0;
    res.copyScale = std::max(0.0, cs);
    res.elements = ev.Out(*out);
    for (size_t i = 0; i < res.elements.size(); i++) {
        Element& e = res.elements[i];
        e.index = (uint32_t)i;
        e.s = (int)i;
        e.q = res.elements.size() > 1 ? (double)i / (res.elements.size() - 1) : 0;
    }
    return res;
}

}  // namespace dxg
