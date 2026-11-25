#include "core/ae_span.h"

#include <xmmintrin.h>

namespace rnd {

void AeFillRcpTable(AeRcpTable* t)
{
    t->r[0] = 0;
    for (int i = 1; i < 256; i++) t->r[i] = _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss((float)i)));
}

AeSpanCore AeCoreOf(const AeSpanParams& p)
{
    AeSpanCore c = {};
    for (int k = 0; k < 9; k++) c.m[k] = p.m[k];
    c.a0 = p.a0, c.opacity = p.opacity, c.persp = p.persp ? 1 : 0, c.quality = p.quality, c.colorMode = p.colorMode;
    return c;
}

void AeSubpixelSample8(const AeTex8& t, int fx, int fy, uint8_t out[4]) { aeimpl::SubpixelSample8(t, fx, fy, out); }
void AeAreaSample8(const AeTex8& t, int x, int y, int rx, int ry, uint8_t out[4]) { aeimpl::AreaSample8(t, x, y, rx, ry, out); }
void AeXferInFront8(const uint8_t src[4], uint8_t op, const AeRcpTable& rcp, uint8_t dst[4])
{
    aeimpl::XferInFront8(src, op, rcp.r, dst);
}

void AeDrawSpan8(const AeSpanParams& p, const AeTex8& tex, int x0, int x1, int y, uint8_t* row)
{
    if (x1 <= x0) return;
    const AeSpanCore c = AeCoreOf(p);
    aeimpl::Samp s;
    aeimpl::InitRow(s, c, aeimpl::SpanStart(x0), aeimpl::SpanStart(y));
    const uint8_t op = aeimpl::OpacityByte(c.opacity);
    for (int x = x0; x < x1; x++) {
        aeimpl::DrawPixel(c, s, tex, op, p.rcp->r, row + 4 * x);
        aeimpl::Step(s);
    }
}

}  // namespace rnd
