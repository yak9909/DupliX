// Recognises the return address of the IsLayerType(0) call inside Artisan's "an AV layer with
// effects must render flattened" check (BEE.dll, AE 2025). Pure function over the code bytes.
#pragma once
#include <cstdint>

namespace rnd {

// p: the return address (first byte after the call). betaFn: address of the exported
// BEE_BetaFeature_AreExpressionEffectsAlwaysAllowedEnabled. Reads 61 bytes from p.
bool MatchFlattenCheckSite(const unsigned char* p, uintptr_t betaFn);

}  // namespace rnd
