// SPIKE (RANDAMU_SPIKE_SCREEN=1): one sub-layer plane that faces the camera and maps layer pixel
// (x, y) onto comp pixel (x, y), to test the "single image" fast mode (spec 12.7). The effect computes
// the plane's M4 in PreRender (where it may read the camera) and the hooks use it.
#pragma once

namespace rnd {

bool SpikeScreenEnabled();
// m4: row-vector 4x4 (16 doubles), layer px -> layer px; set by PreRender for a layer key.
void SpikeSetPlane(int compItemId, int layerId, const double m4[16]);
bool SpikeGetPlane(int compItemId, int layerId, double m4[16]);

}  // namespace rnd
