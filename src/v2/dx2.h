// DupliX 2 (prototype): shared declarations of the effect's files.
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
#include "AE_EffectUI.h"
#include "AE_GeneralPlug.h"

#include <string>

#include "v2/dx2_params.h"

namespace dx2 {

// one line to the file named by DUPLIX2_LOG or DUPLIX_LOG (no-op if neither is set)
void Log(const char* fmt, ...);

extern AEGP_PluginID g_pluginId;

template <class T>
struct Suite {
    SPBasicSuite* sp;
    T* s = nullptr;
    const char* name;
    int32 ver;
    Suite(SPBasicSuite* p, const char* n, int32 v) : sp(p), name(n), ver(v)
    {
        if (sp->AcquireSuite(n, v, (const void**)&s) != kSPNoError) s = nullptr;
    }
    ~Suite()
    {
        if (s) sp->ReleaseSuite(name, ver);
    }
    T* operator->() const { return s; }
    explicit operator bool() const { return s != nullptr; }
};

// Which of n Sources copy i (column a, row b) shows: the render and the Overview's picture agree.
inline unsigned Hash32(unsigned x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
inline int AssignSource(int assign, int seed, int i, int a, int b, int n)
{
    if (n <= 1) return 0;
    switch (assign) {
    case AS_RANDOM: return (int)(Hash32((unsigned)i * 2654435761U ^ Hash32((unsigned)seed + 1)) % (unsigned)n);
    case AS_BY_A: return a % n;
    case AS_BY_B: return b % n;
    default: return i % n;
    }
}

// ---- Sources (dx2_aegp.cpp) ----
struct SourceInfo {
    bool used = false;
    std::string name;  // the layer's name as the timeline shows it (UTF-8)
    int width = 0, height = 0;
};
// The layers chosen in Source 1..8 (read through AEGP: works in UI commands, where params[] holds no layer).
bool ReadSources(PF_InData* in_data, SourceInfo out[kSourceSlots]);
// Sets Source `slot` to `layerId` (0 = None).
bool SetSourceLayer(PF_InData* in_data, int slot, A_long layerId);
A_long SourceLayerId(PF_InData* in_data, int slot);
// The number of Source rows shown: up to the last used one, or more after "+ Add Source", at least 1.
int SourceRowsShown(PF_InData* in_data, const SourceInfo src[kSourceSlots]);
void SetMinSourceRows(PF_InData* in_data, int rows);
// Sequence data: an id per instance (what it remembers in the UI is keyed by it). SEQUENCE_SETUP and RESETUP.
PF_Err SequenceSetup(PF_InData* in_data, PF_OutData* out_data);
void SequenceSetdown(PF_InData* in_data, PF_OutData* out_data);

// ---- which controls are shown (dx2_visibility.cpp) ----
// Hides the controls that do not apply (another Shape's settings, unused modifier slots, ...). Changes only
// the streams whose state differs from what this instance last set.
// Returns true if a row changed (then ask AE to redraw the Effect Controls: PF_OutFlag_REFRESH_UI).
bool UpdateVisibility(PF_InData* in_data, PF_ParamDef* params[]);
// Shows / hides params [first, last] now (the "+" buttons: a hidden parameter's value cannot be set).
bool SetHiddenRange(PF_InData* in_data, PF_ParamDef* params[], int first, int last, bool hidden);

// ---- custom UI (dx2_ui.cpp) ----
PF_Err HandleEvent(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output,
                   PF_EventExtra* extra);
// fixed at PARAMS_SETUP: sized for the narrowest Effect Controls (a 240 px control at a 12 pt font)
constexpr int kHeaderHeight = 124;   // title + 6 lines beside a 96 px picture
constexpr int kModListHeight = 88;   // 8 chips + the button in 3 rows of 3
constexpr int kSrcListHeight = 94;   // 8 cards (or 7 + the button) in 4 rows of 2

// ---- render (dx2_render.cpp) ----
PF_Err PreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* extra);
PF_Err SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra);

}  // namespace dx2
