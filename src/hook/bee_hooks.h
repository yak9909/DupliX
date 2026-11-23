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

struct ParamSnapshot;
// Reads the randamu parameters of `eff` and the anchor point of its layer L at layer time t.
// valid = false when the effect is switched off or L is not a 3D layer. Returns false if a read failed.
// Needs InstallHooks to have run (it stores the suite pointer and plugin ID).
bool ReadParamsFromEffect(AEGP_EffectRefH eff, AEGP_LayerH L, const A_Time& t, ParamSnapshot* out);

}  // namespace rnd
