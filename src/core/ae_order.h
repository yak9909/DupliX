// DupliX core: how After Effects' classic 3D renderer (Advanced3D.aex, "Escher") orders the surfaces of a frame,
// rebuilt from its code (docs/experiments/2026-11-25-dev-log.md, 11/26): for every pair of surfaces whose screen
// quads overlap, the depth difference at the overlap's corners gives "a is in front somewhere" / "b is in front
// somewhere"; the resulting graph is split into strongly connected components. Single surfaces are painted in that
// order (exactly by depth); a component of several surfaces (a cycle: they cross, or straddle each other's planes)
// is drawn through a per-pixel fragment buffer whose result depends on the order AE happened to visit them, which
// changes with every other surface of the frame. No After Effects dependency.
#pragma once

#include <vector>

#include "core/fast_plane.h"

namespace rnd {

// One surface: its quad on the screen (full-resolution comp px, corners in order) and its world corners.
struct AeSurface {
    double sx[4], sy[4];
    double w[4][3];
};

struct AeOrderResult {
    int cycleSurfaces = 0;  // surfaces in components of two or more (AE draws them through its fragment buffer)
    int ties = 0;           // overlapping pairs with no depth difference (AE's order between them is arbitrary)
    std::vector<int> inCycle;  // per surface: 1 if it is in such a component
};

// The order graph of `s` seen by `cam`. eps: relative depth difference below which two surfaces are level.
AeOrderResult AeOrderGraph(const CameraView& cam, const std::vector<AeSurface>& s, double eps = 1e-9);

// A camera-facing rectangle (screen rect l, t, r, b in full-resolution comp px) at camera depth d (the depth unit of
// ProjectRect / the plan) as a surface.
AeSurface CameraFacingSurface(const CameraView& cam, const double rect[4], double d);

}  // namespace rnd
