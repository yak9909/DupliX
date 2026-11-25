// Installs the BEE.dll / AEGPDriver.aex replacements that make AE render a registered layer as n
// classic-3D sub-layers. Spec: docs/superpowers/specs/2026-11-24-randamu-sublayer-design.md section 3.
#pragma once

#include "AE_GeneralPlug.h"
#include "SPBasic.h"

namespace rnd {

enum class HookStatus { NotInstalled, Installed, MissingSymbols };

// Call once from PF_Cmd_GLOBAL_SETUP. The calling thread is remembered as AE's main thread:
// hooks never call AEGP on it (doing so from inside BEE virtuals hung AE in the spike).
HookStatus InstallHooks(SPBasicSuite* pica, AEGP_PluginID pluginId);
HookStatus GetHookStatus();
bool IsMainThread();  // the thread that ran InstallHooks (GLOBAL_SETUP)
// Finds BEE's internal type key of this effect (not the AEGP installed key) once per process:
// `layer` has this effect at AEGP index `effectIndex`. Call from the effect's own render commands.
// The hooks then also require BEE_Layer::HasEffectApplied(key): AE sends no command when the effect
// is deleted (it keeps the instance for undo), so a stale registration must not keep turning the
// layer into copies.
void ResolveEffectTypeKey(const void* layer, int effectIndex);
bool EffectTypeKeyKnown();
bool LayerHasThisEffect(const void* layer);  // AEGP_LayerH is a BEE_Layer*; true while the key is unknown
// 3D switch on and this effect present and switched on (BEE/FLT reads only; any thread).
bool LayerIsLive(const void* layer);

struct ParamSnapshot;
// Reads the randamu parameters of `eff` and the anchor point of its layer L at layer time t.
// valid = false when the effect is switched off or L is not a 3D layer. Returns false if a read failed.
// Needs InstallHooks to have run (it stores the suite pointer and plugin ID).
bool ReadParamsFromEffect(AEGP_EffectRefH eff, AEGP_LayerH L, const A_Time& t, ParamSnapshot* out);
// Anchor point of L at layer time t (layer-space px). Needs InstallHooks to have run.
bool ReadLayerAnchor(AEGP_LayerH L, const A_Time& t, double* x, double* y);
// Finds L's randamu effect and reads it at layer time t (render threads only). False if a read failed; an absent
// or switched-off effect gives true with out->valid = false. *effectIndex (optional): its AEGP index, *otherActive
// (optional): the layer's other active effects. Needs InstallHooks to have run.
bool ReadLayerParams(AEGP_LayerH L, const A_Time& t, ParamSnapshot* out, int* effectIndex = nullptr,
                     int* otherActive = nullptr);
SPBasicSuite* HookPica();
AEGP_PluginID HookPluginId();

}  // namespace rnd
