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

}  // namespace rnd
