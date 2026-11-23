#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
	#include <AE_General.r>
#endif

resource 'PiPL' (16000) {
	{
		Kind { AEEffect },
		Name { "RandamuSpike" },
		Category { "Randamu Dev" },
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
		AE_Effect_Version { 32769 /* PF_VERSION(0,1,0,DEVELOP,1) = (1<<15)|1 */ },
		AE_Effect_Info_Flags { 0 },
		/* PF_OutFlag_DEEP_COLOR_AWARE */
		AE_Effect_Global_OutFlags { 0x02000000 },
		/* SUPPORTS_SMART_RENDER | FLOAT_COLOR_AWARE | I_MIX_GUID_DEPENDENCIES | SUPPORTS_THREADED_RENDERING */
		AE_Effect_Global_OutFlags_2 { 0x08201400 },
		AE_Effect_Match_Name { "RNDM RandamuSpike" },
		AE_Reserved_Info { 0 },
		AE_Effect_Support_URL { "https://example.invalid" }
	}
};
