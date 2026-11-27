#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
	#include <AE_General.r>
#endif

resource 'PiPL' (16000) {
	{
		Kind { AEEffect },
		Name { "DupliX 2 (Prototype)" },
		Category { "DupliX" },
#ifdef AE_OS_WIN
	#if defined(AE_PROC_INTELx64)
		CodeWin64X86 {"EffectMain"},
	#elif defined(AE_PROC_ARM64)
		CodeWinARM64 {"EffectMain"},
	#endif
#elif defined(AE_OS_MAC)
		CodeMacIntel64 {"EffectMain"},
		CodeMacARM64 {"EffectMain"},
#endif
		AE_PiPL_Version { 2, 0 },
		AE_Effect_Spec_Version { PF_PLUG_IN_VERSION, PF_PLUG_IN_SUBVERS },
		/* PF_VERSION(0,1,0,PF_Stage_DEVELOP,1) */
		AE_Effect_Version { 32769 },
		AE_Effect_Info_Flags { 0 },
		/* CUSTOM_UI | DEEP_COLOR_AWARE | SEND_UPDATE_PARAMS_UI (must match GlobalSetup) */
		AE_Effect_Global_OutFlags { 0x06008000 },
		/* PARAM_GROUP_START_COLLAPSED_FLAG | SUPPORTS_SMART_RENDER | FLOAT_COLOR_AWARE | SUPPORTS_THREADED_RENDERING */
		AE_Effect_Global_OutFlags_2 { 0x08001408 },
		AE_Effect_Match_Name { "DupliX2" },
		AE_Reserved_Info { 0 },
		AE_Effect_Support_URL { "https://github.com/yak9909/DupliX" }
	}
};
