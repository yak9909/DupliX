// randamu core: the body of AE's classic 3D span drawing (core/ae_span.h), shared by the CPU (C++) and the GPU (CUDA,
// src/gpu/fast_raster.cu) so both compute the same pixels. Every float operation follows Advanced3D.aex / PF.dll's
// machine code (order and precision); the CUDA build must not contract (nvcc --fmad=false) and keeps IEEE division.
#pragma once

#ifdef __CUDACC__
#define RND_HD __host__ __device__ inline
#else
#define RND_HD inline
#include <cmath>
#endif

namespace rnd {

// A texture as AE holds it: 8 bpc ARGB (bytes A, R, G, B), straight alpha.
struct AeTex8 {
    const unsigned char* data;
    int rowbytes, width, height;
};

// One surface's drawing parameters (raster state and layer params, docs/experiments/2026-11-26-a3d-span-notes.md)
struct AeSpanCore {
    double m[9];       // screen (x, y) -> texture (U, V, W): U = (y*m3 + x*m0) + m6 ...
    double a0;         // raster state + 0xA0
    double opacity;    // layer params + 0x20
    int persp;         // raster state + 0x98
    int quality;       // raster state + 0xA8: 0 nearest, 1 bilinear / area
    int colorMode;     // layer params + 0x5B (2: colour forced to white)
    float zoff;        // layer params + 0xA8 (the M buffer's depth offset)
};

namespace aeimpl {

RND_HD int Cvtt(float f)  // vcvttss2si
{
    if (!(f > -2147483904.0f && f < 2147483648.0f)) return (-2147483647 - 1);
    return (int)f;
}
RND_HD int CvttD(double d)  // vcvttsd2si
{
    if (!(d > -2147483649.0 && d < 2147483648.0)) return (-2147483647 - 1);
    return (int)d;
}
RND_HD float MaxSs(float a, float b) { return a > b ? a : b; }
RND_HD float MinSs(float a, float b) { return a < b ? a : b; }
RND_HD float AbsF(float a) { return a < 0.0f ? -a : (a == 0.0f ? 0.0f : a); }
RND_HD unsigned char Sat8(int v) { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }
RND_HD void Set4(unsigned char* o, const unsigned char* s) { o[0] = s[0], o[1] = s[1], o[2] = s[2], o[3] = s[3]; }
RND_HD void Zero4(unsigned char* o) { o[0] = o[1] = o[2] = o[3] = 0; }
RND_HD bool Eq4(const unsigned char* a, const unsigned char* b)
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

#define RND_INV16 1.52587890625e-05f       // 2^-16
#define RND_INV32 2.3283064365386963e-10f  // 2^-32

struct Samp {
    float f0, f4, f8;
    int persp;
    double U, V, W, invW;
    float dUx, dUy, dVx, dVy, dWx, dWy;
};

// sub_1800542C0: the state at screen (x, y)
RND_HD void InitRow(Samp& s, const AeSpanCore& p, double x, double y)
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
    s.f8 = 0;
    const double d = (double)s.dWx;
    if (1e-12 > d && d > -1e-12) s.dWx = 0;
    s.persp = p.persp;
    s.invW = s.persp ? 1.0 / s.W : 1.0;
    if (p.a0 != 0) s.f8 = (float)(1.0 / p.a0);
}

RND_HD void Step(Samp& s)
{
    s.U = (double)s.dUx + s.U;
    s.V = (double)s.dVx + s.V;
    if (s.dWx != 0.0f) {
        s.W = (double)s.dWx + s.W;
        if (s.persp) s.invW = 1.0 / s.W;
    }
}

// PF_SubpixelSample_Straight (PF.dll 0x180089330)
RND_HD void SubpixelSample8(const AeTex8& t, int fx, int fy, unsigned char* out)
{
    const unsigned char zero[4] = {0, 0, 0, 0};
    const int xf = fx & 0xFFFF, xi = (short)(fx >> 16), yf = fy & 0xFFFF, yi = (short)(fy >> 16);
    const bool vx0 = xi < t.width && xi >= 0, vy0 = yi < t.height && yi >= 0;
    if (!xf && !yf) {
        if (vx0 && vy0) Set4(out, t.data + (long long)yi * t.rowbytes + 4 * (long long)xi);
        else Zero4(out);
        return;
    }
    const bool vx1 = xi + 1 < t.width && xi + 1 >= 0, vy1 = yi + 1 < t.height && yi + 1 >= 0;
    const long long o00 = (long long)yi * t.rowbytes + 4 * (long long)xi;
    const unsigned char* c00 = vx0 && vy0 ? t.data + o00 : zero;
    const unsigned char* c10 = vy0 && vx1 ? t.data + o00 + 4 : zero;
    const unsigned char* c01 = vy1 && vx0 ? t.data + o00 + t.rowbytes : zero;
    const unsigned char* c11 = vy1 && vx1 ? t.data + o00 + t.rowbytes + 4 : zero;
    if (Eq4(c00, c10) && Eq4(c10, c01) && Eq4(c01, c11)) {
        Set4(out, c00);
        return;
    }
    const float fxs = (float)xf * RND_INV16, fys = (float)yf * RND_INV16;
    const float w11 = fys * fxs;
    const float w10 = fxs - w11;
    float tt = 1.0f - w10;
    const float w01 = fys - w11;
    tt = tt - w01;
    const float w00 = tt - w11;
    if (c00[0] == c10[0] && c01[0] == c11[0] && c00[0] == c01[0]) {
        if (!c00[0]) {
            Zero4(out);
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
        Zero4(out);
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

struct AreaAcc {
    float s8, s9[4];
    RND_HD void Add(const unsigned char* c, float w)
    {
        s8 = w + s8;
        for (int k = 0; k < 4; k++) s9[k] = (float)c[k] * w + s9[k];
    }
};
RND_HD float AreaCorner(int a, int b, float alpha) { return (((float)a * RND_INV32) * (float)b) * alpha; }
RND_HD float AreaEdge(int a, float alpha) { return ((float)a * RND_INV16) * alpha; }
RND_HD int AreaFix(short v) { return (int)((unsigned)(int)v << 16); }

// The area sampler PF_AreaSample_Straight dispatches to on AVX CPUs (PF.dll 0x18008A560)
RND_HD void AreaSample8(const AeTex8& t, int x, int y, int rx, int ry, unsigned char* out)
{
    AreaAcc acc = {0, {0, 0, 0, 0}};
    unsigned char px[4];
    int ebx = (int)((unsigned)x + 0x8000u + (unsigned)rx), esi = (int)((unsigned)x - (unsigned)rx + 0x8000u);
    int r15 = (int)((unsigned)y + 0x8000u + (unsigned)ry), edi = (int)((unsigned)y - (unsigned)ry + 0x8000u);
    if (ry <= 0x8000 && rx <= 0x8000) {
        SubpixelSample8(t, x, y, px);
        acc.Add(px, AreaCorner(ebx - esi, r15 - edi, (float)px[0]));
    } else if (ry <= 0x8000) {
        const int w2 = 2 * ry;
        short di = (short)(esi >> 16);
        if (esi & 0xFFFF) {
            SubpixelSample8(t, AreaFix(di), y, px);
            acc.Add(px, AreaCorner(0x10000 - (esi & 0xFFFF), w2, (float)px[0]));
            di++;
        }
        const short si = (short)(ebx >> 16);
        for (; di < si; di++) {
            SubpixelSample8(t, AreaFix(di), y, px);
            acc.Add(px, AreaEdge(w2, (float)px[0]));
        }
        if (ebx & 0xFFFF) {
            SubpixelSample8(t, AreaFix(si), y, px);
            acc.Add(px, AreaCorner(ebx & 0xFFFF, w2, (float)px[0]));
        }
    } else if (rx <= 0x8000) {
        const int w2 = 2 * rx;
        short bx = (short)(edi >> 16);
        if (edi & 0xFFFF) {
            SubpixelSample8(t, x, AreaFix(bx), px);
            acc.Add(px, AreaCorner(0x10000 - (edi & 0xFFFF), w2, (float)px[0]));
            bx++;
        }
        const short di = (short)(r15 >> 16);
        for (; bx < di; bx++) {
            SubpixelSample8(t, x, AreaFix(bx), px);
            acc.Add(px, AreaEdge(w2, (float)px[0]));
        }
        if (r15 & 0xFFFF) {
            SubpixelSample8(t, x, AreaFix(di), px);
            acc.Add(px, AreaCorner(r15 & 0xFFFF, w2, (float)px[0]));
        }
    } else {
        const int W = t.width << 16, H = t.height << 16;
        esi = esi < 0 ? 0 : (esi > W ? W : esi);
        ebx = ebx < 0 ? 0 : (ebx > W ? W : ebx);
        edi = edi < 0 ? 0 : (edi > H ? H : edi);
        r15 = r15 < 0 ? 0 : (r15 > H ? H : r15);
        short r10 = (short)(ebx >> 16);
        const short si = (short)(esi >> 16);
        if (ebx & 0xFFFF) r10++;
        const short r8 = (short)(edi >> 16);
        short r9 = (short)(r15 >> 16);
        if (r15 & 0xFFFF) r9++;
        r9 = (short)(r9 - r8);
        const int l = esi & 0xFFFF, rf = ebx & 0xFFFF, tf = edi & 0xFFFF, bf = r15 & 0xFFFF;
        const unsigned char* row = t.data + (long long)((int)r8 * t.rowbytes) + 4 * (long long)si;
        const short colsBase = (short)(r10 - si);
        if (tf) {
            short cols = colsBase;
            const unsigned char* p = row;
            if (l) {
                Set4(px, p), p += 4;
                acc.Add(px, AreaCorner(0x10000 - l, 0x10000 - tf, (float)px[0]));
                cols--;
            }
            if (rf) cols--;
            if (cols) {
                const float k4 = (float)(0x10000 - tf) * RND_INV16;
                for (; cols > 0; cols--, p += 4) Set4(px, p), acc.Add(px, k4 * (float)px[0]);
            }
            if (rf) Set4(px, p), acc.Add(px, AreaCorner(rf, 0x10000 - tf, (float)px[0]));
            row += t.rowbytes;
            r9--;
        }
        if (bf) r9--;
        for (; r9 > 0; r9--, row += t.rowbytes) {
            short cols = colsBase;
            const unsigned char* p = row;
            if (l) {
                Set4(px, p), p += 4;
                acc.Add(px, AreaEdge(0x10000 - l, (float)px[0]));
                cols--;
            }
            if (rf) cols--;
            for (int c = cols; c > 0; c--, p += 4) Set4(px, p), acc.Add(px, (float)px[0]);
            if (rf) Set4(px, p), acc.Add(px, AreaEdge(rf, (float)px[0]));
        }
        if (bf) {
            short cols = colsBase;
            const unsigned char* p = row;
            if (l) {
                Set4(px, p), p += 4;
                acc.Add(px, AreaCorner(0x10000 - l, bf, (float)px[0]));
                cols--;
            }
            if (rf) cols--;
            if (cols) {
                const float k4 = (float)bf * RND_INV16;
                for (int c = cols; c > 0; c--, p += 4) Set4(px, p), acc.Add(px, k4 * (float)px[0]);
            }
            if (rf) Set4(px, p), acc.Add(px, AreaCorner(rf, bf, (float)px[0]));
        }
    }
    if (acc.s8 == 0.0f) {
        Zero4(out);
        return;
    }
    const float area = (float)rx * (float)ry;
    const float k = 1073741824.0f / area;
    const float alpha = MinSs(acc.s8 * k, 255.0f);
    if (alpha == 0.0f) {
        Zero4(out);
        return;
    }
    out[0] = Sat8(Cvtt(alpha + 0.5f));
    for (int c = 1; c < 4; c++) out[c] = Sat8(Cvtt(MinSs(acc.s9[c] / acc.s8, 255.0f) + 0.5f));
}

// PF.dll's in-front transfer without blending tables, SIMD path (0x180254DE0); rcp: rcpps of 1..255 on AE's CPU
RND_HD void XferInFront8(const unsigned char* src, unsigned char op, const float* rcp, unsigned char* dst)
{
    unsigned sa = src[0];
    if (op != 0xFF) {
        const unsigned e = sa * op + 128;
        sa = ((e >> 8) + e) >> 8;
    }
    sa &= 0xFF;
    if (!sa) return;
    if (sa == 0xFF) {
        Set4(dst, src);
        return;
    }
    const unsigned da = dst[0];
    if (!da) {
        Set4(dst, src);
        dst[0] = (unsigned char)sa;
        return;
    }
    const unsigned short tn = (unsigned short)((255 - sa) * (255 - da) + 128);
    const unsigned mn = (unsigned short)(tn + (tn >> 8)) >> 8;
    const unsigned aout = (~mn) & 0xFF;
    unsigned char res[4];
    for (int c = 1; c < 4; c++) {
        const unsigned short t = (unsigned short)(da * dst[c] + 128);
        const unsigned short dpre = (unsigned short)((unsigned short)(t + (t >> 8)) >> 8);
        const short diff = (short)(src[c] - dpre);
        const int prod = (int)diff * (int)sa;
        const int q = prod + 128;
        const int q2 = (q >> 8) + q;
        const int q3 = q2 >> 8;
        const short v = (short)(q3 + dpre);
        const float f = (float)v * 255.0f;
        const float g = (float)(int)(aout >> 1);
        const float h = f + g;
        res[c] = Sat8(Cvtt(h * rcp[aout] + 0.5f));
    }
    dst[0] = (unsigned char)aout;
    dst[1] = res[1], dst[2] = res[2], dst[3] = res[3];
}

RND_HD int Fixed(float t) { return Cvtt((t < 0.0f ? -0.5f : 0.5f) + t * 65536.0f); }

// sub_180056980: nearest
RND_HD void SampleNearest(const Samp& s, const AeTex8& t, unsigned char* out)
{
    Zero4(out);
    if (!(s.invW > 0)) return;
    const int ix = CvttD(floor(s.invW * s.U)), iy = CvttD(floor(s.invW * s.V));
    if (ix < 0 || ix >= t.width || iy < 0 || iy >= t.height) return;
    Set4(out, t.data + (long long)iy * t.rowbytes + 4 * (long long)ix);
}

// sub_180055540 (CPU flag set) -> sub_18005D1C0 -> PF.dll's sampler
RND_HD void SampleQ1(const Samp& s, const AeTex8& t, unsigned char* out)
{
    const float uf = (float)s.U, vf = (float)s.V, iw = (float)s.invW, wf = (float)s.W;
    float u, v, su, sv;
    if (s.persp) {
        if (!(wf + s.f0 > 0.0f)) {
            Zero4(out);
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
        su = MaxSs(AbsF(dux), AbsF(duy)) * 0.5f;
        sv = MaxSs(AbsF(dvx), AbsF(dvy)) * 0.5f;
    } else {
        u = uf, v = vf;
        su = MaxSs(AbsF(s.dUx), AbsF(s.dUy)) * 0.5f;
        sv = MaxSs(AbsF(s.dVx), AbsF(s.dUx)) * 0.5f;  // AE's own operands
    }
    const int fx = Fixed(u), fy = Fixed(v);
    const int rx = 100.0f > su ? Fixed(su) : 0x640000;
    const int ry = 100.0f > sv ? Fixed(sv) : 0x640000;
    if (rx > 0x8000 || ry > 0x8000) AreaSample8(t, fx, fy, rx, ry, out);
    else SubpixelSample8(t, fx, fy, out);
}

RND_HD unsigned char OpacityByte(double opacity) { return (unsigned char)CvttD(opacity * 255.0 + 0.5); }

// One pixel of a span with the state s at that pixel (the caller steps s afterwards)
// The pixel a span draws at state s (before the transfer)
RND_HD void SamplePixel(const AeSpanCore& p, const Samp& s, const AeTex8& tex, unsigned char* px)
{
    if (p.quality) SampleQ1(s, tex, px);
    else SampleNearest(s, tex, px);
    if (p.colorMode == 2) px[1] = px[2] = px[3] = 0xFF;
}
// The transfer's alpha of a sampled pixel (XferInFront8: 0xFF replaces the target pixel by the sample)
RND_HD unsigned XferAlpha(const unsigned char* px, unsigned char op)
{
    unsigned sa = px[0];
    if (op != 0xFF) {
        const unsigned e = sa * op + 128;
        sa = ((e >> 8) + e) >> 8;
    }
    return sa & 0xFF;
}
RND_HD void DrawPixel(const AeSpanCore& p, const Samp& s, const AeTex8& tex, unsigned char op, const float* rcp,
                      unsigned char* dst)
{
    unsigned char px[4];
    SamplePixel(p, s, tex, px);
    if (px[0]) XferInFront8(px, op, rcp, dst);
}

// ---- a span into AE's M buffer (sub_1800477F0: 8 bpc, one motion sample, no lights, no mask, [params+0xB0][0] == 0).
// Each pixel whose sample's alpha is not 0 becomes a fragment: the sample (SamplePixel; AE's insert sub_180021A40
// multiplies its alpha by the opacity byte) at a depth. The depth's offset, at the span's start (after InitRow):
// ((-dWx * 0.5f) - 0.5f * dWy) + params+0xA8, in float. AE takes the pixels in pairs while two are left: a pair's
// first pixel's depth is float(f0 + W) + off in float, the second's (and a last single pixel's) (f0 + W) + off in
// double, rounded once.
RND_HD float MbufOff(const Samp& s, float zoff) { return ((-s.dWx) * 0.5f - 0.5f * s.dWy) + zoff; }
RND_HD float MbufDepth(const Samp& s, float off, bool pairFirst)
{
    if (pairFirst) {
        const float a = (float)((double)s.f0 + s.W);
        return a + off;
    }
    return (float)(((double)s.f0 + s.W) + (double)off);
}
// whether pixel i of a span of len pixels is the first of a pair
RND_HD bool MbufPairFirst(int i, int len) { return !(i & 1) && i + 1 < len; }

// The pixel centre AE starts a span at (sub_18004CFD0: (float)x + 0.5f, as double)
RND_HD double SpanStart(int v) { return (double)((float)v + 0.5f); }

}  // namespace aeimpl
}  // namespace rnd
