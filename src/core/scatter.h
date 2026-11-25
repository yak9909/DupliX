// randamu core: deterministic per-copy transforms. No After Effects dependency.
#pragma once

namespace rnd {

struct ScatterParams {
    int count = 0;                  // >= 0
    int seed = 0;
    double pos[3] = {0, 0, 0};      // ± range in layer-space px
    double rotDeg[3] = {0, 0, 0};   // ± range in degrees
    double scaleMin = 100, scaleMax = 100;      // percent (100 = same size)
    double opacityMin = 100, opacityMax = 100;  // percent (0..100)
    bool keepFirst = false;         // copy 0 = identity transform, opacity 100%
    bool fast = false;              // fast mode: all copies drawn by the effect into one screen-aligned plane
    bool merge = false;             // normal mode: merge copies into group planes (in the padding: size stays 96)
};

// m4: row-vector matrix (p' = p * M), row 3 = translation, layer-space px. opacity: 0..1.
struct CopyXform {
    double m4[16];
    double opacity;
};

// Uniform in [0,1), determined only by (seed, copy, channel).
// Channels: 0..2 position XYZ, 3..5 rotation XYZ, 6 scale, 7 opacity.
double Rand01(int seed, int copy, int channel);

// The random draws of copy i before they become a matrix: translation (px), rotation (degrees),
// scale (percent), opacity (percent). The reference scenes put these on real duplicate layers.
struct CopyParams {
    double t[3];
    double r[3];
    double s;
    double o;
};
CopyParams DrawCopy(const ScatterParams& p, int i);

void ComputeCopy(const ScatterParams& p, int i, double anchorX, double anchorY, CopyXform* out);

namespace detail {
// M = T(-A) * S(s,s,s) * Rz * Ry * Rx * T(A + t)   (row vectors; angles in degrees, AE's Rotate X/Y/Z signs)
void MakeM4(double tx, double ty, double tz, double rxDeg, double ryDeg, double rzDeg, double s, double ax, double ay,
            double out[16]);
}  // namespace detail

}  // namespace rnd
