#include "core/merge_plan.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>

#include "core/merge_geom.h"

namespace rnd {

void CopyToWorld(const MergeLayerInput& l, int i, double toWorld[16], double* opacity)
{
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
