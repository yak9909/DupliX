// randamu core: After Effects 2025's classic 3D span drawing of one textured surface (8 bpc), rebuilt from its
// machine code so that DupliX can draw copies with exactly AE's pixels (docs/experiments/2026-11-26-a3d-span-notes.md).
// Covered: one motion sample, quality 0 (nearest) / 1 (bilinear, area), transfer "in front" without blending tables,
// no lights, no mask, AE's SIMD paths (U_G_CPU_DataP + 8 != 0). The arithmetic is in core/ae_span_impl.h (shared
// with the GPU). No After Effects dependency.
#pragma once

#include <cstdint>

#include "core/ae_span_impl.h"

namespace rnd {

// PF.dll's in-front transfer approximates 1 / alpha with the CPU's rcpps: its results for alpha 1..255 on this
// machine (index 0 unused). Fill with AeFillRcpTable on the CPU that runs AE.
struct AeRcpTable {
    float r[256];
};
void AeFillRcpTable(AeRcpTable* t);

struct AeSpanParams {
    double m[9] = {};      // the surface's sample: screen (x, y) -> texture (U, V, W)
    bool persp = false;    // raster state + 0x98
    double a0 = 0;         // raster state + 0xA0
    int quality = 1;       // raster state + 0xA8: 0 nearest, 1 bilinear / area
    double opacity = 1;    // layer params + 0x20
    int colorMode = 1;     // layer params + 0x5B (2: colour forced to white)
    const AeRcpTable* rcp = nullptr;
};
AeSpanCore AeCoreOf(const AeSpanParams& p);

// Draws pixels x0 .. x1 - 1 of row y (screen px) into `row` (the target's row y, 8 bpc ARGB) like AE.
void AeDrawSpan8(const AeSpanParams& p, const AeTex8& tex, int x0, int x1, int y, uint8_t* row);

// The pieces (exposed for tests): PF.dll's samplers and transfer.
void AeSubpixelSample8(const AeTex8& tex, int fx, int fy, uint8_t out[4]);
void AeAreaSample8(const AeTex8& tex, int fx, int fy, int rx, int ry, uint8_t out[4]);
void AeXferInFront8(const uint8_t src[4], uint8_t op, const AeRcpTable& rcp, uint8_t dst[4]);

}  // namespace rnd
