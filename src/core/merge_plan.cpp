#include "core/merge_plan.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <thread>

#include "core/merge_geom.h"

namespace rnd {

void CopyToWorld(const MergeLayerInput& l, int i, double toWorld[16], double* opacity)
{
    if (!l.explicitCopies.empty()) {
        std::memcpy(toWorld, &l.explicitCopies[(size_t)i * 16], 16 * sizeof(double));
        if (opacity) *opacity = 1;
        return;
    }
    double G[16], a[16];
    WorldM4(l.world, l.anchorX, l.anchorY, G);
    CopyXform x;
    ComputeCopy(l.p, i, l.anchorX, l.anchorY, &x);
    Mul4(x.m4, G, a);
    Mul4(a, l.layerToWorld, toWorld);
    if (opacity) *opacity = x.opacity;
}

namespace {

// The screen rect of quads, clipped to the comp (extended to the alignment) and aligned outward to kMergeAlign.
bool AlignedRect(const std::vector<ScreenQuad>& qs, double compW, double compH, double out[4])
{
    double l = 1e300, t = 1e300, r = -1e300, b = -1e300;
    for (const ScreenQuad& q : qs)
        for (int k = 0; k < 4; k++) l = std::min(l, q.x[k]), r = std::max(r, q.x[k]), t = std::min(t, q.y[k]), b = std::max(b, q.y[k]);
    const double A = kMergeAlign;
    const double maxX = std::ceil(compW / A) * A, maxY = std::ceil(compH / A) * A;
    l = std::max(0.0, std::floor(l / A) * A), t = std::max(0.0, std::floor(t / A) * A);
    r = std::min(maxX, std::ceil(r / A) * A), b = std::min(maxY, std::ceil(b / A) * A);
    if (!(r > l) || !(b > t)) return false;
    out[0] = l, out[1] = t, out[2] = r, out[3] = b;
    return true;
}

// The area of q inside the comp rect shrunk by inset px: what AE surely draws of it (AE culls a surface outside its
// view; a group whose representative is culled is not drawn at all, so the representative is a copy well on screen).
constexpr double kMinRepArea = 16;  // comp px on screen a representative needs
double OnScreenArea(const ScreenQuad& q, double compW, double compH, double inset = 4)
{
    if (!q.valid) return 0;
    double px[16], py[16], tx[16], ty[16];
    int n = 4;
    for (int k = 0; k < 4; k++) px[k] = q.x[k], py[k] = q.y[k];
    // clip by x >= l, x <= r, y >= t, y <= b
    const double lim[4] = {inset, compW - inset, inset, compH - inset};
    for (int e = 0; e < 4 && n; e++) {
        const bool isX = e < 2, low = e % 2 == 0;
        auto inside = [&](double x, double y) {
            const double v = isX ? x : y;
            return low ? v >= lim[e] : v <= lim[e];
        };
        int m = 0;
        for (int i = 0; i < n && m < 14; i++) {
            const int j = (i + n - 1) % n;
            const bool ci = inside(px[i], py[i]), cj = inside(px[j], py[j]);
            if (ci != cj) {
                const double a = isX ? px[j] : py[j], b = isX ? px[i] : py[i];
                const double t = (lim[e] - a) / (b - a);
                tx[m] = px[j] + t * (px[i] - px[j]), ty[m] = py[j] + t * (py[i] - py[j]), m++;
            }
            if (ci) tx[m] = px[i], ty[m] = py[i], m++;
        }
        n = m;
        for (int i = 0; i < n; i++) px[i] = tx[i], py[i] = ty[i];
    }
    double a = 0;
    for (int i = 0; i < n; i++) a += px[i] * py[(i + 1) % n] - px[(i + 1) % n] * py[i];
    return std::fabs(a) / 2;
}

}  // namespace

MergePlan BuildSingleGroupPlan(const MergeSceneInput& in)
{
    MergePlan plan;
    plan.cam = in.cam;
    for (const MergeLayerInput& l : in.layers) {
        MergeLayerPlan lp;
        lp.in = l;
        const int n = std::max(l.p.count, 0);
        std::vector<char> grouped(n, 0);
        MergeCluster c;
        std::vector<ScreenQuad> qs;
        c.depth = 1e300;
        if (!l.block) {
            for (int i = 0; i < n; i++) {
                double toW[16], op = 0;
                CopyToWorld(l, i, toW, &op);
                ScreenQuad q = ProjectRect(in.cam, toW, l.srcRect);
                if (!(op > 0) || !q.valid) continue;
                grouped[i] = 1;
                c.copies.push_back(i);
                qs.push_back(q);
                c.depth = std::min(c.depth, q.zMin);
            }
        }
        const bool group = c.copies.size() >= 2 && AlignedRect(qs, in.cam.compW, in.cam.compH, c.rect);
        if (group) lp.clusters.push_back(c);
        for (int i = 0; i < n; i++) {
            if (!group || !grouped[i]) lp.subs.push_back({i, -1});
            else if (i == c.copies[0]) lp.subs.push_back({-1, 0});
        }
        plan.layers[MergeLayerKey(l.compItemId, l.layerId)] = std::move(lp);
    }
    return plan;
}

MergePlan BuildIsolatedGroupPlan(const MergeSceneInput& in, double marginPx)
{
    const auto t0 = std::chrono::steady_clock::now();
    MergePlan plan;
    plan.cam = in.cam;
    // every other surface on screen (an unbounded plane, a plane crossing the camera's plane or a box: everywhere)
    std::vector<ScreenQuad> others;
    bool everywhere = !in.boxes.empty();
    for (const ScenePlane& sp : in.planes) {
        if (sp.unbounded) {
            everywhere = true;
            continue;
        }
        ScreenQuad q = ProjectRect(in.cam, sp.toWorld, sp.rect);
        if (!q.valid) everywhere = true;
        else others.push_back(q);
    }
    // every copy of every merge layer
    struct C {
        int layer, copy;
        ScreenQuad q;
        double minX, minY, maxX, maxY, area;
        double corners[4][3];
        bool ok;
    };
    std::vector<C> cs;
    for (int li = 0; li < (int)in.layers.size(); li++) {
        const MergeLayerInput& l = in.layers[li];
        for (int i = 0; i < std::max(l.p.count, 0); i++) {
            C c;
            c.layer = li, c.copy = i;
            double toW[16], op = 0;
            CopyToWorld(l, i, toW, &op);
            c.q = ProjectRect(in.cam, toW, l.srcRect);
            RectCorners(toW, l.srcRect, c.corners);
            c.ok = !l.block && c.q.valid && !everywhere;
            c.minX = c.minY = 1e300, c.maxX = c.maxY = -1e300, c.area = 0;
            for (int k = 0; k < 4; k++) {
                c.minX = std::min(c.minX, c.q.x[k]), c.maxX = std::max(c.maxX, c.q.x[k]);
                c.minY = std::min(c.minY, c.q.y[k]), c.maxY = std::max(c.maxY, c.q.y[k]);
                const int n = (k + 1) % 4;
                c.area += c.q.x[k] * c.q.y[n] - c.q.x[n] * c.q.y[k];
            }
            c.area = op > 0 ? OnScreenArea(c.q, in.cam.compW, in.cam.compH) : 0;  // for the representative
            if (c.ok)
                for (const ScreenQuad& o : others)
                    if (QuadsOverlap(c.q, o, marginPx)) {
                        c.ok = false;
                        break;
                    }
            cs.push_back(c);
        }
    }
    const auto tPairs = std::chrono::steady_clock::now();
    // components of copies overlapping on screen (any layer); a component is a group when every copy in it is ok
    // and of one layer
    std::vector<int> parent(cs.size());
    for (size_t i = 0; i < cs.size(); i++) parent[i] = (int)i;
    std::function<int(int)> root = [&](int i) { return parent[i] == i ? i : parent[i] = root(parent[i]); };
    std::vector<int> byX(cs.size());
    for (size_t i = 0; i < cs.size(); i++) byX[i] = (int)i;
    std::sort(byX.begin(), byX.end(), [&](int a, int b) { return cs[a].minX < cs[b].minX; });
    // the bounding boxes in x order, packed (the sweep reads them for every candidate pair)
    struct Box {
        double x0, x1, y0, y1;
    };
    std::vector<Box> boxes(byX.size());
    for (size_t a = 0; a < byX.size(); a++) {
        const C& c = cs[byX[a]];
        boxes[a] = c.q.valid ? Box{c.minX, c.maxX, c.minY, c.maxY} : Box{c.minX, -1e300, 1e300, -1e300};
    }
    const double g = 2 * marginPx;
    // the overlapping pairs on several threads: each a range of the sweep, its pairs in sweep order; then joined
    // (the components do not depend on the order). Only the components count: a thread skips a pair already joined
    // by its own earlier pairs (a union-find of its own), most of the pairs of a dense pile (11/27, 3271 copies of a
    // large image: 1.6 million overlapping pairs, about 280 ms of overlap tests).
    // Copies meeting in 3D join groups too (11/27): AE draws them as a cluster in its M buffer, and so does the group
    // drawing now (DrawGroup draws a group's clusters with AE's own M buffer).
    struct Pair {
        int a, b;
    };
    const int nt = byX.size() >= 1024 ? std::max(1, std::min(8, (int)std::thread::hardware_concurrency())) : 1;
    std::vector<std::vector<Pair>> found((size_t)nt);
    std::vector<double> counted((size_t)nt * 2, 0);
    auto sweep = [&](int t) {
        const size_t from = byX.size() * t / nt, to = byX.size() * (t + 1) / nt;
        std::vector<int> up(cs.size());
        for (size_t i = 0; i < cs.size(); i++) up[i] = (int)i;
        auto find = [&](int i) {
            while (up[i] != i) i = up[i] = up[up[i]];
            return i;
        };
        for (size_t a = from; a < to; a++) {
            const C& A = cs[byX[a]];
            if (!A.q.valid) continue;
            const Box ba = boxes[a];
            for (size_t b = a + 1; b < byX.size() && boxes[b].x0 <= ba.x1 + g; b++) {
                const Box& bb = boxes[b];
                counted[2 * t]++;  // diagnostics: candidate pairs
                if (bb.y0 > ba.y1 + g || ba.y0 > bb.y1 + g) continue;  // also an invalid B (an empty box)
                const int ra = find(byX[a]), rb = find(byX[b]);
                if (ra == rb) continue;  // joined already
                const C& B = cs[byX[b]];
                if (!QuadsOverlap(A.q, B.q, marginPx)) continue;
                counted[2 * t + 1]++;  // diagnostics: overlapping pairs tested and joined
                up[ra] = rb;
                found[t].push_back({byX[a], byX[b]});
            }
        }
    };
    if (nt == 1) {
        sweep(0);
    } else {
        std::vector<std::thread> ts;
        for (int t = 1; t < nt; t++) ts.emplace_back(sweep, t);
        sweep(0);
        for (std::thread& t : ts) t.join();
    }
    for (int t = 0; t < nt; t++) {
        plan.ms[2] += counted[2 * t], plan.ms[3] += counted[2 * t + 1];
        for (const Pair& p : found[t]) parent[root(p.a)] = root(p.b);
    }
    plan.ms[0] = std::chrono::duration<double, std::milli>(tPairs - t0).count();
    plan.ms[1] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tPairs).count();
    // a copy that cannot be placed on screen (behind the camera) overlaps everything it may: its whole layer is
    // left one by one
    std::vector<char> compOk(cs.size(), 1), layerBad(in.layers.size(), 0);
    std::vector<int> compLayer(cs.size(), -1);
    for (size_t i = 0; i < cs.size(); i++) {
        if (!cs[i].q.valid) layerBad[cs[i].layer] = 1;
        const int r = root((int)i);
        if (!cs[i].ok) compOk[r] = 0;
        if (compLayer[r] < 0) compLayer[r] = cs[i].layer;
        else if (compLayer[r] != cs[i].layer) compOk[r] = 0;
    }
    size_t at = 0;
    // (diagnostics, RANDAMU_PLAN_MEETS=1: every overlapping pair of copies in ok components tested for meeting in 3D,
    // counted in fb[13]; tests that groups with crossing copies are drawn as AE draws them)
    {
        const char* ev = std::getenv("RANDAMU_PLAN_MEETS");
        if (ev && ev[0] == '1')
            for (size_t a = 0; a < byX.size(); a++) {
                const C& A = cs[byX[a]];
                if (!A.q.valid || !compOk[root(byX[a])]) continue;
                for (size_t b = a + 1; b < byX.size() && boxes[b].x0 <= boxes[a].x1 + g; b++) {
                    const C& B = cs[byX[b]];
                    if (boxes[b].y0 > boxes[a].y1 + g || boxes[a].y0 > boxes[b].y1 + g || !QuadsOverlap(A.q, B.q, marginPx)) continue;
                    if (A.q.zMax >= B.q.zMin && B.q.zMax >= A.q.zMin && QuadsIntersect3D(A.corners, B.corners)) plan.fb[13]++;
                }
            }
    }
    for (int li = 0; li < (int)in.layers.size(); li++) {
        const MergeLayerInput& l = in.layers[li];
        const int n = std::max(l.p.count, 0);
        MergeLayerPlan lp;
        lp.in = l;
        MergeCluster g;
        g.depth = 0;
        g.rect[0] = g.rect[1] = g.rect[2] = g.rect[3] = 0;
        int rep = -1;
        double repArea = -1;
        for (int i = 0; i < n; i++) {
            const C& c = cs[at + i];
            if (layerBad[li] || !compOk[root((int)(at + i))]) continue;
            g.copies.push_back(i);
            if (c.area > repArea) repArea = c.area, rep = i;
        }
        // no copy surely drawn by AE to stand for the group: one by one
        const bool group = g.copies.size() >= 2 && repArea >= kMinRepArea;
        for (int i = 0; i < n; i++) {
            const bool grouped = group && std::binary_search(g.copies.begin(), g.copies.end(), i);
            MergePlan::DebugCopy dc;
            const C& c = cs[at + i];
            dc.layer = li, dc.copy = i, dc.group = grouped ? 0 : -1;
            for (int k = 0; k < 4; k++) dc.x[k] = c.q.x[k], dc.y[k] = c.q.y[k];
            dc.zMin = c.q.zMin, dc.zMax = c.q.zMax;
            std::memcpy(dc.corners, c.corners, sizeof(dc.corners));
            plan.debugCopies.push_back(dc);
            if (!grouped) lp.subs.push_back({i, -1});
            else if (i == rep) lp.subs.push_back({rep, 0});  // the group: AE draws it as its representative copy
        }
        if (group) lp.clusters.push_back(g);
        plan.layers[MergeLayerKey(l.compItemId, l.layerId)] = std::move(lp);
        at += n;
    }
    return plan;
}

