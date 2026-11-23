#include "hook/flatten_site.h"

#include <cstring>

namespace rnd {

// AE 2025 (BEE.dll), right after `call [rax+IsLayerType]` in Artisan's flatten check:
//  [0]  test al,al; jnz short
//  [4]  call BEE_BetaFeature_AreExpressionEffectsAlwaysAllowedEnabled
//  [9]  test al,al; jz short; mov rax,[rbx]; mov rcx,rbx; call [rax+disp32]   (effects count)
//  [25] mov rdx,[rax]; mov rcx,rax; call [rdx+disp32]
//  [37] test eax,eax; jnz short; mov rax,[rbx]; mov rcx,rbx; call [rax+disp32] (uses layer styles)
//  [53] test al,al; jnz short; mov rcx,rbx; call BEE_GetSelectedTrackMatte
// The first half alone also matches the audio-synth check (reached from HasAudio), hence the tail.
bool MatchFlattenCheckSite(const unsigned char* p, uintptr_t betaFn)
{
    if (!(p[0] == 0x84 && p[1] == 0xC0 && p[2] == 0x75 && p[4] == 0xE8)) return false;
    int rel = 0;
    std::memcpy(&rel, p + 5, 4);
    if ((uintptr_t)(p + 9 + rel) != betaFn) return false;
    if (!(p[9] == 0x84 && p[10] == 0xC0 && p[11] == 0x74 && p[13] == 0x48 && p[14] == 0x8B && p[16] == 0x48 &&
          p[17] == 0x8B && p[19] == 0xFF && p[20] == 0x90))
        return false;
    return p[25] == 0x48 && p[26] == 0x8B && p[27] == 0x10 && p[28] == 0x48 && p[29] == 0x8B && p[30] == 0xC8 &&
           p[31] == 0xFF && p[32] == 0x92 && p[37] == 0x85 && p[38] == 0xC0 && p[39] == 0x75 && p[41] == 0x48 &&
           p[42] == 0x8B && p[47] == 0xFF && p[48] == 0x90 && p[53] == 0x84 && p[54] == 0xC0 && p[55] == 0x75 &&
           p[57] == 0x48 && p[58] == 0x8B && p[59] == 0xCB && p[60] == 0xE8;
}

}  // namespace rnd
