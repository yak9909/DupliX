// randamu core: After Effects 2025's classic 3D span drawing of one textured surface (8 bpc), rebuilt from its
// machine code so that DupliX can draw copies with exactly AE's pixels (docs/experiments/2026-11-26-a3d-span-notes.md).
// Covered: one motion sample, quality 0 (nearest) / 1 (bilinear, area), transfer "in front" without blending tables,
// no lights, no mask, AE's SIMD paths (U_G_CPU_DataP + 8 != 0). Every float operation follows the machine code's
// order and precision. No After Effects dependency.
#pragma once

#include <cstdint>

namespace rnd {

// A texture as AE holds it: 8 bpc ARGB (bytes A, R, G, B), straight alpha.
struct AeTex8 {
    const uint8_t* data = nullptr;
    int rowbytes = 0, width = 0, height = 0;
};

// PF.dll's in-front transfer approximates 1 / alpha with the CPU's rcpps: its results for alpha 1..255 on this
// machine (index 0 unused). Fill with AeFillRcpTable on the CPU that runs AE.
struct AeRcpTable {
    float r[256];
};
void AeFillRcpTable(AeRcpTable* t);

struct AeSpanParams {
    double m[9] = {};      // the surface's sample: screen (x, y) -> texture (U, V, W) = (x, y, 1) * [m0 m1 m2; m3 m4 m5; m6 m7 m8]
    bool persp = false;    // raster state + 0x98
    double a0 = 0;         // raster state + 0xA0 (unused by this path, kept for the record)
    int quality = 1;       // raster state + 0xA8: 0 nearest, 1 bilinear / area
    double opacity = 1;    // layer params + 0x20
    int colorMode = 1;     // layer params + 0x5B (2: colour forced to white)
    const AeRcpTable* rcp = nullptr;
};

// Whether AeDrawSpan8 reproduces AE for these raster / layer settings (else AE must draw the surface).
bool AeSpanSupported(int quality, int xfer, int motionSamples, bool lights, bool mask, bool blendTables, int cpuFlag);

// Draws pixels x0 .. x1 - 1 of row y (screen px) into `row` (the target's row y, 8 bpc ARGB) like AE.
void AeDrawSpan8(const AeSpanParams& p, const AeTex8& tex, int x0, int x1, int y, uint8_t* row);

// The pieces (exposed for tests): PF.dll's samplers and transfer.
void AeSubpixelSample8(const AeTex8& tex, int fx, int fy, uint8_t out[4]);
void AeAreaSample8(const AeTex8& tex, int fx, int fy, int rx, int ry, uint8_t out[4]);
void AeXferInFront8(const uint8_t src[4], uint8_t op, const AeRcpTable& rcp, uint8_t dst[4]);

}  // namespace rnd