namespace {

struct Pt2 {
    double x, y;
};

double Cross2(const Pt2& o, const Pt2& a, const Pt2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

// A convex polygon of at most 4 + 4 corners (two convex quads clipped), with room to spare.
struct Poly {
    Pt2 p[16];
    int n = 0;
};

// Convex polygon `in` clipped by the convex quad `clip` (Sutherland-Hodgman; any winding), as in ae_order.cpp.
void ClipByQuad(const Poly& in, const Pt2 clip[4], Poly* out)
{
    double area = 0;
    for (int k = 0; k < 4; k++) area += clip[k].x * clip[(k + 1) % 4].y - clip[(k + 1) % 4].x * clip[k].y;
    const double s = area >= 0 ? 1 : -1;
    Poly a = in, b;
    for (int k = 0; k < 4 && a.n; k++) {
        const Pt2& e0 = clip[k];
        const Pt2& e1 = clip[(k + 1) % 4];
        b.n = 0;
        Pt2 prev = a.p[a.n - 1];
        bool prevIn = s * Cross2(e0, e1, prev) >= 0;
        for (int i = 0; i < a.n && b.n < 15; i++) {
            const Pt2& cur = a.p[i];
            const bool curIn = s * Cross2(e0, e1, cur) >= 0;
            if (curIn != prevIn) {
                const double d1x = cur.x - prev.x, d1y = cur.y - prev.y, d2x = e1.x - e0.x, d2y = e1.y - e0.y;
                const double den = d1x * d2y - d1y * d2x;
                const double t = den != 0 ? ((e0.x - prev.x) * d2y - (e0.y - prev.y) * d2x) / den : 0;
                b.p[b.n++] = {prev.x + t * d1x, prev.y + t * d1y};
            }
            if (curIn) b.p[b.n++] = cur;
            prev = cur;
            prevIn = curIn;
        }
        a = b;
    }
    *out = a;
}

double PolyArea2(const Poly& p)
{
    double a = 0;
    for (int k = 0; k < p.n; k++) a += p.p[k].x * p.p[(k + 1) % p.n].y - p.p[(k + 1) % p.n].x * p.p[k].y;
    return std::fabs(a) / 2;
}

// The convex quad q grown by m: every edge moved outward by m, the corners where the moved edges meet (a superset of
// every point within m of q). False if q is degenerate.
bool GrowQuad(const ScreenQuad& q, double m, Pt2 out[4])
{
    double a = 0;
    for (int k = 0; k < 4; k++) a += q.x[k] * q.y[(k + 1) % 4] - q.x[(k + 1) % 4] * q.y[k];
    if (!(std::fabs(a) / 2 > 1e-6)) return false;
    const double s = a > 0 ? 1 : -1;
    Pt2 p[4], d[4];
    for (int k = 0; k < 4; k++) {
        const int n = (k + 1) % 4;
        double dx = q.x[n] - q.x[k], dy = q.y[n] - q.y[k];
        const double len = std::sqrt(dx * dx + dy * dy);
        if (!(len > 1e-9)) return false;
        dx /= len, dy /= len;
        d[k] = {dx, dy};
        p[k] = {q.x[k] + s * dy * m, q.y[k] - s * dx * m};  // outward: (dy, -dx) for a positive area
    }
    for (int k = 0; k < 4; k++) {
        const int b = (k + 3) % 4;  // the edge ending at corner k
        const double den = d[b].x * d[k].y - d[b].y * d[k].x;
        if (std::fabs(den) < 1e-12) {
            out[k] = p[k];
            continue;
        }
        const double t = ((p[k].x - p[b].x) * d[k].y - (p[k].y - p[b].y) * d[k].x) / den;
        out[k] = {p[b].x + t * d[b].x, p[b].y + t * d[b].y};
    }
    return true;
}

constexpr int kApart = 0, kUnsure = 2;  // a pair's order: apart, +1 a in front, -1 b in front, unsure

// A planar quad prepared for the pair judgments: its screen corners, the corners grown by the margin, and its plane's
// camera depth at screen (x, y): z = k / (a x + b y + c) (as PlaneDepthAt: the ray's camera z is 1 per unit).
struct Shape {
    Pt2 q[4], g[4];
    bool grown = false;
    double a = 0, b = 0, c = 0, k = 0;
};
Shape MakeShape(const CameraView& cam, const ScreenQuad& q, const double w[16], double m)
{
    Shape s;
    for (int i = 0; i < 4; i++) s.q[i] = {q.x[i], q.y[i]};
    s.grown = q.valid && GrowQuad(q, m, s.g);
    const double n[3] = {w[1] * w[6] - w[2] * w[5], w[2] * w[4] - w[0] * w[6], w[0] * w[5] - w[1] * w[4]};
    const double* C = cam.camToWorld;
    const double n0 = n[0] * C[0] + n[1] * C[1] + n[2] * C[2], n1 = n[0] * C[4] + n[1] * C[5] + n[2] * C[6],
                 n2 = n[0] * C[8] + n[1] * C[9] + n[2] * C[10];
    s.a = n0 / cam.dist, s.b = n1 / cam.dist;
    s.c = n2 - cam.compW / 2 * s.a - cam.compH / 2 * s.b;
    s.k = n[0] * (w[12] - C[12]) + n[1] * (w[13] - C[13]) + n[2] * (w[14] - C[14]);
    return s;
}
bool ShapeDepth(const Shape& s, const Pt2& p, double* z)
{
    const double nd = s.a * p.x + s.b * p.y + s.c;
    if (std::fabs(nd) < 1e-12) return false;
    *z = s.k / nd;
    return *z > 0;
}

// The order of two planar quads where they overlap on screen.
int JudgePair(const ScreenQuad& qa, const Shape& sa, const ScreenQuad& qb, const Shape& sb, double m, int* why)
{
    *why = 0;
    if (!QuadsOverlap(qa, qb, m)) return kApart;
    // AE's pair test (sub_18002B040 -> sub_18002B350) makes an edge wherever the two screen polygons meet (edge
    // crossings and corners inside the other) and the depths differ by more than 1e-12 there: any overlap orders
    // them. Only the margin band (no overlap, a few px apart) has no edge, and AE's order is then its walk's, while
    // antialiased edges may share a pixel: unsure.
    Poly pa, ov;
    pa.n = 4;
    for (int i = 0; i < 4; i++) pa.p[i] = sa.q[i];
    ClipByQuad(pa, sb.q, &ov);
    if (ov.n < 3 || PolyArea2(ov) < 0.01) return *why = 0, kUnsure;
    if (!sa.grown || !sb.grown) return *why = 1, kUnsure;
    for (int i = 0; i < 4; i++) pa.p[i] = sa.g[i];
    ClipByQuad(pa, sb.g, &ov);
    if (ov.n < 3) return *why = 1, kUnsure;
    bool aFront = true, bFront = true;
    for (int i = 0; i < ov.n; i++) {
        double za = 0, zb = 0;
        if (!ShapeDepth(sa, ov.p[i], &za) || !ShapeDepth(sb, ov.p[i], &zb)) return *why = 1, kUnsure;
        const double tol = 1e-5 * std::max(za, zb);
        if (!(za < zb - tol)) aFront = false;
        if (!(zb < za - tol)) bFront = false;
    }
    return aFront ? 1 : bFront ? -1 : (*why = 2, kUnsure);
}

}  // namespace

MergePlan BuildFrontBackGroupPlan(const MergeSceneInput& in, double marginPx, double budgetMs)
{
    MergePlan plan;
    plan.cam = in.cam;
    // items: every copy of every merge layer, then every other plane (an unbounded plane, a plane crossing the
    // camera's plane or a box: everywhere, nothing is grouped)
    struct It {
        int layer, copy;  // layer -1: another surface
        ScreenQuad q;
        double toW[16];
        double minX, minY, maxX, maxY, area;
        bool ok;
        Shape sh;
    };
    std::vector<It> its;
    bool everywhere = !in.boxes.empty();
    std::vector<char> layerBad(in.layers.size(), 0);
    auto bounds = [&](It& t) {
        t.sh = MakeShape(in.cam, t.q, t.toW, marginPx);
        t.minX = t.minY = 1e300, t.maxX = t.maxY = -1e300, t.area = 0;
        for (int k = 0; k < 4; k++) {
            t.minX = std::min(t.minX, t.q.x[k]), t.maxX = std::max(t.maxX, t.q.x[k]);
            t.minY = std::min(t.minY, t.q.y[k]), t.maxY = std::max(t.maxY, t.q.y[k]);
            const int n = (k + 1) % 4;
            t.area += t.q.x[k] * t.q.y[n] - t.q.x[n] * t.q.y[k];
        }
        t.area = std::fabs(t.area) / 2;
    };
    for (int li = 0; li < (int)in.layers.size(); li++) {
        const MergeLayerInput& l = in.layers[li];
        for (int i = 0; i < std::max(l.p.count, 0); i++) {
            It t;
            t.layer = li, t.copy = i;
            double op = 0;
            CopyToWorld(l, i, t.toW, &op);
            t.q = ProjectRect(in.cam, t.toW, l.srcRect);
            t.ok = !l.block && t.q.valid;
            if (!t.q.valid) layerBad[li] = 1;
            bounds(t);
            t.area = op > 0 ? OnScreenArea(t.q, in.cam.compW, in.cam.compH) : 0;  // for the representative
            its.push_back(t);
        }
    }
    const size_t nCopies = its.size();
    for (const ScenePlane& sp : in.planes) {
        if (sp.unbounded) {
            everywhere = true;
            continue;
        }
        It t;
        t.layer = -1, t.copy = -1;
        std::memcpy(t.toW, sp.toWorld, sizeof(t.toW));
        t.q = ProjectRect(in.cam, sp.toWorld, sp.rect);
        t.ok = false;
        if (!t.q.valid) {
            everywhere = true;
            continue;
        }
        bounds(t);
        its.push_back(t);
    }
    for (size_t i = 0; i < nCopies; i++)
        if (everywhere || layerBad[its[i].layer]) its[i].ok = false;
    // every overlapping pair with a copy in it: per copy, (the other item, the order seen from the copy)
    struct Nb {
        int other, order;
    };
    std::vector<std::vector<Nb>> nbs(nCopies);
    std::vector<int> byX(its.size());
    for (size_t i = 0; i < its.size(); i++) byX[i] = (int)i;
    std::sort(byX.begin(), byX.end(), [&](int a, int b) { return its[a].minX < its[b].minX; });
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t a = 0; a < byX.size(); a++) {
        // past the budget (very dense scenes): the isolated groups (BuildIsolatedGroupPlan) instead
        if ((a & 127) == 127 && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > budgetMs) {
            MergePlan iso = BuildIsolatedGroupPlan(in, marginPx);
            iso.fb[12] = 1;
            return iso;
        }
        const It& A = its[byX[a]];
        if (!A.q.valid) continue;
        for (size_t b = a + 1; b < byX.size() && its[byX[b]].minX <= A.maxX + 2 * marginPx; b++) {
            const It& B = its[byX[b]];
            if (!B.q.valid || B.minY > A.maxY + 2 * marginPx || A.minY > B.maxY + 2 * marginPx) continue;
            if (!A.ok && !B.ok) continue;  // neither may be grouped: their order is AE's either way
            int uw = 0;
            const int o = JudgePair(A.q, A.sh, B.q, B.sh, marginPx, &uw);
            if (o == kApart) continue;
            if (o == kUnsure) plan.fb[6 + uw + (A.layer < 0 || B.layer < 0 ? 3 : 0)]++;
            if (A.layer >= 0) nbs[byX[a]].push_back({byX[b], o == kUnsure ? kUnsure : o});
            if (B.layer >= 0) nbs[byX[b]].push_back({byX[a], o == kUnsure ? kUnsure : -o});
        }
    }
    // the largest sets: a copy leaves its set while a pair breaks the rule (side +1: front, -1: back); a copy that
    // leaves is outside for the copies of its layer still in the set, which are checked again
    std::vector<int> side(nCopies, 0);  // the copy's group: +1 front, -1 back, 0 none
    for (const int want : {1, -1}) {
        std::vector<int> work;
        for (size_t i = 0; i < nCopies; i++)
            if (its[i].ok && !side[i]) side[i] = want, work.push_back((int)i);
        while (!work.empty()) {
            const int x = work.back();
            work.pop_back();
            if (side[x] != want) continue;
            bool bad = false;
            for (const Nb& nb : nbs[x]) {
                const bool mate = nb.other < (int)nCopies && side[nb.other] == want && its[nb.other].layer == its[x].layer;
                bad = nb.order == kUnsure || (!mate && nb.order != want);
                if (bad) break;
            }
            if (!bad) continue;
            side[x] = 0;
            for (const Nb& nb : nbs[x])
                if (nb.other < (int)nCopies && side[nb.other] == want && its[nb.other].layer == its[x].layer)
                    work.push_back(nb.other);
        }
    }
    size_t at = 0;
    for (int li = 0; li < (int)in.layers.size(); li++) {
        const MergeLayerInput& l = in.layers[li];
        const int n = std::max(l.p.count, 0);
        MergeLayerPlan lp;
        lp.in = l;
        MergeCluster g[2];  // front, back
        int rep[2] = {-1, -1};
        double repArea[2] = {-1, -1};
        for (int i = 0; i < n; i++) {
            const It& t = its[at + i];
            if (t.ok) plan.fb[0]++;
            const int s = side[at + i];
            if (!s) {
                bool unsure = false;
                for (const Nb& nb : nbs[at + i]) unsure = unsure || nb.order == kUnsure;
                if (t.ok) plan.fb[unsure ? 3 : 4]++;
                continue;
            }
            const int k = s > 0 ? 0 : 1;
            g[k].copies.push_back(i);
            if (t.area > repArea[k]) repArea[k] = t.area, rep[k] = i;
        }
        int clusterOf[2] = {-1, -1};
        for (int k = 0; k < 2; k++) {
            g[k].depth = 0, g[k].place = k ? -1 : 1;
            g[k].rect[0] = g[k].rect[1] = g[k].rect[2] = g[k].rect[3] = 0;
            // a group needs a representative AE surely draws (well on screen, visible): otherwise one by one
            if (g[k].copies.size() >= 2 && repArea[k] >= kMinRepArea) {
                clusterOf[k] = (int)lp.clusters.size();
                plan.fb[k ? 2 : 1] += (int)g[k].copies.size();
                lp.clusters.push_back(g[k]);
            } else {
                plan.fb[5] += (int)g[k].copies.size();
            }
        }
        for (int i = 0; i < n; i++) {
            const int s = side[at + i];
            const int k = s > 0 ? 0 : 1;
            const int c = s ? clusterOf[k] : -1;
            MergePlan::DebugCopy dc;
            const It& t = its[at + i];
            dc.layer = li, dc.copy = i, dc.group = c;
            for (int q = 0; q < 4; q++) dc.x[q] = t.q.x[q], dc.y[q] = t.q.y[q];
            dc.zMin = t.q.zMin, dc.zMax = t.q.zMax;
            RectCorners(t.toW, l.srcRect, dc.corners);
            plan.debugCopies.push_back(dc);
            if (c < 0) lp.subs.push_back({i, -1});
            else if (i == rep[k]) lp.subs.push_back({rep[k], c});  // the group: AE draws it as its representative
        }
        plan.layers[MergeLayerKey(l.compItemId, l.layerId)] = std::move(lp);
        at += n;
    }
    return plan;
}

