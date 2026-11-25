#include "core/ae_span.h"

#include <xmmintrin.h>

#include <climits>
#include <cmath>
#include <cstring>

namespace rnd {

namespace {

// vcvttss2si / vcvttsd2si: truncation, 0x80000000 when out of range or NaN
int Cvtt(float f)
{
    if (!(f > -2147483904.0f && f < 2147483648.0f)) return INT_MIN;
    return (int)f;
}
int CvttD(double d)
{
    if (!(d > -2147483649.0 && d < 2147483648.0)) return INT_MIN;
    return (int)d;
}
// vmaxss / vminss: the first operand when the comparison holds, else the second
float MaxSs(float a, float b) { return a > b ? a : b; }
float MinSs(float a, float b) { return a < b ? a : b; }
// vpackssdw + vpackuswb of one dword
uint8_t Sat8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

const float kInv16 = 1.52587890625e-05f;       // 2^-16 (PF xmmword_1803AB660, dword_1803AB6E0)
const float kInv32 = 2.3283064365386963e-10f;  // 2^-32 (PF xmmword_1803AB670)

void Load(const uint8_t* p, uint8_t out[4]) { std::memcpy(out, p, 4); }

// The per-pixel sampler state (Advanced3D's v55, the fields this path uses)
struct Samp {
    float f0 = 0, f4 = 0, f8 = 0;
    bool persp = false;
    double U = 0, V = 0, W = 0, invW = 1;
    float dUx = 0, dUy = 0, dVx = 0, dVy = 0, dWx = 0, dWy = 0;
};

// sub_1800542C0: the state at screen (x, y) (pixel centre, doubles)
void InitRow(Samp& s, const AeSpanParams& p, double x, double y)
{
    const double* m = p.m;
    s.dUx = (float)m[0], s.dVx = (float)m[1], s.dWx = (float)m[2];
    s.dUy = (float)m[3], s.dVy = (float)m[4], s.dWy = (float)m[5];
    s.U = (y * m[3] + x * m[0]) + m[6];
    s.V = (x * m[1] + y * m[4]) + m[7];
    s.W = (x * m[2] + y * m[5]) + m[8];
    const float h = s.dWx * 0.5f;
    const float a = h + 0.0f, b = 0.0f - h;
    const bool pos = 0.0f < s.dWx;
    const float pp = pos ? a : b, qq = pos ? b : a;
    const float k = s.dWy * 0.5f;
    float q1, p1;
    if (s.dWy > 0.0f) q1 = qq - k, p1 = k + pp;
    else q1 = k + qq, p1 = pp - k;
    s.f0 = q1 + q1;
    s.f4 = p1 + p1;
    const double d = (double)s.dWx;
    if (1e-12 > d && d > -1e-12) s.dWx = 0;
    s.persp = p.persp;
    s.invW = s.persp ? 1.0 / s.W : 1.0;
    if (p.a0 != 0) s.f8 = (float)(1.0 / p.a0);
}

void Step(Samp& s)
{
    s.U = (double)s.dUx + s.U;
    s.V = (double)s.dVx + s.V;
    if (s.dWx != 0.0f) {
        s.W = (double)s.dWx + s.W;
        if (s.persp) s.invW = 1.0 / s.W;
    }
}

// sub_180056980: nearest
void SampleNearest(const Samp& s, const AeTex8& t, uint8_t out[4])
{
    std::memset(out, 0, 4);
    if (!(s.invW > 0)) return;
    const int ix = CvttD(std::floor(s.invW * s.U)), iy = CvttD(std::floor(s.invW * s.V));
    if (ix < 0 || ix >= t.width || iy < 0 || iy >= t.height) return;
    Load(t.data + (ptrdiff_t)iy * t.rowbytes + 4 * ix, out);
}

int Fixed(float t) { return Cvtt((t < 0.0f ? -0.5f : 0.5f) + t * 65536.0f); }

// sub_180055540 (U_G_CPU_DataP + 8 != 0) -> sub_18005D1C0 -> PF.dll's sampler
void SampleQ1(const Samp& s, const AeTex8& t, uint8_t out[4])
{
    const float uf = (float)s.U, vf = (float)s.V, iw = (float)s.invW, wf = (float)s.W;
    float u, v, su, sv;
    if (s.persp) {
        if (!(wf + s.f0 > 0.0f)) {
            std::memset(out, 0, 4);
            return;
        }
        const float ux = s.dUx * iw, wx = s.dWx * iw, wy = s.dWy * iw;
        u = uf * iw;
        const float dux = ux - wx * u;
        v = vf * iw;
        const float uy = s.dUy * iw, vx = s.dVx * iw;
        const float dvx = vx - wx * v;
        const float duy = uy - wy * u;
        const float vy = s.dVy * iw;
        const float dvy = vy - wy * v;
        su = MaxSs(std::fabs(dux), std::fabs(duy)) * 0.5f;
        sv = MaxSs(std::fabs(dvx), std::fabs(dvy)) * 0.5f;
    } else {
        u = uf, v = vf;
        su = MaxSs(std::fabs(s.dUx), std::fabs(s.dUy)) * 0.5f;
        sv = MaxSs(std::fabs(s.dVx), std::fabs(s.dUx)) * 0.5f;  // AE's own operands
    }
    const int fx = Fixed(u), fy = Fixed(v);
    const int rx = 100.0f > su ? Fixed(su) : 0x640000;
    const int ry = 100.0f > sv ? Fixed(sv) : 0x640000;
    if (rx > 0x8000 || ry > 0x8000) AeAreaSample8(t, fx, fy, rx, ry, out);
    else AeSubpixelSample8(t, fx, fy, out);
}

}  // namespace

void AeFillRcpTable(AeRcpTable* t)
{
    t->r[0] = 0;
    for (int i = 1; i < 256; i++) t->r[i] = _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss((float)i)));
}

