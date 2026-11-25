// randamu: the merge plan of the frame a hook is asked about (render threads only).
#pragma once

#include <memory>

#include "AE_GeneralPlug.h"
#include "core/merge_plan.h"

namespace rnd {

// The comp plan for `layer` (an AEGP_LayerH = BEE_Layer*) at its layer time. reread: read the comp through AEGP
// and rebuild the plan if anything changed (the Count hook, once per frame and layer); otherwise the plan last
// found for this layer and time is used (read only if there is none). Null when nothing can be read.
std::shared_ptr<const MergePlan> MergePlanFor(const void* layer, const A_Time& layerTime, bool reread);

// The layer's part of a plan (null if the layer is not in it or has no group: then it is drawn as before).
const MergeLayerPlan* MergeLayerOf(const MergePlan& plan, int compItemId, int layerId);

}  // namespace rnd