namespace {

constexpr int kUnknown = 2;  // an order not computed yet

// A copy of a merge-on layer.
struct Item {
    int layer, copy;
    double toWorld[16];
    double corners[4][3];
    ScreenQuad q;
    double bb[4];  // q's bounding box
    bool individual = false;
    double pn[3], pd, pcam;  // its plane: unit normal, n . p0, the camera's signed distance
};

void SetPlane(Item& it, const double cam[3])
{
    const double* m = it.toWorld;
    double n[3] = {m[1] * m[6] - m[2] * m[5], m[2] * m[4] - m[0] * m[6], m[0] * m[5] - m[1] * m[4]};
    const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (int k = 0; k < 3; k++) it.pn[k] = l > 0 ? n[k] / l : 0;
    it.pd = it.pn[0] * m[12] + it.pn[1] * m[13] + it.pn[2] * m[14];
    it.pcam = it.pn[0] * cam[0] + it.pn[1] * cam[1] + it.pn[2] * cam[2] - it.pd;
}
// SideOfPlane with the plane precomputed
int SideOf(const Item& p, const double pts[4][3])
{
    if (std::fabs(p.pcam) < 1e-6) return 0;
    int same = 0;
    for (int k = 0; k < 4; k++) {
        const double s = p.pn[0] * pts[k][0] + p.pn[1] * pts[k][1] + p.pn[2] * pts[k][2] - p.pd;
        const double tol = 1e-6 * (1 + std::fabs(pts[k][0] - p.toWorld[12]) + std::fabs(pts[k][1] - p.toWorld[13]) +
                                   std::fabs(pts[k][2] - p.toWorld[14]));
        if (std::fabs(s) < tol) return 0;
        same += (s > 0) == (p.pcam > 0);
    }
    return same == 4 ? 1 : same == 0 ? -1 : 0;
}
int OrderItems(const Item& a, const Item& b)
{
    int s = SideOf(b, a.corners);
    if (s) return s;
    return -SideOf(a, b.corners);
}
// Something a group must keep its order with: another layer's plane, a box, an individual copy.
struct Obstacle {
    const double* toWorld;
    double corners[4][3];
    ScreenQuad q;
    double bb[4];
    bool unbounded, box, cornersKnown;
};
struct Group {
    int layer;
    std::vector<int> items;
    std::map<int, int> sides;  // obstacle -> +1 (the group is in front of it) / -1 (behind)
    double zMin = 1e300, lo = 1, hi = 1e300, d = 0;
};

void BBoxOf(const ScreenQuad& q, double bb[4])
{
    bb[0] = bb[1] = 1e300, bb[2] = bb[3] = -1e300;
    for (int k = 0; k < 4; k++)
        bb[0] = std::min(bb[0], q.x[k]), bb[1] = std::min(bb[1], q.y[k]), bb[2] = std::max(bb[2], q.x[k]), bb[3] = std::max(bb[3], q.y[k]);
}

// The order of two planar quads where they overlap on screen: +1 if a is in front of b, -1 if behind, 0 unknown
// (they cross each other's planes).
int Order(const double aToWorld[16], const double aCorners[4][3], const double bToWorld[16], const double bCorners[4][3],
          const double cam[3])
{
    int s = SideOfPlane(bToWorld, cam, aCorners);  // a on b's camera side: a in front
    if (s) return s;
    s = SideOfPlane(aToWorld, cam, bCorners);  // b on a's camera side: b in front
    return -s;
}

// The plane's camera z range over the screen box `r` (its corners; a plane's depth along rays is extreme at a
// convex region's corners). False if a ray misses the plane.
bool DepthRange(const CameraView& cam, const double planeToWorld[16], const double r[4], double* zMin, double* zMax)
{
    *zMin = 1e300, *zMax = -1e300;
    const double xs[2] = {r[0], r[2]}, ys[2] = {r[1], r[3]};
    for (double x : xs)
        for (double y : ys) {
            double z;
            if (!PlaneDepthAt(cam, planeToWorld, x, y, &z)) return false;
            *zMin = std::min(*zMin, z), *zMax = std::max(*zMax, z);
        }
    return true;
}

// Bounded obstacles by screen cell (their bounding boxes grown by the margin); unbounded ones are always candidates.
class ObstacleGrid {
public:
    ObstacleGrid(const std::vector<Obstacle>& obs, double W, double H, double margin) : obs_(obs), margin_(margin)
    {
        cell_ = std::max(W, H) / 16;
        x0_ = -W, y0_ = -H;
        nx_ = (int)std::ceil(3 * W / cell_) + 1, ny_ = (int)std::ceil(3 * H / cell_) + 1;
        cells_.resize((size_t)nx_ * ny_);
        mark_.assign(obs.size(), -1);
        for (size_t o = 0; o < obs.size(); o++) {
            if (obs[o].unbounded) {
                unbounded_.push_back((int)o);
                continue;
            }
            int a, b, c, d;
            Range(obs[o].bb, &a, &b, &c, &d);
            for (int y = b; y <= d; y++)
                for (int x = a; x <= c; x++) cells_[(size_t)y * nx_ + x].push_back((int)o);
        }
    }
    template <class F>
    void Query(const double bb[4], F f)
    {
        stamp_++;
        for (int o : unbounded_) f(o);
        int a, b, c, d;
        Range(bb, &a, &b, &c, &d);
        for (int y = b; y <= d; y++)
            for (int x = a; x <= c; x++)
                for (int o : cells_[(size_t)y * nx_ + x])
                    if (mark_[o] != stamp_) {
                        mark_[o] = stamp_;
                        f(o);
                    }
    }

private:
    void Range(const double bb[4], int* a, int* b, int* c, int* d) const
    {
        auto cx = [&](double v) { return std::min(nx_ - 1, std::max(0, (int)std::floor((v - x0_) / cell_))); };
        auto cy = [&](double v) { return std::min(ny_ - 1, std::max(0, (int)std::floor((v - y0_) / cell_))); };
        *a = cx(bb[0] - margin_), *c = cx(bb[2] + margin_), *b = cy(bb[1] - margin_), *d = cy(bb[3] + margin_);
    }
    const std::vector<Obstacle>& obs_;
    double margin_, cell_, x0_, y0_;
    int nx_, ny_;
    std::vector<std::vector<int>> cells_;
    std::vector<int> unbounded_, mark_;
    int stamp_ = 0;
};

}  // namespace