bool AeSpanSupported(int quality, int xfer, int motionSamples, bool lights, bool mask, bool blendTables, int cpuFlag)
{
    return (quality == 0 || quality == 1) && xfer == 2 && motionSamples == 1 && !lights && !mask && !blendTables &&
           cpuFlag != 0;
}

// PF_SubpixelSample_Straight (PF.dll 0x180089330)
void AeSubpixelSample8(const AeTex8& t, int fx, int fy, uint8_t out[4])
{
    static const uint8_t zero[4] = {0, 0, 0, 0};
    const int xf = fx & 0xFFFF, xi = (int16_t)(fx >> 16), yf = fy & 0xFFFF, yi = (int16_t)(fy >> 16);
    const uint8_t* p00 = t.data + (ptrdiff_t)yi * t.rowbytes + 4 * (ptrdiff_t)xi;
    if (!xf && !yf) {
        if (xi >= 0 && xi < t.width && yi >= 0 && yi < t.height) Load(p00, out);
        else std::memset(out, 0, 4);
        return;
    }
    const bool vx0 = xi < t.width && xi >= 0, vy0 = yi < t.height && yi >= 0;
    const bool vx1 = xi + 1 < t.width && xi + 1 >= 0, vy1 = yi + 1 < t.height && yi + 1 >= 0;
    const uint8_t* c00 = vx0 && vy0 ? p00 : zero;
    const uint8_t* c10 = vy0 && vx1 ? p00 + 4 : zero;
    const uint8_t* c01 = vy1 && vx0 ? p00 + t.rowbytes : zero;
    const uint8_t* c11 = vy1 && vx1 ? p00 + t.rowbytes + 4 : zero;
    if (!std::memcmp(c00, c10, 4) && !std::memcmp(c10, c01, 4) && !std::memcmp(c01, c11, 4)) {
        Load(c00, out);
        return;
    }
    const float fxs = (float)xf * kInv16, fys = (float)yf * kInv16;
    const float w11 = fys * fxs;
    const float w10 = fxs - w11;
    float tt = 1.0f - w10;
    const float w01 = fys - w11;
    tt = tt - w01;
    const float w00 = tt - w11;
    if (c00[0] == c10[0] && c01[0] == c11[0] && c00[0] == c01[0]) {
        if (!c00[0]) {
            std::memset(out, 0, 4);
            return;
        }
        out[0] = Sat8(Cvtt((float)c00[0]));
        for (int k = 1; k < 4; k++) {
            const float s5 = (float)c11[k] * w11 + (float)c01[k] * w01;
            const float s1 = (float)c10[k] * w10 + w00 * (float)c00[k];
            out[k] = Sat8(Cvtt((s5 + s1) + 0.5f));
        }
        return;
    }
    const float a00 = (float)c00[0] * w00, a10 = (float)c10[0] * w10, a01 = (float)c01[0] * w01, a11 = (float)c11[0] * w11;
    const float A = (a11 + a01) + (a10 + a00);
    if (!(A >= 0.5f)) {
        std::memset(out, 0, 4);
        return;
    }
    const float r = 1.0f / A;
    out[0] = Sat8(Cvtt(A + 0.5f));
    for (int k = 1; k < 4; k++) {
        const float s5 = a11 * (float)c11[k] + a01 * (float)c01[k];
        const float s0 = a10 * (float)c10[k] + a00 * (float)c00[k];
        out[k] = Sat8(Cvtt((s5 + s0) * r + 0.5f));
    }
}

