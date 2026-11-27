// DupliX 2 (prototype): the render. So far only the Sources are real: the copies are laid out in a flat grid
// (Count A x Count B, Grid Size X/Y, centred on the layer) and each copy shows one of the chosen layers, picked
// by Assign. With no Source chosen the copies show this layer. Each copy is fitted into its cell (keeping its
// aspect); the 3D shapes, Count C and the rest of the controls are not drawn yet.
#include "v2/dx2.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace dx2 {
namespace {

constexpr int kInputId = 0;  // checkout ids: the input, then Source k = 1 + k

struct Pre {
    int countA = 5, countB = 5;
    int seed = 0;
    int assign = AS_SEQUENTIAL;
    double gridX = 800, gridY = 800;
    bool has[kSourceSlots] = {};
    PF_LRect layerRect = {0, 0, 0, 0};  // the input's max rect (downsampled layer px)
};
void DeletePre(void* p) { delete (Pre*)p; }

PF_Err Checkout(PF_InData* in_data, int index, PF_ParamDef* d)
{
    AEFX_CLR_STRUCT(*d);
    return PF_CHECKOUT_PARAM(in_data, index, in_data->current_time, in_data->time_step, in_data->time_scale, d);
}

bool RectEmpty(const PF_LRect& r) { return r.right <= r.left || r.bottom <= r.top; }

}  // namespace

PF_Err PreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* extra)
{
    auto* pre = new Pre();
    extra->output->pre_render_data = pre;
    extra->output->delete_pre_render_data_func = DeletePre;

    PF_ParamDef d;
    if (!Checkout(in_data, P_COUNT_A, &d)) pre->countA = d.u.sd.value, PF_CHECKIN_PARAM(in_data, &d);
    if (!Checkout(in_data, P_COUNT_B, &d)) pre->countB = d.u.sd.value, PF_CHECKIN_PARAM(in_data, &d);
    if (!Checkout(in_data, P_SEED, &d)) pre->seed = d.u.sd.value, PF_CHECKIN_PARAM(in_data, &d);
    if (!Checkout(in_data, P_SRC_ASSIGN, &d)) pre->assign = d.u.pd.value, PF_CHECKIN_PARAM(in_data, &d);
    if (!Checkout(in_data, P_GRID_SIZE_X, &d)) pre->gridX = d.u.fs_d.value, PF_CHECKIN_PARAM(in_data, &d);
    if (!Checkout(in_data, P_GRID_SIZE_Y, &d)) pre->gridY = d.u.fs_d.value, PF_CHECKIN_PARAM(in_data, &d);

    // every layer whole, whatever part of the output is asked for
    PF_RenderRequest req = extra->input->output_request;
    req.rect.left = req.rect.top = -30000;
    req.rect.right = req.rect.bottom = 30000;
    req.preserve_rgb_of_zero_alpha = FALSE;

    PF_CheckoutResult in_result;
    AEFX_CLR_STRUCT(in_result);
    PF_Err err = extra->cb->checkout_layer(in_data->effect_ref, P_INPUT, kInputId, &req, in_data->current_time,
                                           in_data->time_step, in_data->time_scale, &in_result);
    if (err) {
        Log("prerender: input checkout failed (%d)", (int)err);
        return err;
    }
    pre->layerRect = in_result.max_result_rect;

    for (int k = 0; k < kSourceSlots; k++) {
        PF_CheckoutResult r;
        AEFX_CLR_STRUCT(r);
        // a Source set to None checks out nothing (empty rects)
        if (extra->cb->checkout_layer(in_data->effect_ref, P_SRC_LAYER + k, 1 + k, &req, in_data->current_time,
                                      in_data->time_step, in_data->time_scale, &r) == PF_Err_NONE)
            pre->has[k] = !RectEmpty(r.max_result_rect);
    }

    // the output: the layer's own bounds (the copies are drawn inside them)
    PF_LRect out = pre->layerRect;
    const PF_LRect& want = extra->input->output_request.rect;
    PF_LRect res = {std::max(out.left, want.left), std::max(out.top, want.top), std::min(out.right, want.right),
                    std::min(out.bottom, want.bottom)};
    if (RectEmpty(res)) res = {0, 0, 0, 0};
    extra->output->result_rect = res;
    extra->output->max_result_rect = out;
    extra->output->solid = FALSE;
    extra->output->flags = PF_RenderOutputFlag_RETURNS_EXTRA_PIXELS;
    Log("prerender: layer (%d,%d)-(%d,%d), request (%d,%d)-(%d,%d), sources %d%d%d%d%d%d%d%d", out.left, out.top,
        out.right, out.bottom, want.left, want.top, want.right, want.bottom, pre->has[0], pre->has[1], pre->has[2],
        pre->has[3], pre->has[4], pre->has[5], pre->has[6], pre->has[7]);
    return PF_Err_NONE;
}

