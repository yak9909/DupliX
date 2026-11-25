// randamu core: geometry of the merge optimisation (spec docs/superpowers/specs/2026-11-25-merge-optimization-design.md
// 5.4, 5.5). No After Effects dependency. Matrices are row vectors (p' = p * M), row 3 = translation.
#pragma once

#include "core/fast_plane.h"

namespace rnd {

// A rectangle (layer px) of a plane seen by the camera: its corners in full-resolution comp px and its
// camera-space z range. valid = false when a corner is at camera-space z <= 1 (at or behind the camera).
struct ScreenQuad {
    double x[4], y[4];
    double zMin, zMax;
    bool valid;
};
ScreenQuad ProjectRect(const CameraView& cam, const double toWorld[16], const double rect[4]);

// The sub-layer matrix of a group plane: the layer's texture rect texRect (layer px, as AE clips the texture)
// facing the camera at camera-space depth d, covering the screen rect `rect` (full-resolution comp px,
// left, top, right, bottom). layerToWorld: the layer's own transform (AE multiplies it after the sub-layer's).
bool GroupPlaneM4(const CameraView& cam, const double layerToWorld[16], double d, const double texRect[4],
                  const double rect[4], double out[16]);

// The texture matrix (texture px -> layer px, 3x3) of a group's world of worldW x worldH px spread over texRect.
void GroupTextureM3(const double texRect[4], int worldW, int worldH, double out[9]);

// The texture rect AE clips to: the layer px covered by a texture of w x h px under its matrix m3.
void TextureLayerRect(const double m3[9], int w, int h, double out[4]);

}  // namespace rnd