// The area sampler PF_AreaSample_Straight dispatches to on this CPU (PF.dll 0x18008A560)
void AeAreaSample8(const AeTex8& t, int x, int y, int rx, int ry, uint8_t out[4])
{
    float s8 = 0, s9[4] = {0, 0, 0, 0};
    uint8_t px[4];
    auto add = [&](const uint8_t c[4], float w) {
        s8 = w + s8;
        for (int k = 0; k < 4; k++) s9[k] = (float)c[k] * w + s9[k];
    };
    auto corner = [](int a, int b, float alpha) { return (((float)a * kInv32) * (float)b) * alpha; };
    auto edge = [](int a, float alpha) { return ((float)a * kInv16) * alpha; };
    auto fix = [](int16_t v) { return (int)((uint32_t)(int32_t)v << 16); };
    int ebx = (int)((uint32_t)x + 0x8000u + (uint32_t)rx), esi = (int)((uint32_t)x - (uint32_t)rx + 0x8000u);
    int r15 = (int)((uint32_t)y + 0x8000u + (uint32_t)ry), edi = (int)((uint32_t)y - (uint32_t)ry + 0x8000u);
    if (ry <= 0x8000 && rx <= 0x8000) {
        AeSubpixelSample8(t, x, y, px);
        add(px, corner(ebx - esi, r15 - edi, (float)px[0]));
    } else if (ry <= 0x8000) {
        const int w2 = 2 * ry;
        int16_t di = (int16_t)(esi >> 16);
        if (esi & 0xFFFF) {
            AeSubpixelSample8(t, fix(di), y, px);
            add(px, corner(0x10000 - (esi & 0xFFFF), w2, (float)px[0]));
            di++;
        }
        const int16_t si = (int16_t)(ebx >> 16);
        for (; di < si; di++) {
            AeSubpixelSample8(t, fix(di), y, px);
            add(px, edge(w2, (float)px[0]));
        }
        if (ebx & 0xFFFF) {
            AeSubpixelSample8(t, fix(si), y, px);
            add(px, corner(ebx & 0xFFFF, w2, (float)px[0]));
        }
    } else if (rx <= 0x8000) {
        const int w2 = 2 * rx;
        int16_t bx = (int16_t)(edi >> 16);
        if (edi & 0xFFFF) {
            AeSubpixelSample8(t, x, fix(bx), px);
            add(px, corner(0x10000 - (edi & 0xFFFF), w2, (float)px[0]));
            bx++;
        }
        const int16_t di = (int16_t)(r15 >> 16);
        for (; bx < di; bx++) {
            AeSubpixelSample8(t, x, fix(bx), px);
            add(px, edge(w2, (float)px[0]));
        }
        if (r15 & 0xFFFF) {
            AeSubpixelSample8(t, x, fix(di), px);
            add(px, corner(r15 & 0xFFFF, w2, (float)px[0]));
        }
    } else {
        const int W = t.width << 16, H = t.height << 16;
        auto clampTo = [](int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); };
        esi = clampTo(esi, W), ebx = clampTo(ebx, W), edi = clampTo(edi, H), r15 = clampTo(r15, H);
        int16_t r10 = (int16_t)(ebx >> 16);
        const int16_t si = (int16_t)(esi >> 16);
        if (ebx & 0xFFFF) r10++;
        const int16_t r8 = (int16_t)(edi >> 16);
        int16_t r9 = (int16_t)(r15 >> 16);
        if (r15 & 0xFFFF) r9++;
        r9 = (int16_t)(r9 - r8);
        const int l = esi & 0xFFFF, rf = ebx & 0xFFFF, tf = edi & 0xFFFF, bf = r15 & 0xFFFF;
        const uint8_t* row = t.data + (ptrdiff_t)((int)r8 * t.rowbytes) + 4 * (ptrdiff_t)si;
        if (tf) {
            int16_t cols = (int16_t)(r10 - si);
            const uint8_t* p = row;
            if (l) {
                Load(p, px), p += 4;
                add(px, corner(0x10000 - l, 0x10000 - tf, (float)px[0]));
                cols--;
            }
            if (rf) cols--;
            if (cols) {
                const float k4 = (float)(0x10000 - tf) * kInv16;
                for (; cols > 0; cols--, p += 4) Load(p, px), add(px, k4 * (float)px[0]);
            }
            if (rf) Load(p, px), add(px, corner(rf, 0x10000 - tf, (float)px[0]));
            row += t.rowbytes;
            r9--;
        }
        if (bf) r9--;
        const int16_t colsBase = (int16_t)(r10 - si);
        for (; r9 > 0; r9--, row += t.rowbytes) {
            int16_t cols = colsBase;
            const uint8_t* p = row;
            if (l) {
                Load(p, px), p += 4;
                add(px, edge(0x10000 - l, (float)px[0]));
                cols--;
            }
            if (rf) cols--;
            for (int c = cols; c > 0; c--, p += 4) Load(p, px), add(px, (float)px[0]);
            if (rf) Load(p, px), add(px, edge(rf, (float)px[0]));
        }
        if (bf) {
            int16_t cols = colsBase;
            const uint8_t* p = row;
            if (l) {
                Load(p, px), p += 4;
                add(px, corner(0x10000 - l, bf, (float)px[0]));
                cols--;
            }
            if (rf) cols--;
            if (cols) {
                const float k4 = (float)bf * kInv16;
                for (int c = cols; c > 0; c--, p += 4) Load(p, px), add(px, k4 * (float)px[0]);
            }
            if (rf) Load(p, px), add(px, corner(rf, bf, (float)px[0]));
        }
    }
    if (s8 == 0.0f) {
        std::memset(out, 0, 4);
        return;
    }
    const float area = (float)rx * (float)ry;
    const float k = 1073741824.0f / area;
    const float alpha = MinSs(s8 * k, 255.0f);
    if (alpha == 0.0f) {
        std::memset(out, 0, 4);
        return;
    }
    out[0] = Sat8(Cvtt(alpha + 0.5f));
    for (int c = 1; c < 4; c++) out[c] = Sat8(Cvtt(MinSs(s9[c] / s8, 255.0f) + 0.5f));
}

