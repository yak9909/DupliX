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

#include <atomic>

// Writes one line to the file named by the RANDAMU_LOG environment variable (no-op if unset).
void RLog(const char* fmt, ...);
// The merge may draw its group textures on the GPU (AE's GPU acceleration is CUDA; RANDAMU_MERGE_GPU_DRAW overrides).
bool MergeGpuAllowed();
// The log's generation: it changes when RANDAMU_LOG names another file (tests switch logs between comps).
int RLogGeneration();
// "Log only the first few" counters that start again in each log generation (each test comp's log gets them).
struct LogLimit {
    std::atomic<int> n{0}, gen{-1};
    // true for the first `limit` calls of this generation (and every power of two after it if powersOfTwo)
    bool Take(int limit, bool powersOfTwo = false, int* countOut = nullptr);
    // the count so far in this generation (without counting)
    int Count() const { return gen.load(std::memory_order_relaxed) == RLogGeneration() ? n.load() : 0; }
};

extern "C" {
DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                            PF_LayerDef* output, void* extra);
}
