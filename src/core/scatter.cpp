#include "core/scatter.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace rnd {

namespace {

uint64_t SplitMix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

void Identity(double m[16])
{
    std::memset(m, 0, sizeof(double) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0;
}

void Mul(const double a[16], const double b[16], double o[16])
{
    double t[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
            t[r * 4 + c] = s;
        }
    std::memcpy(o, t, sizeof(t));
}

double Signed(int seed, int copy, int channel, double range)
{
    return (2.0 * Rand01(seed, copy, channel) - 1.0) * range;
}

}  // namespace

double Rand01(int seed, int copy, int channel)
{
    uint64_t h = SplitMix64((uint64_t)(uint32_t)seed);
    h = SplitMix64(h ^ ((uint64_t)(uint32_t)copy * 0xD1B54A32D192ED03ull));
    h = SplitMix64(h ^ ((uint64_t)(uint32_t)channel * 0x8CB92BA72F3D8DD7ull));
    return (double)(h >> 11) * (1.0 / 9007199254740992.0);  // 53 bits -> [0,1)
}

namespace detail {

void MakeM4(double tx, double ty, double tz, double rxDeg, double ryDeg, double rzDeg, double s, double ax, double ay,
            double out[16])
{
    const double d2r = 3.14159265358979323846 / 180.0;
    double cx = std::cos(rxDeg * d2r), sx = std::sin(rxDeg * d2r);
    double cy = std::cos(ryDeg * d2r), sy = std::sin(ryDeg * d2r);
    double cz = std::cos(rzDeg * d2r), sz = std::sin(rzDeg * d2r);
    double m[16];
    Identity(m);
    m[12] = -ax;
    m[13] = -ay;
    const double S[16] = {s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, 0, 0, 0, 1};
    const double Rz[16] = {cz, sz, 0, 0, -sz, cz, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const double Ry[16] = {cy, 0, -sy, 0, 0, 1, 0, 0, sy, 0, cy, 0, 0, 0, 0, 1};
    const double Rx[16] = {1, 0, 0, 0, 0, cx, sx, 0, 0, -sx, cx, 0, 0, 0, 0, 1};
    double T[16];
    Identity(T);
    T[12] = ax + tx;
    T[13] = ay + ty;
    T[14] = tz;
    Mul(m, S, m);
    Mul(m, Rz, m);
    Mul(m, Ry, m);
    Mul(m, Rx, m);
    Mul(m, T, m);
    std::memcpy(out, m, sizeof(m));
}

}  // namespace detail

void ComputeCopy(const ScatterParams& p, int i, double anchorX, double anchorY, CopyXform* out)
{
    if (p.keepFirst && i == 0) {
        Identity(out->m4);
        out->opacity = 1.0;
        return;
    }
    double tx = Signed(p.seed, i, 0, p.pos[0]);
    double ty = Signed(p.seed, i, 1, p.pos[1]);
    double tz = Signed(p.seed, i, 2, p.pos[2]);
    double rx = Signed(p.seed, i, 3, p.rotDeg[0]);
    double ry = Signed(p.seed, i, 4, p.rotDeg[1]);
    double rz = Signed(p.seed, i, 5, p.rotDeg[2]);
    double sc = p.scaleMin + Rand01(p.seed, i, 6) * (p.scaleMax - p.scaleMin);
    double op = p.opacityMin + Rand01(p.seed, i, 7) * (p.opacityMax - p.opacityMin);
    detail::MakeM4(tx, ty, tz, rx, ry, rz, sc / 100.0, anchorX, anchorY, out->m4);
    out->opacity = op / 100.0;
}

}  // namespace rnd