// PF.dll's in-front transfer without blending tables, SIMD path (0x180254DE0)
void AeXferInFront8(const uint8_t src[4], uint8_t op, const AeRcpTable& rcp, uint8_t dst[4])
{
    unsigned sa = src[0];
    if (op != 0xFF) {
        const unsigned e = sa * op + 128;
        sa = ((e >> 8) + e) >> 8;
    }
    sa &= 0xFF;
    if (!sa) return;
    if (sa == 0xFF) {
        std::memcpy(dst, src, 4);
        return;
    }
    const unsigned da = dst[0];
    if (!da) {
        std::memcpy(dst, src, 4);
        dst[0] = (uint8_t)sa;
        return;
    }
    const uint16_t tn = (uint16_t)((255 - sa) * (255 - da) + 128);
    const unsigned mn = (uint16_t)(tn + (tn >> 8)) >> 8;
    const unsigned aout = (~mn) & 0xFF;
    uint8_t res[4];
    for (int c = 1; c < 4; c++) {
        const uint16_t t = (uint16_t)(da * dst[c] + 128);
        const uint16_t dpre = (uint16_t)((uint16_t)(t + (t >> 8)) >> 8);
        const int16_t diff = (int16_t)(src[c] - dpre);
        const int32_t prod = (int32_t)diff * (int32_t)sa;
        const int32_t q = prod + 128;
        const int32_t q2 = (q >> 8) + q;
        const int32_t q3 = q2 >> 8;
        const int16_t v = (int16_t)(q3 + dpre);
        const float f = (float)v * 255.0f;
        const float g = (float)(int)(aout >> 1);
        const float h = f + g;
        res[c] = Sat8(Cvtt(h * rcp.r[aout] + 0.5f));
    }
    dst[0] = (uint8_t)aout;
    dst[1] = res[1], dst[2] = res[2], dst[3] = res[3];
}

void AeDrawSpan8(const AeSpanParams& p, const AeTex8& tex, int x0, int x1, int y, uint8_t* row)
{
    if (x1 <= x0) return;
    Samp s;
    InitRow(s, p, (double)(float)((float)x0 + 0.5f), (double)(float)((float)y + 0.5f));
    const uint8_t op = (uint8_t)CvttD(p.opacity * 255.0 + 0.5);
    uint8_t px[4];
    for (int x = x0; x < x1; x++) {
        if (p.quality) SampleQ1(s, tex, px);
        else SampleNearest(s, tex, px);
        if (p.colorMode == 2) px[1] = px[2] = px[3] = 0xFF;
        if (px[0]) AeXferInFront8(px, op, *p.rcp, row + 4 * x);
        Step(s);
    }
}

}  // namespace rnd
