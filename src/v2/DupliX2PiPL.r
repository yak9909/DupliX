#include "AEConfig.h"
#include "AE_EffectVers.h"
#include "dx2_flags.h"

#ifndef AE_OS_WIN
	#include <AE_General.r>
#endif

resource 'PiPL' (16000) {
	{
		Kind { AEEffect },
		Name { "DupliX 2 (Preview)" },
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
		/* the values are in dx2_flags.h (GlobalSetup checks them against the SDK) */
		AE_Effect_Version { DX2_EFFECT_VERSION },
		AE_Effect_Info_Flags { 0 },
		AE_Effect_Global_OutFlags { DX2_OUT_FLAGS },
		AE_Effect_Global_OutFlags_2 { DX2_OUT_FLAGS2 },
		AE_Effect_Match_Name { "DupliX2" },
		AE_Reserved_Info { 0 },
		AE_Effect_Support_URL { "https://github.com/yak9909/DupliX" }
	}
};
