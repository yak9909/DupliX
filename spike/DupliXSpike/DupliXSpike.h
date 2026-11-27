// THROWAWAY SPIKE — not product code.
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
#include "AE_GeneralPlug.h"

#define SPIKE_NUM_SLOTS 64

enum {
    SP_INPUT = 0,
    SP_SLOT_FIRST,                                        // 1
    SP_SLOT_LAST = SP_SLOT_FIRST + SPIKE_NUM_SLOTS - 1,   // 64
    SP_COMP_SIZE,   // expr: [thisComp.width, thisComp.height, 0]
    SP_SELF_O,      // expr: toComp([0,0,0])
    SP_SELF_X,      // expr: toComp([100,0,0])
    SP_SELF_Y,      // expr: toComp([0,100,0])
    SP_S1_C0,       // expr: slot1 layer toComp([0,0,0]) + camera depth
    SP_S1_C1,       // expr: slot1 layer toComp([100,0,0])
    SP_S1_C2,       // expr: slot1 layer toComp([0,100,0])
    SP_S2_C0,       // S6 (track matte): same as S1 C0..C2, for the slot-2 layer (the matte)
    SP_S2_C1,
    SP_S2_C2,
    SP_NUM_PARAMS
};

enum {
    DISK_ID_SLOT_BASE = 1,        // 1..64
    DISK_ID_COMP_SIZE = 200,
    DISK_ID_SELF_O,
    DISK_ID_SELF_X,
    DISK_ID_SELF_Y,
    DISK_ID_S1_C0,
    DISK_ID_S1_C1,
    DISK_ID_S1_C2,
    DISK_ID_S2_C0,
    DISK_ID_S2_C1,
    DISK_ID_S2_C2,
};

extern "C" {
DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data,
                            PF_ParamDef* params[], PF_LayerDef* output, void* extra);
}