MergePlan BuildMergePlan(const MergeSceneInput& in, const MergeOptions& opt)
{
    MergePlan plan;
    plan.cam = in.cam;
    const double cam[3] = {in.cam.camToWorld[12], in.cam.camToWorld[13], in.cam.camToWorld[14]};
    const double W = in.cam.compW, H = in.cam.compH;

    // every copy of every merge-on layer
    std::vector<Item> items;
    std::vector<std::vector<int>> layerItems(in.layers.size());
    for (size_t li = 0; li < in.layers.size(); li++) {
        const MergeLayerInput& l = in.layers[li];
        for (int i = 0; i < std::max(l.p.count, 0); i++) {
            Item it;
            it.layer = (int)li, it.copy = i;
            double op = 0;
            CopyToWorld(l, i, it.toWorld, &op);
            RectCorners(it.toWorld, l.srcRect, it.corners);
            it.q = ProjectRect(in.cam, it.toWorld, l.srcRect);
            if (it.q.valid) BBoxOf(it.q, it.bb);
            // blocked layers, copies at or behind the camera and invisible ones are drawn by AE one by one
            it.individual = l.block != 0 || !it.q.valid || !(op > 0);
            SetPlane(it, cam);
            layerItems[li].push_back((int)items.size());
            items.push_back(it);
        }
    }
    // the fixed obstacles: other layers' planes and boxes
    std::vector<Obstacle> fixed;
    for (const ScenePlane& p : in.planes) {
        Obstacle o;
        o.toWorld = p.toWorld;
        RectCorners(p.toWorld, p.rect, o.corners);
        o.q = ProjectRect(in.cam, p.toWorld, p.rect);
        o.unbounded = p.unbounded || !o.q.valid;
        if (o.q.valid) BBoxOf(o.q, o.bb);
        o.box = false, o.cornersKnown = !p.unbounded;
        fixed.push_back(o);
    }
    for (size_t b = 0; b < in.boxes.size(); b++) {
        Obstacle o = {};
        o.toWorld = nullptr;
        o.unbounded = true, o.box = true, o.cornersKnown = false;
        fixed.push_back(o);
    }

    // A copy and another surface that overlap on screen and each straddle the other's plane without meeting in 3D
    // cannot be put in one depth order without cutting one of them. AE's classic 3D (which cuts only surfaces that
    // really intersect) then orders its surfaces its own way, and that order changes with the set of surfaces it
    // draws (a user scene, 11/25: two copies AE drew itself swapped when other copies were merged; with the other
    // surface away AE's order was the depth order and merging matched). Such a frame does not merge: AE draws
    // everything as without the merge. (Copies that pierce a surface are cut by AE exactly: they are only drawn
    // one by one, below.)
    for (const Item& it : items) {
        if (!it.q.valid) continue;
        for (const Obstacle& ob : fixed) {
            if (ob.box || !ob.cornersKnown || ob.unbounded || !QuadsOverlap(it.q, ob.q, opt.marginPx)) continue;
            if (!SideOfPlane(ob.toWorld, cam, it.corners) && !SideOfPlane(it.toWorld, cam, ob.corners) &&
                !QuadsIntersect3D(it.corners, ob.corners)) {
                plan.ambiguous = true;
                break;
            }
        }
        if (plan.ambiguous) break;
    }
    if (plan.ambiguous) {
        for (Item& it : items) it.individual = true;
    }

    // Once: every copy against the fixed obstacles, and the order of every overlapping pair of copies (copies that
    // become individual are obstacles for the others: their orders come from these pairs). Obstacle index o:
    // o < F a fixed obstacle, o >= F the individual copy o - F.
    const int F = (int)fixed.size();
    auto t0 = std::chrono::steady_clock::now();
    auto lap = [&](int k) {
        auto t1 = std::chrono::steady_clock::now();
        plan.ms[k] += std::chrono::duration<double, std::milli>(t1 - t0).count();
        t0 = t1;
    };
    std::vector<std::vector<std::pair<int, int>>> fixedSides(items.size()), allNb(items.size());
    {
        ObstacleGrid grid(fixed, W, H, opt.marginPx);
        for (size_t c = 0; c < items.size(); c++) {
            Item& it = items[c];
            if (it.individual) continue;
            grid.Query(it.bb, [&](int o) {
                if (it.individual) return;
                const Obstacle& ob = fixed[o];
                if (!ob.unbounded && !QuadsOverlap(it.q, ob.q, opt.marginPx)) return;
                int s = 0;
                if (!ob.box) s = ob.cornersKnown ? Order(it.toWorld, it.corners, ob.toWorld, ob.corners, cam)
                                                 : SideOfPlane(ob.toWorld, cam, it.corners);
                if (!s) it.individual = true;  // pierces it (or a box): drawn by AE
                else fixedSides[c].push_back({o, s});
            });
        }
    }
    const auto start = std::chrono::steady_clock::now();
    auto overBudget = [&] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > opt.budgetMs;
    };
    // nothing to keep an order with (one merge-on layer, no other surface, no individual copy): one group, no pairs
    bool trivial = in.layers.size() == 1 && in.planes.empty() && in.boxes.empty();
    for (const Item& it : items) trivial = trivial && !it.individual;
    if (!trivial) {
        std::vector<int> byX, far;  // copies on the screen; copies at / behind the camera (unbounded)
        for (size_t c = 0; c < items.size(); c++) (items[c].q.valid ? byX : far).push_back((int)c);
        std::sort(byX.begin(), byX.end(), [&](int a, int b) { return items[a].bb[0] < items[b].bb[0]; });
        for (size_t i = 0; i < byX.size(); i++) {
            if ((i & 255) == 255 && overBudget()) break;  // the rounds stop at once
            const Item& a = items[byX[i]];
            for (size_t j = i + 1; j < byX.size(); j++) {
                const Item& b = items[byX[j]];
                if (b.bb[0] > a.bb[2] + 2 * opt.marginPx) break;
                if (b.bb[1] > a.bb[3] + 2 * opt.marginPx || a.bb[1] > b.bb[3] + 2 * opt.marginPx) continue;
                if (!QuadsOverlap(a.q, b.q, opt.marginPx)) continue;
                allNb[byX[i]].push_back({byX[j], kUnknown});  // the order is computed when it is needed
                allNb[byX[j]].push_back({byX[i], kUnknown});
            }
        }
        for (int u : far)
            for (int c : byX) allNb[c].push_back({u, kUnknown});
    }
    // the order of c with its k-th neighbour (+1: c in front), computed once
    auto ord = [&](int c, int k) {
        auto& e = allNb[c][k];
        if (e.second == kUnknown) e.second = OrderItems(items[c], items[e.first]);
        return e.second;
    };
    lap(0);

    std::vector<Group> groups;
    std::vector<int> groupOf(items.size(), -1);
    bool done = false;
    for (int round = 0; round < opt.maxRounds && !done; round++) {
        plan.rounds = round + 1;
        if (overBudget()) {
            plan.overBudget = true;
            break;
        }
        // each candidate's side of every obstacle it overlaps; an individual copy it cannot be ordered with -> individual
        bool changed = false;
        std::vector<std::vector<std::pair<int, int>>> sides(items.size());
        std::vector<std::vector<int>> nb(items.size());  // indices into allNb[c] of neighbours still candidates
        std::vector<int> cand;
        for (size_t c = 0; c < items.size(); c++) {
            if (items[c].individual) continue;
            sides[c] = fixedSides[c];
            for (size_t k = 0; k < allNb[c].size(); k++) {
                const int n = allNb[c][k].first;
                if (!items[n].individual) {
                    nb[c].push_back((int)k);
                    continue;
                }
                const int o = ord((int)c, (int)k);
                if (!o) {
                    items[c].individual = true;
                    changed = true;
                    break;
                }
                sides[c].push_back({F + n, o});
            }
            if (!items[c].individual) cand.push_back((int)c);
        }
        if (changed) continue;
        std::sort(cand.begin(), cand.end(), [&](int a, int b) { return items[a].q.zMin < items[b].q.zMin; });
        lap(1);
        // greedy grouping, nearest first. The groups' order (front[g]: the groups g is in front of) must stay acyclic
        // and agree with every overlapping pair of copies of two groups.
        groups.clear();
        std::fill(groupOf.begin(), groupOf.end(), -1);
        std::vector<std::map<int, int>> rel;  // rel[g][h]: +1 g in front of h, -1 behind
        std::vector<int> visit;
        int visitStamp = 0;
        auto reaches = [&](int from, int to) {  // a chain of "in front of" from `from` to `to`
            if (from == to) return true;
            visitStamp++;
            std::vector<int> stack{from};
            visit[from] = visitStamp;
            while (!stack.empty()) {
                const int g = stack.back();
                stack.pop_back();
                for (auto& r : rel[g])
                    if (r.second > 0 && visit[r.first] != visitStamp) {
                        if (r.first == to) return true;
                        visit[r.first] = visitStamp;
                        stack.push_back(r.first);
                    }
            }
            return false;
        };
        // the orders c (as a member of g; g = -1: a new group) would add; false if they conflict or make a cycle
        auto fits = [&](int c, int g, std::map<int, int>* adds) {
            adds->clear();
            if (g >= 0)
                for (auto& so : sides[c]) {
                    auto f = groups[g].sides.find(so.first);
                    if (f != groups[g].sides.end() && f->second != so.second) return false;
                }
            for (int k : nb[c]) {
                const int h = groupOf[allNb[c][k].first];
                if (h < 0 || h == g) continue;
                const int o = ord(c, k);
                if (!o) return false;
                auto a = adds->find(h);
                if (a != adds->end() && a->second != o) return false;
                if (g >= 0) {
                    auto r = rel[g].find(h);
                    if (r != rel[g].end() && r->second != o) return false;
                }
                (*adds)[h] = o;
            }
            // no cycle: for g in front of h (g -> h) nothing may lead from h back to g, and for a new group
            // nothing in front of it may be reachable from what it is in front of
            for (auto& a : *adds) {
                if (g >= 0) {
                    if (a.second > 0 ? reaches(a.first, g) : reaches(g, a.first)) return false;
                } else if (a.second > 0) {
                    for (auto& b : *adds)
                        if (b.second < 0 && reaches(a.first, b.first)) return false;
                }
            }
            return true;
        };
        std::vector<std::vector<int>> recent(in.layers.size());  // the last groups made per layer
        std::map<int, int> adds;
        bool aborted = false;
        int visited = 0;
        for (int c : cand) {
            if ((++visited & 255) == 0 && overBudget()) {
                aborted = true;
                break;
            }
            const Item& it = items[c];
            // candidates: the groups of the copies it overlaps, and the layer's latest groups
            std::vector<int> tries;
            for (int k : nb[c]) {
                const int h = groupOf[allNb[c][k].first];
                if (h >= 0 && groups[h].layer == it.layer && std::find(tries.begin(), tries.end(), h) == tries.end()) tries.push_back(h);
            }
            for (int h : recent[it.layer])
                if (std::find(tries.begin(), tries.end(), h) == tries.end()) tries.push_back(h);
            int chosen = -1;
            for (int g : tries)
                if (fits(c, g, &adds)) {
                    chosen = g;
                    break;
                }
            if (chosen < 0) {
                if (!fits(c, -1, &adds)) {
                    items[c].individual = true;
                    changed = true;
                    continue;
                }
                Group ng;
                ng.layer = it.layer;
                groups.push_back(ng);
                rel.emplace_back();
                visit.push_back(0);
                chosen = (int)groups.size() - 1;
                recent[it.layer].push_back(chosen);
                if (recent[it.layer].size() > 3) recent[it.layer].erase(recent[it.layer].begin());
            }
            Group& g = groups[chosen];
            g.items.push_back(c);
            groupOf[c] = chosen;
            g.zMin = std::min(g.zMin, it.q.zMin);
            for (auto& so : sides[c]) g.sides[so.first] = so.second;
            for (auto& a : adds) {
                rel[chosen][a.first] = a.second;
                rel[a.first][chosen] = -a.second;
            }
        }
        if (aborted) {
            plan.overBudget = true;
            break;
        }
        if (changed) continue;
        // groups of one copy are drawn as that copy
        for (Group& g : groups)
            if (g.items.size() < 2) {
                items[g.items[0]].individual = true;
                changed = true;
            }
        if (changed) continue;

        lap(2);
        // each group's depth range from its obstacles, where its copies overlap them
        std::vector<char> badGroup(groups.size(), 0);
        int bad = -1;
        for (size_t gi = 0; gi < groups.size(); gi++) {
            Group& g = groups[gi];
            g.lo = 1, g.hi = 1e300;
            for (int c : g.items)
                for (auto& so : sides[c]) {
                    const bool isFixed = so.first < F;
                    const Item* oi = isFixed ? nullptr : &items[so.first - F];
                    const double* obToWorld = isFixed ? fixed[so.first].toWorld : oi->toWorld;
                    const double* obb = isFixed ? fixed[so.first].bb : oi->bb;
                    const bool unb = isFixed ? fixed[so.first].unbounded : !oi->q.valid;
                    double r[4] = {items[c].bb[0], items[c].bb[1], items[c].bb[2], items[c].bb[3]};
                    if (!unb)
                        r[0] = std::max(r[0], obb[0]), r[1] = std::max(r[1], obb[1]), r[2] = std::min(r[2], obb[2]),
                        r[3] = std::min(r[3], obb[3]);
                    r[0] = std::max(r[0], -opt.marginPx), r[1] = std::max(r[1], -opt.marginPx);
                    r[2] = std::min(r[2], W + opt.marginPx), r[3] = std::min(r[3], H + opt.marginPx);
                    if (!(r[2] >= r[0]) || !(r[3] >= r[1])) continue;  // the overlap is off the screen
                    double z0, z1;
                    if (!DepthRange(in.cam, obToWorld, r, &z0, &z1)) {
                        if (so.second < 0) badGroup[gi] = 1;  // behind something whose depth is unbounded there
                        continue;
                    }
                    if (so.second > 0) g.hi = std::min(g.hi, z0);
                    else g.lo = std::max(g.lo, z1);
                }
        }
        // depths in front-to-back order of the groups (acyclic by construction), inside each group's range
        {
            const int ng = (int)groups.size();
            std::vector<int> indeg(ng, 0), order;
            for (int g = 0; g < ng; g++)
                for (auto& r : rel[g])
                    if (r.second > 0) indeg[r.first]++;
            for (int g = 0; g < ng; g++)
                if (!indeg[g]) order.push_back(g);
            for (size_t k = 0; k < order.size(); k++)
                for (auto& r : rel[order[k]])
                    if (r.second > 0 && --indeg[r.first] == 0) order.push_back(r.first);
            const double eps = 1e-3;
            std::vector<double> lo(ng);
            for (int g = 0; g < ng; g++) lo[g] = groups[g].lo;
            for (int g : order) {
                Group& G = groups[g];
                if (badGroup[g] || !(lo[g] + 2 * eps < G.hi)) {
                    badGroup[g] = 1;
                    continue;
                }
                G.d = std::max(lo[g] + eps, std::min(G.zMin, G.hi - eps));
                for (auto& r : rel[g])
                    if (r.second > 0) lo[r.first] = std::max(lo[r.first], G.d);
            }
            for (int g = 0; g < ng; g++)
                if ((int)order.size() < ng && indeg[g]) badGroup[g] = 1;  // (a cycle cannot happen; be safe)
        }
        for (size_t g = 0; g < groups.size(); g++)
            if (badGroup[g]) {
                for (int c : groups[g].items) items[c].individual = true;
                bad = (int)g;
            }
        if (bad >= 0) continue;
        lap(3);
        // the textures' area
        std::vector<double> rects(groups.size() * 4);
        double area = 0;
        int largest = -1;
        double largestArea = 0;
        for (size_t g = 0; g < groups.size(); g++) {
            std::vector<ScreenQuad> qs;
            for (int c : groups[g].items) qs.push_back(items[c].q);
            if (!AlignedRect(qs, W, H, &rects[g * 4])) {
                bad = (int)g;  // all off the screen: AE draws them (as nothing)
                break;
            }
            const double a = (rects[g * 4 + 2] - rects[g * 4]) * (rects[g * 4 + 3] - rects[g * 4 + 1]);
            area += a;
            if (a > largestArea) largestArea = a, largest = (int)g;
        }
        if (bad < 0 && area > opt.maxAreaScreens * W * H) bad = largest;
        if (bad >= 0) {
            for (int c : groups[bad].items) items[c].individual = true;
            continue;
        }
        done = true;
    }

    for (size_t c = 0; c < items.size(); c++) {
        const Item& it = items[c];
        MergePlan::DebugCopy dc;
        dc.layer = it.layer, dc.copy = it.copy;
        dc.group = done && !it.individual ? groupOf[c] : -1;
        for (int k = 0; k < 4; k++) {
            dc.x[k] = it.q.x[k], dc.y[k] = it.q.y[k];
            for (int j = 0; j < 3; j++) dc.corners[k][j] = it.corners[k][j];
        }
        dc.zMin = it.q.zMin, dc.zMax = it.q.zMax;
        plan.debugCopies.push_back(dc);
    }
    // the layers' plans
    for (size_t li = 0; li < in.layers.size(); li++) {
        MergeLayerPlan lp;
        lp.in = in.layers[li];
        std::map<int, int> clusterOfGroup;
        if (done) {
            for (size_t g = 0; g < groups.size(); g++) {
                if (groups[g].layer != (int)li) continue;
                MergeCluster cl;
                std::vector<ScreenQuad> qs;
                for (int c : groups[g].items) cl.copies.push_back(items[c].copy), qs.push_back(items[c].q);
                std::sort(cl.copies.begin(), cl.copies.end());
                cl.depth = groups[g].d;
                AlignedRect(qs, W, H, cl.rect);
                clusterOfGroup[(int)g] = (int)lp.clusters.size();
                lp.clusters.push_back(cl);
            }
        }
        for (int c : layerItems[li]) {
            const int g = done && !items[c].individual ? groupOf[c] : -1;
            if (g < 0) {
                lp.subs.push_back({items[c].copy, -1});
                continue;
            }
            const MergeCluster& cl = lp.clusters[clusterOfGroup[g]];
            if (items[c].copy == cl.copies[0]) lp.subs.push_back({-1, clusterOfGroup[g]});
        }
        plan.layers[MergeLayerKey(lp.in.compItemId, lp.in.layerId)] = std::move(lp);
    }
    return plan;
}

