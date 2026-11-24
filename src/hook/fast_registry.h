// The fast mode's plane (sub-layer M4) per layer and layer time, set by the effect's PreRender and read
// by the Rects hook. The plane depends on the camera and the layer's transform, so it is kept per time;
// each layer keeps its 64 most recent times (MFR renders several frames at once).
#pragma once

#include "hook/registry.h"

namespace rnd {

// t = tValue / tScale (layer time); times are compared as fractions (10/30 == 20/60).
void FastSetPlane(LayerKey k, long long tValue, unsigned long long tScale, const double m4[16]);
// The plane at exactly that time (*exact = true), else the one at the nearest registered time
// (*exact = false). False if the layer has none.
bool FastGetPlane(LayerKey k, long long tValue, unsigned long long tScale, double m4[16], bool* exact);
void FastClear(LayerKey k);

}  // namespace rnd