namespace {

struct Px {
    float a, r, g, b;  // premultiplied, 0..1
};

template <class P>
struct Fmt;
template <>
struct Fmt<PF_Pixel8> {
    static constexpr float kMax = 255.0f;
};
template <>
struct Fmt<PF_Pixel16> {
    static constexpr float kMax = 32768.0f;
};
template <>
struct Fmt<PF_PixelFloat> {
    static constexpr float kMax = 1.0f;
};

template <class P>
inline Px Load(const P& p)
{
    const float s = 1.0f / Fmt<P>::kMax;
    return {p.alpha * s, p.red * s, p.green * s, p.blue * s};
}
template <class T>
inline T Clamp(float v, float mx)
{
    v = v * mx + 0.5f;
    if (v < 0) v = 0;
    if (v > mx) v = mx;
    return (T)v;
}
inline void Store(PF_Pixel8& p, const Px& v)
{
    p.alpha = Clamp<A_u_char>(v.a, 255), p.red = Clamp<A_u_char>(v.r, 255), p.green = Clamp<A_u_char>(v.g, 255),
    p.blue = Clamp<A_u_char>(v.b, 255);
}
inline void Store(PF_Pixel16& p, const Px& v)
{
    p.alpha = Clamp<A_u_short>(v.a, 32768), p.red = Clamp<A_u_short>(v.r, 32768),
    p.green = Clamp<A_u_short>(v.g, 32768), p.blue = Clamp<A_u_short>(v.b, 32768);
}
inline void Store(PF_PixelFloat& p, const Px& v) { p.alpha = v.a, p.red = v.r, p.green = v.g, p.blue = v.b; }

template <class P>
inline const P& At(const PF_EffectWorld* w, int x, int y)
{
    return *(const P*)((const char*)w->data + (size_t)y * w->rowbytes + (size_t)x * sizeof(P));
}

// bilinear, transparent outside the world
template <class P>
Px Sample(const PF_EffectWorld* w, float x, float y)
{
    x -= 0.5f, y -= 0.5f;
    const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const float fx = x - x0, fy = y - y0;
    Px acc = {0, 0, 0, 0};
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            const int xi = x0 + i, yj = y0 + j;
            if (xi < 0 || yj < 0 || xi >= w->width || yj >= w->height) continue;
            const float wt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            const Px p = Load(At<P>(w, xi, yj));
            acc.a += p.a * wt, acc.r += p.r * wt, acc.g += p.g * wt, acc.b += p.b * wt;
        }
    return acc;
}

struct Copy {
    const PF_EffectWorld* src;
    float x0, y0, x1, y1;  // in the output world's px
    float scale;           // output px per source px
};

template <class P>
void DrawRows(PF_EffectWorld* out, const std::vector<Copy>& copies, int yBegin, int yEnd)
{
    for (int y = yBegin; y < yEnd; y++) {
        P* row = (P*)((char*)out->data + (size_t)y * out->rowbytes);
        const float py = y + 0.5f;
        for (const Copy& c : copies) {
            if (py < c.y0 || py >= c.y1) continue;
            // a box of n x n bilinear samples per output px when the copy is smaller than its source
            const int n = std::clamp((int)std::ceil(1.0f / c.scale), 1, 8);
            const float inv = 1.0f / c.scale, step = 1.0f / n;
            const int xa = std::max(0, (int)std::floor(c.x0)), xb = std::min(out->width, (int)std::ceil(c.x1));
            for (int x = xa; x < xb; x++) {
                const float px = x + 0.5f;
                if (px < c.x0 || px >= c.x1) continue;
                Px s = {0, 0, 0, 0};
                for (int j = 0; j < n; j++)
                    for (int i = 0; i < n; i++) {
                        const float ox = x + (i + 0.5f) * step, oy = y + (j + 0.5f) * step;
                        const Px q = Sample<P>(c.src, (ox - c.x0) * inv, (oy - c.y0) * inv);
                        s.a += q.a, s.r += q.r, s.g += q.g, s.b += q.b;
                    }
                const float k = 1.0f / (n * n);
                s.a *= k, s.r *= k, s.g *= k, s.b *= k;
                if (s.a <= 0) continue;
                Px d = Load(row[x]);
                const float t = 1.0f - s.a;  // premultiplied "over"
                d.a = s.a + d.a * t, d.r = s.r + d.r * t, d.g = s.g + d.g * t, d.b = s.b + d.b * t;
                Store(row[x], d);
            }
        }
    }
}

template <class P>
void Draw(PF_EffectWorld* out, const std::vector<Copy>& copies)
{
    for (int y = 0; y < out->height; y++) memset((char*)out->data + (size_t)y * out->rowbytes, 0, (size_t)out->width * sizeof(P));
    const int threads = std::clamp((int)std::thread::hardware_concurrency(), 1, 16);
    const int rows = out->height, chunk = (rows + threads - 1) / threads;
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; t++) {
        const int a = t * chunk, b = std::min(rows, a + chunk);
        if (a < b) pool.emplace_back([=, &copies] { DrawRows<P>(out, copies, a, b); });
    }
    DrawRows<P>(out, copies, 0, std::min(rows, chunk));
    for (auto& th : pool) th.join();
}

}  // namespace

