/* DupliX 2: the version and the global out flags, in one place for the PiPL (DupliX2PiPL.r, read by "cl /EP", so
   plain literals only) and GlobalSetup (dx2.cpp, which static_asserts them against the SDK's enums). A mismatch
   shows a dialog in AE the first time the effect is applied ("global outflags do not match"). */
#ifndef DX2_FLAGS_H
#define DX2_FLAGS_H

/* PF_VERSION(0, 2, 0, PF_Stage_DEVELOP, 1) */
#define DX2_VERSION_MAJOR 0
#define DX2_VERSION_MINOR 2
#define DX2_VERSION_BUG 0
#define DX2_VERSION_BUILD 1
#define DX2_EFFECT_VERSION 65537

/* PF_OutFlag_CUSTOM_UI (1<<15) | PF_OutFlag_DEEP_COLOR_AWARE (1<<25) | PF_OutFlag_SEND_UPDATE_PARAMS_UI (1<<26) */
#define DX2_OUT_FLAGS 0x06008000

/* PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG (1<<3) | PF_OutFlag2_SUPPORTS_SMART_RENDER (1<<10) |
   PF_OutFlag2_FLOAT_COLOR_AWARE (1<<12) | PF_OutFlag2_SUPPORTS_THREADED_RENDERING (1<<27) */
#define DX2_OUT_FLAGS2 0x08001408

#endif
