// Fast mode switched on / off by the user (spec 12.13, plan 2026-11-25 Task 5). Main thread only, inside
// PF_Cmd_USER_CHANGED_PARAM. Public AEGP calls only, in one undo group.
#pragma once

#include "effect/randamu.h"
#include "core/world.h"

enum class TransferResult {
    Done,
    DoneAnimationDropped,  // keyframes / expressions on the layer's transform were removed
    RefusedSeparated,      // the layer's position has separated dimensions: nothing changed
    RefusedWorldAnimated,  // the World Transform is keyframed or has expressions: nothing changed
    Partial,               // a layer stream could not be reset; *out is still to be written
    Failed                 // nothing could be read: nothing changed
};

// Off -> on: the layer's 3D transform at the current frame (parents included) is multiplied into the World
// Transform (*out = cur * layer-to-world * V^-1), and the layer goes to its initial 2D state: position = comp
// centre (z 0), orientation and rotations 0, scale 100%, no parent, 3D off. Keyframes and expressions on
// those streams are removed (DoneAnimationDropped). The anchor point is kept. Every check is made before
// anything changes. *worldChanged: whether the caller must write *out to the World Transform parameters
// (false when the layer is already in its initial state, so a keyframed World Transform is never touched).
TransferResult TransferLayerToWorld(PF_InData* in_data, AEGP_PluginID id, const rnd::WorldXform& cur,
                                    rnd::WorldXform* out, bool* worldChanged);

// On -> off: the layer becomes 3D again (nothing is transferred back: the World Transform holds the placement).
bool SetLayer3D(PF_InData* in_data, bool on);
