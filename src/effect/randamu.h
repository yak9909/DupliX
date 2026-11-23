// randamu: After Effects 2025 effect that scatters the layer as n real 3D copies
// (classic 3D sub-layers). See docs/superpowers/specs/2026-11-24-randamu-sublayer-design.md.
#pragma once

#define PF_DEEP_COLOR_AWARE 1

#include "AEConfig.h"
#ifdef AE_OS_WIN
#include <Windows.h>
#endif

#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AE_EffectCBSuites.h"
#include "AE_EffectSuites.h"
#include "AE_GeneralPlug.h"

#include "randamu_params.h"

// Writes one line to the file named by the RANDAMU_LOG environment variable (no-op if unset).
void RLog(const char* fmt, ...);

extern "C" {
DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                            PF_LayerDef* output, void* extra);
}
