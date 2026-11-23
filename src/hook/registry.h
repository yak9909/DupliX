// Layers that carry the randamu effect, keyed by (comp item ID, layer ID), with the last parameter
// snapshot read by the effect itself. Lookups are lock-free (called from BEE virtuals on any thread).
#pragma once

#include "core/scatter.h"

namespace rnd {

struct LayerKey {
    int compItemId;
    int layerId;
};

struct ParamSnapshot {
    ScatterParams p;
    double anchorX = 0, anchorY = 0;  // layer anchor point (layer space px)
    bool valid = false;
};

void RegRegister(LayerKey k, const ParamSnapshot& snap);  // add, or replace the snapshot
void RegUnregister(LayerKey k);
bool RegLookup(LayerKey k, ParamSnapshot* out);
bool RegContains(LayerKey k);

}  // namespace rnd
