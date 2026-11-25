// randamu core: the merge optimisation (normal mode, spec docs/superpowers/specs/2026-11-25-merge-optimization-design.md).
// The data of one frame of a comp, the whole-layer reasons not to merge, and the plan (which copies AE draws one
// by one, which are drawn by DupliX into a group plane). No After Effects dependency.
// Matrices are row vectors (p' = p * M), row 3 = translation, like AEGP.
#pragma once

#include <map>
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
};

struct LayerFlagsForMerge {
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
};

struct MergeSceneInput {
    CameraView cam;
    std::vector<MergeLayerInput> layers;  // the comp's DupliX layers with merge on
    std::vector<ScenePlane> planes;       // other 3D layers, and every copy of DupliX layers with merge off
    std::vector<SceneBox> boxes;
};

// The camera AE uses when a comp has none: at (w/2, h/2, -dist) looking down +z, no rotation (AE's default).
void DefaultCamera(double compW, double compH, double dist, CameraView* out);

}  // namespace rnd