unsigned long long HashMergeScene(const MergeSceneInput& in)
{
    unsigned long long h = 1469598103934665603ULL;
    auto mix = [&h](const void* p, size_t n) {
        const unsigned char* b = (const unsigned char*)p;
        for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ULL;
    };
    auto d = [&mix](double v) { mix(&v, sizeof(v)); };
    auto i32 = [&mix](int v) { mix(&v, sizeof(v)); };
    for (double v : in.cam.camToWorld) d(v);
    d(in.cam.dist), d(in.cam.compW), d(in.cam.compH);
    for (const MergeLayerInput& l : in.layers) {
        i32(l.compItemId), i32(l.layerId), i32(l.p.count), i32(l.p.seed), i32(l.p.keepFirst), i32((int)l.block);
        for (int k = 0; k < 3; k++) d(l.p.pos[k]), d(l.p.rotDeg[k]), d(l.world.pos[k]), d(l.world.rotDeg[k]), d(l.world.scale[k]);
        d(l.p.scaleMin), d(l.p.scaleMax), d(l.p.opacityMin), d(l.p.opacityMax), d(l.anchorX), d(l.anchorY);
        for (double v : l.layerToWorld) d(v);
        for (double v : l.srcRect) d(v);
    }
    for (const ScenePlane& p : in.planes) {
        for (double v : p.toWorld) d(v);
        for (double v : p.rect) d(v);
        i32(p.unbounded);
    }
    for (const SceneBox& b : in.boxes) {
        for (int k = 0; k < 3; k++) d(b.minW[k]), d(b.maxW[k]);
        i32(b.unbounded);
    }
    return h;
}

