// randamu: the texture of a group plane (spec docs/superpowers/specs/2026-11-25-merge-optimization-design.md 5.3).
#pragma once

#include "AE_GeneralPlug.h"
#include "core/merge_plan.h"

namespace rnd {

// Draws group `c` of layer `lp` from the layer texture AE gave (layerTex, its matrix texM3: texture px -> layer px)
// into a new AEGP world of the same pixel type covering the group's screen rect at the texture's resolution.
// *outM3: the new world's texture matrix (it spans the same layer rect as layerTex). Null on failure.
// The caller hands the world to AE (which deletes it, like every texture) and deletes layerTex.
// dsfX/dsfY: the render's downsample factor (1/n; 0 = unknown: estimated from the texture's size, which is
// rounded up by AE when the layer's size is not a multiple of n). With it the group covers c.rect grown to
// whole render pixels (AlignRectToDownsample), as the Rects hook places it.
AEGP_WorldH RenderGroupTexture(AEGP_WorldH layerTex, const double texM3[9], const MergePlan& plan, const MergeLayerPlan& lp,
                               const MergeCluster& c, int dsfX, int dsfY, double outM3[9]);

}  // namespace rnd
