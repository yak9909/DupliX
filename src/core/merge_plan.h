// randamu core: the merge optimisation (normal mode, spec docs/superpowers/specs/2026-11-25-merge-optimization-design.md).
// The data of one frame of a comp, the whole-layer reasons not to merge, and the plan (which copies AE draws one
// by one, which are drawn by DupliX into a group plane). No After Effects dependency.
// Matrices are row vectors (p' = p * M), row 3 = translation, like AEGP.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "core/fast_plane.h"
#include "core/scatter.h"
#include "core/world.h"

namespace rnd {

// Whole-layer reasons not to merge (the status line and the log).
enum MergeBlock : unsigned {
    MB_NONE = 0,
    MB_MOTION_BLUR = 1,
    MB_DOF = 2,
    MB_LIGHTS = 4,
    MB_SHADOWS = 8,
    MB_BLEND = 16,
    MB_OPACITY = 32,
    MB_MATTE = 64,
    MB_UNREADABLE = 128,
    MB_OTHER_EFFECTS = 256,  // other active effects on the layer may grow each copy beyond the rect the plan knows
    MB_SOLID = 512,          // AE fills a solid's copies without asking for a texture: a group cannot be swapped in
};

struct LayerFlagsForMerge {
    bool solid = false;
    bool otherEffects = false;
    bool compMotionBlur = false, layerMotionBlur = false;
    bool dof = false;
    bool acceptsLights = false, anyLight = false;
    bool castsShadows = false, anyShadowLight = false;
    bool normalBlend = true, fullOpacity = true, trackMatte = false;
};
unsigned MergeBlockReasons(const LayerFlagsForMerge& f);

// Another surface: a layer's rectangle in world space.
struct ScenePlane {
    double toWorld[16];  // layer px -> world (comp px)
    double rect[4];      // left, top, right, bottom (layer px)
    bool unbounded;      // extent unknown (effects that may grow it): the whole infinite plane
};
// A non-planar thing (collapsed 3D precomp, 3D text that may be per-character 3D). unbounded: its extent is
// unknown (it covers the whole screen at every depth).
struct SceneBox {
    double minW[3], maxW[3];
    bool unbounded;
};

// One DupliX layer with merge on.
struct MergeLayerInput {
    int compItemId = 0, layerId = 0;
    ScatterParams p;
    WorldXform world;
    double anchorX = 0, anchorY = 0;
    double layerToWorld[16];
    double srcRect[4];   // the layer's own rect (layer px), as the copies are drawn
    unsigned block = 0;  // MergeBlock reasons (0: may merge)
    // Unit tests only: when not empty, copy i's matrix layer px -> world is explicitCopies[16 i .. 16 i + 15]
    // (opacity 1) instead of the scatter parameters; p.count must be its size / 16.
    std::vector<double> explicitCopies;
};

struct MergeSceneInput {
    CameraView cam;
    std::vector<MergeLayerInput> layers;  // the comp's DupliX layers with merge on
    std::vector<ScenePlane> planes;       // other 3D layers, and every copy of DupliX layers with merge off
    std::vector<SceneBox> boxes;
};

// The camera AE uses when a comp has none: at (w/2, h/2, -dist) looking down +z, no rotation (AE's default).
void DefaultCamera(double compW, double compH, double dist, CameraView* out);

// Copy i of a layer: its matrix layer px -> world (copy * World Transform * layer-to-world) and its opacity (0..1).
void CopyToWorld(const MergeLayerInput& l, int i, double toWorld[16], double* opacity);

// ---- the plan

// Group rects are aligned to this many comp px, so that they are whole buffer px at every AE downsample factor
// (1/1, 1/2, 1/3, 1/4, 1/6, 1/8).
constexpr double kMergeAlign = 24;

struct MergeCluster {
    std::vector<int> copies;  // copy indices of the layer, in their order
    double depth;             // camera-space z of the group plane (d)
    double rect[4];           // the group's screen rect: full-resolution comp px, aligned to kMergeAlign
    // the group drawing: where the group is drawn. 0 at its representative's place in AE's order; +1 after every
    // surface of the frame (a front group); -1 before every surface (a back group)
    int place = 0;
};
// One sub-layer AE draws: an individual copy (copy >= 0) or a group plane (cluster >= 0).
struct MergeSub {
    int copy;
    int cluster;
};
struct MergeLayerPlan {
    MergeLayerInput in;
    std::vector<MergeSub> subs;
    std::vector<MergeCluster> clusters;
};
struct MergePlan {
    int rounds = 0;  // BuildMergePlan's rounds (diagnostics)
    bool overBudget = false;  // the budget ran out: nothing merges this frame
    bool ambiguous = false;   // a copy and another surface straddle each other's planes: nothing merges this frame
    double ms[4] = {0, 0, 0, 0};  // diagnostics: time in sides / neighbours / grouping / depths
    // diagnostics (BuildFrontBackGroupPlan): copies that may be grouped, in front groups, in back groups, and the
    // others: with an unsure pair (margin band, sliver, crossing, level), between surfaces (something in front and
    // something behind), alone in their group (a group of one is drawn one by one); then the unsure pairs of two
    // copies and of a copy and another surface, each by its reason: a sliver, no depth there, level or crossing;
    // [12] 1 when the budget ran out (the plan is BuildIsolatedGroupPlan's); [13] (diagnostics, RANDAMU_PLAN_MEETS=1)
    // BuildIsolatedGroupPlan's overlapping pairs of grouped copies that meet in 3D
    int fb[14] = {};
    CameraView cam;
    std::map<long long, MergeLayerPlan> layers;  // MergeLayerKey(comp, layer)
    // diagnostics (RANDAMU_MERGE_DUMP): every copy's screen quad, its world corners and its group (-1: one by one)
    struct DebugCopy {
        int layer, copy, group;
        double x[4], y[4], zMin, zMax;
        double corners[4][3];
    };
    std::vector<DebugCopy> debugCopies;
};
inline long long MergeLayerKey(int compItemId, int layerId) { return ((long long)compItemId << 32) | (unsigned)layerId; }

// Task 3 (interim): every copy of a layer that may merge (block 0, in front of the camera, opacity > 0) in one group
// at the depth of its nearest copy; the others one by one. A group of one copy is drawn as that copy.
MergePlan BuildSingleGroupPlan(const MergeSceneInput& in);

// The group drawing (11/26, spec: HANDOFF 11/26 12 時): copies of a layer whose screen quads (grown by marginPx)
// overlap no other surface (other layers, copies of other layers) and only copies that do the same form the
// layer's group; the others are drawn one by one. The group is one sub-layer: its representative copy (the largest
// on screen), with MergeSub {rep, 0}. At its draw, DupliX builds every group copy's surface with AE's own functions
// and has AE order and draw them (as AE would if they were sub-layers): nothing else overlaps them, so the result is
// AE's. A layer with a copy that cannot be placed on screen (behind the camera) is drawn one by one.
MergePlan BuildIsolatedGroupPlan(const MergeSceneInput& in, double marginPx = 3);

// The front and back groups (11/26): a layer's front group is its copies each of which is clearly in front of every
// surface it overlaps on screen outside the group (other layers, other layers' copies and groups, the layer's
// copies drawn one by one); its back group likewise clearly behind. A front group is drawn after every surface of
// the frame, a back group before every surface (MergeCluster::place): what each copy overlaps is then drawn on the
// same side of it as in AE's order. "Clearly": the quads overlap by more than a sliver and, at every corner of the
// overlap of the quads grown by marginPx, the depths differ in the same direction (1/z is linear on the screen for a
// plane, so the corners decide the whole overlap). Pairs inside a group are clear too (AE's order between them is
// then its graph's edge, the same in the group's own list as in the frame's). Copies overlapping nothing join the
// front group. The same exclusions as BuildIsolatedGroupPlan (boxes, unbounded planes, behind the camera).
// Past budgetMs of pair judgments (very dense scenes) it returns BuildIsolatedGroupPlan's plan with fb[12] = 1.
MergePlan BuildFrontBackGroupPlan(const MergeSceneInput& in, double marginPx = 3, double budgetMs = 40);

struct MergeOptions {
    double marginPx = 3;          // screen overlap tests grow every quad by this much (edge filtering)
    double maxAreaScreens = 2;    // the groups' textures together cover at most this many comp screens
    int maxRounds = 40;           // rounds of "a conflict makes copies individual, plan again"
    // past this the frame does not merge (every copy one by one): a bounded cost. Generous: AE drawing the copies one
    // by one costs far more (about 14 ms a copy in the user's scene), and a frame that merges only sometimes would
    // flicker by the 1-2 levels merged copies differ.
    double budgetMs = 200;
};
// The plan (spec 5.5, 5.9; Task 4: orders are judged where the copies' own quads are). Every group: copies of one
// layer that, against every surface overlapping them on screen (other 3D layers, individual copies, copies of
// other groups), are all on the same side of it; its plane's depth d keeps every such order where its copies are
// (in front of what it is in front of, behind the rest), and groups overlapping each other are ordered by d like
// their contents. Copies that cannot be grouped are drawn one by one.
MergePlan BuildMergePlan(const MergeSceneInput& in, const MergeOptions& opt = MergeOptions());

// A hash of everything a plan depends on (camera, layers, parameters, surfaces).
unsigned long long HashMergeScene(const MergeSceneInput& in);

// Plans by key (e.g. comp and comp time). Get builds the plan once per (key, hash): concurrent callers wait for
// the first one's result. A different hash replaces the entry. Thread-safe.
class MergePlanCache {
public:
    std::shared_ptr<const MergePlan> Get(long long key, unsigned long long hash, const std::function<MergePlan()>& build);
    // The plan last stored under key (null if none).
    std::shared_ptr<const MergePlan> Latest(long long key) const;

private:
    struct Entry;
    mutable std::mutex mu_;
    std::map<long long, std::shared_ptr<Entry>> entries_;
};

}  // namespace rnd
