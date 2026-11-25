#include "core/merge_plan.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>

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