struct MergePlanCache::Entry {
    unsigned long long hash;
    std::shared_future<std::shared_ptr<const MergePlan>> plan;
};

std::shared_ptr<const MergePlan> MergePlanCache::Get(long long key, unsigned long long hash,
                                                    const std::function<MergePlan()>& build)
{
    std::promise<std::shared_ptr<const MergePlan>> made;
    std::shared_future<std::shared_ptr<const MergePlan>> other;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = entries_.find(key);
        if (it != entries_.end() && it->second->hash == hash) {
            other = it->second->plan;
        } else {
            if (entries_.size() > 512) entries_.clear();  // many frames seen: keep memory bounded
            auto e = std::make_shared<Entry>();
            e->hash = hash;
            e->plan = made.get_future().share();
            entries_[key] = e;
        }
    }
    if (other.valid()) return other.get();  // built (or being built) by another caller: wait for it
    std::shared_ptr<const MergePlan> p;
    try {
        p = std::make_shared<const MergePlan>(build());
    } catch (...) {
        p = nullptr;  // the callers draw without merging
    }
    made.set_value(p);
    return p;
}

std::shared_ptr<const MergePlan> MergePlanCache::Latest(long long key) const
{
    std::shared_future<std::shared_ptr<const MergePlan>> f;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = entries_.find(key);
        if (it == entries_.end()) return nullptr;
        f = it->second->plan;
    }
    return f.get();
}

unsigned MergeBlockReasons(const LayerFlagsForMerge& f)
{
    unsigned b = MB_NONE;
    if (f.compMotionBlur && f.layerMotionBlur) b |= MB_MOTION_BLUR;
    if (f.dof) b |= MB_DOF;
    if (f.acceptsLights && f.anyLight) b |= MB_LIGHTS;
    if (f.castsShadows && f.anyShadowLight) b |= MB_SHADOWS;
    if (!f.normalBlend) b |= MB_BLEND;
    if (!f.fullOpacity) b |= MB_OPACITY;
    if (f.trackMatte) b |= MB_MATTE;
    if (f.otherEffects) b |= MB_OTHER_EFFECTS;
    if (f.solid) b |= MB_SOLID;
    return b;
}

void DefaultCamera(double compW, double compH, double dist, CameraView* out)
{
    const double T[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, compW / 2, compH / 2, -dist, 1};
    for (int k = 0; k < 16; k++) out->camToWorld[k] = T[k];
    out->dist = dist;
    out->compW = compW;
    out->compH = compH;
}

}  // namespace rnd
