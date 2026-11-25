#include "core/merge_plan.h"

namespace rnd {

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
