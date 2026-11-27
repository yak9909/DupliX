// DupliX: the merge optimisation's view of one comp frame, read through AEGP on a render thread
// (spec docs/superpowers/specs/2026-11-25-merge-optimization-design.md 5.5, 5.9).
#pragma once

#include "AE_GeneralPlug.h"
#include "core/merge_plan.h"

namespace rnd {

// Reads the comp of `layer` (an AEGP_LayerH = BEE_Layer*) at the layer's time `layerTime`: the active camera
// (AE's default camera when there is none), the comp's DupliX layers with merge on (with their whole-layer
// reasons), and every other surface. Render threads only (never on AE's main thread). False if something required
// could not be read: then nothing merges.
bool ReadMergeScene(const void* layer, const A_Time& layerTime, MergeSceneInput* out);

// The whole-layer reasons not to merge (MergeBlock bits) of DupliX layer L at comp time compT, read through AEGP on
// any thread (the effect's status line, on AE's main thread). False if something could not be read.
bool ReadMergeBlock(AEGP_LayerH L, const A_Time& compT, unsigned* block);

// One line per layer / surface for the log (DUPLIX_LOG): the Task 2 gate reads it.
void LogMergeScene(const MergeSceneInput& in);

}  // namespace rnd