PF_Err SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra)
{
    const Pre* pre = (const Pre*)extra->input->pre_render_data;
    PF_EffectWorld* outW = nullptr;
    PF_EffectWorld* inW = nullptr;
    PF_EffectWorld* srcW[kSourceSlots] = {};
    PF_Err err = extra->cb->checkout_layer_pixels(in_data->effect_ref, kInputId, &inW);
    if (err) Log("render: input pixels failed (%d)", (int)err);
    if (!err) {
        err = extra->cb->checkout_output(in_data->effect_ref, &outW);
        if (err) Log("render: output failed (%d)", (int)err);
    }
    // the chosen Sources, in slot order (an empty slot is skipped)
    std::vector<const PF_EffectWorld*> sources;
    for (int k = 0; k < kSourceSlots && !err && pre; k++) {
        if (!pre->has[k]) continue;
        if (extra->cb->checkout_layer_pixels(in_data->effect_ref, 1 + k, &srcW[k]) == PF_Err_NONE && srcW[k] &&
            srcW[k]->width > 0 && srcW[k]->height > 0)
            sources.push_back(srcW[k]);
    }
    if (!err && outW && pre) {
        if (sources.empty() && inW) sources.push_back(inW);
        // layout in downsampled layer px; the output world's (0,0) is result_rect's top left
        const double dsx = in_data->downsample_x.den ? (double)in_data->downsample_x.num / in_data->downsample_x.den : 1;
        const double dsy = in_data->downsample_y.den ? (double)in_data->downsample_y.num / in_data->downsample_y.den : 1;
        const PF_LRect& lr = pre->layerRect;
        const PF_LRect& rr = extra->input->output_request.rect;
        const double ox = std::max(lr.left, rr.left), oy = std::max(lr.top, rr.top);
        const double cx = (lr.left + lr.right) * 0.5 - ox, cy = (lr.top + lr.bottom) * 0.5 - oy;
        const int A = std::max(1, pre->countA), B = std::max(1, pre->countB);
        const double cellW = pre->gridX * dsx / A, cellH = pre->gridY * dsy / B;
        std::vector<Copy> copies;
        const int n = (int)sources.size();
        for (int b = 0; b < B && n > 0; b++)
            for (int a = 0; a < A; a++) {
                const int i = b * A + a;
                const PF_EffectWorld* w = sources[AssignSource(pre->assign, pre->seed, i, a, b, n)];
                const double fit = std::min(cellW * 0.9 / w->width, cellH * 0.9 / w->height);
                if (!(fit > 0)) continue;
                const double px = cx + (a - (A - 1) * 0.5) * cellW, py = cy + (b - (B - 1) * 0.5) * cellH;
                const double hw = w->width * fit * 0.5, hh = w->height * fit * 0.5;
                copies.push_back({w, (float)(px - hw), (float)(py - hh), (float)(px + hw), (float)(py + hh), (float)fit});
            }
        PF_WorldSuite2* ws = nullptr;
        PF_PixelFormat fmt = PF_PixelFormat_INVALID;
        if (in_data->pica_basicP->AcquireSuite(kPFWorldSuite, kPFWorldSuiteVersion2, (const void**)&ws) == kSPNoError && ws) {
            ws->PF_GetPixelFormat(outW, &fmt);
            in_data->pica_basicP->ReleaseSuite(kPFWorldSuite, kPFWorldSuiteVersion2);
        }
        Log("render: %d source(s), %d x %d copies drawn %d, format %d, output %d x %d", n, A, B, (int)copies.size(),
            (int)fmt, outW->width, outW->height);
        // every source has the output's depth (AE converts layers to the project's depth)
        switch (fmt) {
        case PF_PixelFormat_ARGB32: Draw<PF_Pixel8>(outW, copies); break;
        case PF_PixelFormat_ARGB64: Draw<PF_Pixel16>(outW, copies); break;
        case PF_PixelFormat_ARGB128: Draw<PF_PixelFloat>(outW, copies); break;
        default:
            Log("render: unknown pixel format %d", (int)fmt);
            err = PF_Err_BAD_CALLBACK_PARAM;
            break;
        }
    }
    if (err) Log("render: failed (%d)", (int)err);
    for (int k = 0; k < kSourceSlots; k++)
        if (srcW[k]) extra->cb->checkin_layer_pixels(in_data->effect_ref, 1 + k);
    if (inW) extra->cb->checkin_layer_pixels(in_data->effect_ref, kInputId);
    return err;
}

}  // namespace dx2
