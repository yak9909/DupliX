// DupliX 2 (design v4): the render. PreRender evaluates the graph (src/graph) at the frame's time with the macro
// values and checks out the layers; SmartRender draws each element as its Source (Layer 1..8 or this layer) turned,
// scaled and faded, in list order, seen straight on (orthographic: z does not move a copy; a turned copy shows its
// turned outline). Render Mode 3D Layers draws the same for now (design v4 P6).
//
// Coordinates: the graph's positions are layer pixels from the layer's centre at full resolution. A copy's
// Source pixel at the Source layer's centre goes to the element's position.
//
// Tests: DUPLIX2_DUMP=<file> appends "t <seconds>" and the elements (graph::FormatElements) on every PreRender.
#include "v2/dx2.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace dx2 {
namespace {

constexpr int kInputId = 0;    // checkout ids: the input, then Layer k = 1 + k
constexpr int kOwn = kLayers;  // the Source slot of this layer

struct SourceRect {
    bool has = false;
    PF_LRect rect = {0, 0, 0, 0};  // the checked-out world's place in that layer's coordinates (downsampled)
    double pivX = 0, pivY = 0;     // the layer's centre in the world's pixels
    int w = 0, h = 0;
};

struct Copy {
    int src;                 // 0..7 = Layer, kOwn = this layer
    double px, py;           // where the Source's centre goes (output layer coordinates, downsampled)
    double ia, ib, ic, id;   // the inverse of the 2x2 map from Source px to output px
    double pivX, pivY;
    float opacity;
    int n;                   // samples per axis per output pixel (more when the copy is smaller than its Source)
    int x0, y0, x1, y1;      // its bounds in output layer coordinates
};

struct Pre {
    std::vector<Copy> copies;
    PF_LRect out = {0, 0, 0, 0};  // the result rect (output layer coordinates)
    bool has[kLayers + 1] = {};
};
void DeletePre(void* p) { delete (Pre*)p; }

bool RectEmpty(const PF_LRect& r) { return r.right <= r.left || r.bottom <= r.top; }

double Ratio(const PF_RationalScale& r) { return r.den ? (double)r.num / r.den : 1.0; }

std::mutex g_dumpMu;

// "t <seconds> layer <w> <h>": the layer's size tells the tests' comps apart
void Dump(double t, int w, int h, const dxg::EvalResult& r)
{
    char path[MAX_PATH] = {0};
    if (!GetEnvironmentVariableA("DUPLIX2_DUMP", path, sizeof(path))) return;
    std::lock_guard<std::mutex> lk(g_dumpMu);
    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "a") || !fp) return;
    fprintf(fp, "t %.6f layer %d %d\n", t, w, h);
    fputs(dxg::FormatElements(r).c_str(), fp);
    fclose(fp);
}

}  // namespace

PF_Err PreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* extra)
{
    (void)out_data;
    auto* pre = new Pre();
    extra->output->pre_render_data = pre;
    extra->output->delete_pre_render_data_func = DeletePre;
    const double dsx = Ratio(in_data->downsample_x), dsy = Ratio(in_data->downsample_y);
    const double tSec = in_data->time_scale ? (double)in_data->current_time / in_data->time_scale : 0;

    // the graph and the seed (checked out at the frame's time)
    std::shared_ptr<const dxg::Graph> g;
    int seed = 0;
    PF_ParamDef d;
    AEFX_CLR_STRUCT(d);
    if (!PF_CHECKOUT_PARAM(in_data, P_GRAPH, in_data->current_time, in_data->time_step, in_data->time_scale, &d)) {
        g = GraphOf(d.u.arb_d.value);
        PF_CHECKIN_PARAM(in_data, &d);
    }
    if (!g) g = std::make_shared<dxg::Graph>();
    AEFX_CLR_STRUCT(d);
    if (!PF_CHECKOUT_PARAM(in_data, P_SEED, in_data->current_time, in_data->time_step, in_data->time_scale, &d)) {
        seed = d.u.sd.value;
        PF_CHECKIN_PARAM(in_data, &d);
    }
    // the macros the graph reads, at this frame's time; another time (later: births) is checked out when asked
    const uint32_t usedM = dxg::UsedMacros(*g);
    double now[kMacros] = {};
    for (int k = 0; k < kMacros; k++) {
        now[k] = dxg::MacroDefault(k);
        if (!(usedM >> k & 1)) continue;
        AEFX_CLR_STRUCT(d);
        if (!PF_CHECKOUT_PARAM(in_data, P_MACRO + k, in_data->current_time, in_data->time_step, in_data->time_scale, &d)) {
            now[k] = d.u.fs_d.value;
            PF_CHECKIN_PARAM(in_data, &d);
        }
    }
    auto macro = [&](int k, double t) -> double {
        if (k < 0 || k >= kMacros) return 0;
        if (t == tSec) return now[k];
        const A_long at = (A_long)std::llround(t * in_data->time_scale);
        PF_ParamDef m;
        AEFX_CLR_STRUCT(m);
        if (PF_CHECKOUT_PARAM(in_data, P_MACRO + k, at, in_data->time_step, in_data->time_scale, &m)) return now[k];
        const double v = m.u.fs_d.value;
        PF_CHECKIN_PARAM(in_data, &m);
        return v;
    };

    // every layer whole, whatever part of the output is asked for
    PF_RenderRequest req = extra->input->output_request;
    req.rect.left = req.rect.top = -30000;
    req.rect.right = req.rect.bottom = 30000;
    req.preserve_rgb_of_zero_alpha = FALSE;
    SourceRect src[kLayers + 1];
    auto take = [&](const PF_CheckoutResult& r, SourceRect& s) {
        s.rect = r.result_rect;
        s.has = !RectEmpty(r.result_rect);
        s.w = r.result_rect.right - r.result_rect.left;
        s.h = r.result_rect.bottom - r.result_rect.top;
        s.pivX = r.ref_width * dsx * 0.5 - r.result_rect.left;
        s.pivY = r.ref_height * dsy * 0.5 - r.result_rect.top;
    };
    PF_CheckoutResult in_result;
    AEFX_CLR_STRUCT(in_result);
    PF_Err err = extra->cb->checkout_layer(in_data->effect_ref, P_INPUT, kInputId, &req, in_data->current_time,
                                           in_data->time_step, in_data->time_scale, &in_result);
    if (err) {
        Log("prerender: input checkout failed (%d)", (int)err);
        return err;
    }
    take(in_result, src[kOwn]);
    const double cx = in_result.ref_width * dsx * 0.5, cy = in_result.ref_height * dsy * 0.5;  // our layer's centre
    const uint32_t usedL = dxg::UsedLayers(*g);
    uint32_t layersSet = 0;
    for (int k = 0; k < kLayers; k++) {
        if (!(usedL >> k & 1)) continue;
        PF_CheckoutResult r;
        AEFX_CLR_STRUCT(r);
        // a Layer set to None checks out nothing (empty rects)
        if (extra->cb->checkout_layer(in_data->effect_ref, P_LAYER + k, 1 + k, &req, in_data->current_time,
                                      in_data->time_step, in_data->time_scale, &r) == PF_Err_NONE) {
            take(r, src[k]);
            if (src[k].has) layersSet |= 1u << k;
        }
    }

    dxg::EvalContext ctx;
    ctx.time = tSec;
    ctx.seed = seed;
    ctx.macro = macro;
    ctx.layersSet = layersSet;
    const dxg::EvalResult res = dxg::Evaluate(*g, ctx);
    Dump(tSec, (int)in_result.ref_width, (int)in_result.ref_height, res);

    // the copies: the 2x2 map Source px -> output px is D M D^-1 (D = the downsampling, M at full resolution)
    double ux0 = 1e300, uy0 = 1e300, ux1 = -1e300, uy1 = -1e300;
    for (const dxg::Element& e : res.elements) {
        if (!e.visible || !(e.opacity > 0)) continue;
        const int si = (e.source >= 0 && e.source < kLayers) ? e.source : kOwn;
        const SourceRect& s = src[si];
        if (!s.has) continue;
        const double k = res.copyScale;
        const double m00 = e.basis[0] * e.scale[0] * k, m01 = e.basis[1] * e.scale[1] * k;
        const double m10 = e.basis[3] * e.scale[0] * k, m11 = e.basis[4] * e.scale[1] * k;
        const double a = m00, b = m01 * dsx / dsy, c = m10 * dsy / dsx, dd = m11;
        const double det = a * dd - b * c;
        if (!(std::fabs(det) > 1e-9)) continue;  // edge-on or no size
        Copy cp;
        cp.src = si;
        cp.px = cx + e.pos[0] * dsx;
        cp.py = cy + e.pos[1] * dsy;
        cp.ia = dd / det, cp.ib = -b / det, cp.ic = -c / det, cp.id = a / det;
        cp.pivX = s.pivX, cp.pivY = s.pivY;
        cp.opacity = (float)std::min(1.0, e.opacity);
        // the smallest stretch of the map (singular value): how many Source px one output px spans at most
        const double p2 = a * a + b * b + c * c + dd * dd;
        const double smin = std::sqrt(std::max(0.0, (p2 - std::sqrt(std::max(0.0, p2 * p2 - 4 * det * det))) * 0.5));
        cp.n = std::clamp((int)std::ceil(1.0 / std::max(smin, 1e-6)), 1, 8);
        double bx0 = 1e300, by0 = 1e300, bx1 = -1e300, by1 = -1e300;
        for (int j = 0; j < 4; j++) {
            const double sx = (j & 1 ? s.w : 0) - s.pivX, sy = (j & 2 ? s.h : 0) - s.pivY;
            const double ox = cp.px + a * sx + b * sy, oy = cp.py + c * sx + dd * sy;
            bx0 = std::min(bx0, ox), by0 = std::min(by0, oy), bx1 = std::max(bx1, ox), by1 = std::max(by1, oy);
        }
        constexpr double kLim = 1 << 20;
        cp.x0 = (int)std::floor(std::max(bx0, -kLim)) - 1, cp.y0 = (int)std::floor(std::max(by0, -kLim)) - 1;
        cp.x1 = (int)std::ceil(std::min(bx1, kLim)) + 1, cp.y1 = (int)std::ceil(std::min(by1, kLim)) + 1;
        if (cp.x1 <= cp.x0 || cp.y1 <= cp.y0) continue;
        ux0 = std::min(ux0, (double)cp.x0), uy0 = std::min(uy0, (double)cp.y0);
        ux1 = std::max(ux1, (double)cp.x1), uy1 = std::max(uy1, (double)cp.y1);
        pre->copies.push_back(cp);
        pre->has[si] = true;
    }

    PF_LRect max = {0, 0, 0, 0};
    if (!pre->copies.empty()) max = {(A_long)ux0, (A_long)uy0, (A_long)ux1, (A_long)uy1};
    const PF_LRect& want = extra->input->output_request.rect;
    PF_LRect res_rect = {std::max(max.left, want.left), std::max(max.top, want.top), std::min(max.right, want.right),
                         std::min(max.bottom, want.bottom)};
    if (RectEmpty(res_rect)) res_rect = {0, 0, 0, 0};
    pre->out = res_rect;
    extra->output->result_rect = res_rect;
    extra->output->max_result_rect = max;
    extra->output->solid = FALSE;
    extra->output->flags = PF_RenderOutputFlag_RETURNS_EXTRA_PIXELS;
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 200)
        Log("prerender: t %.3f, %zu elements, %zu copies drawn, out (%d,%d)-(%d,%d), request (%d,%d)-(%d,%d), ds %.3f/%.3f%s",
            tSec, res.elements.size(), pre->copies.size(), res_rect.left, res_rect.top, res_rect.right, res_rect.bottom,
            want.left, want.top, want.right, want.bottom, dsx, dsy, res.warnings.empty() ? "" : " (warnings)");
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

// AE's effect worlds hold straight (unpremultiplied) alpha (src/core/raster.h): filtering and "over" are done
// premultiplied, and the result goes back to straight once per pixel.
template <class P>
inline Px Load(const P& p)
{
    const float s = 1.0f / Fmt<P>::kMax;
    const float a = p.alpha * s;
    return {a, p.red * s * a, p.green * s * a, p.blue * s * a};
}
template <class T>
inline T Clamp(float v, float mx)
{
    v = v * mx + 0.5f;
    if (v < 0) v = 0;
    if (v > mx) v = mx;
    return (T)v;
}
inline Px Straight(const Px& v)
{
    if (!(v.a > 0)) return {0, 0, 0, 0};
    const float k = 1.0f / v.a;
    return {v.a, v.r * k, v.g * k, v.b * k};
}
inline void Store(PF_Pixel8& p, const Px& pm)
{
    const Px v = Straight(pm);
    p.alpha = Clamp<A_u_char>(v.a, 255), p.red = Clamp<A_u_char>(v.r, 255), p.green = Clamp<A_u_char>(v.g, 255),
    p.blue = Clamp<A_u_char>(v.b, 255);
}
inline void Store(PF_Pixel16& p, const Px& pm)
{
    const Px v = Straight(pm);
    p.alpha = Clamp<A_u_short>(v.a, 32768), p.red = Clamp<A_u_short>(v.r, 32768),
    p.green = Clamp<A_u_short>(v.g, 32768), p.blue = Clamp<A_u_short>(v.b, 32768);
}
inline void Store(PF_PixelFloat& p, const Px& pm)
{
    const Px v = Straight(pm);
    p.alpha = v.a, p.red = v.r, p.green = v.g, p.blue = v.b;
}

template <class P>
inline const P& At(const PF_EffectWorld* w, int x, int y)
{
    return *(const P*)((const char*)w->data + (size_t)y * w->rowbytes + (size_t)x * sizeof(P));
}

// bilinear, transparent outside the world; (x, y) in the world's pixels (centres at +0.5)
template <class P>
Px Sample(const PF_EffectWorld* w, double x, double y)
{
    x -= 0.5, y -= 0.5;
    const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const float fx = (float)(x - x0), fy = (float)(y - y0);
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

template <class P>
void DrawRows(PF_EffectWorld* out, const Pre& pre, const PF_EffectWorld* const* worlds, int yBegin, int yEnd)
{
    const int ox = pre.out.left, oy = pre.out.top;
    std::vector<Px> acc((size_t)out->width);  // the row, premultiplied
    for (int y = yBegin; y < yEnd; y++) {
        P* row = (P*)((char*)out->data + (size_t)y * out->rowbytes);
        std::fill(acc.begin(), acc.end(), Px{0, 0, 0, 0});
        bool any = false;
        const int Y = y + oy;
        for (const Copy& c : pre.copies) {
            if (Y < c.y0 || Y >= c.y1) continue;
            const PF_EffectWorld* w = worlds[c.src];
            if (!w) continue;
            const int xa = std::max(0, c.x0 - ox), xb = std::min((int)out->width, c.x1 - ox);
            const double step = 1.0 / c.n;
            const float k = c.opacity / (float)(c.n * c.n);
            for (int x = xa; x < xb; x++) {
                Px s = {0, 0, 0, 0};
                for (int j = 0; j < c.n; j++)
                    for (int i = 0; i < c.n; i++) {
                        const double dx = x + ox + (i + 0.5) * step - c.px, dy = Y + (j + 0.5) * step - c.py;
                        const double sx = c.pivX + c.ia * dx + c.ib * dy, sy = c.pivY + c.ic * dx + c.id * dy;
                        if (sx < -1 || sy < -1 || sx > w->width + 1 || sy > w->height + 1) continue;
                        const Px q = Sample<P>(w, sx, sy);
                        s.a += q.a, s.r += q.r, s.g += q.g, s.b += q.b;
                    }
                if (s.a <= 0) continue;
                s.a *= k, s.r *= k, s.g *= k, s.b *= k;
                Px& d = acc[(size_t)x];
                const float t = 1.0f - s.a;  // premultiplied "over"
                d.a = s.a + d.a * t, d.r = s.r + d.r * t, d.g = s.g + d.g * t, d.b = s.b + d.b * t;
                any = true;
            }
        }
        if (any)
            for (int x = 0; x < out->width; x++) Store(row[x], acc[(size_t)x]);
    }
}

template <class P>
void Draw(PF_EffectWorld* out, const Pre& pre, const PF_EffectWorld* const* worlds)
{
    for (int y = 0; y < out->height; y++) memset((char*)out->data + (size_t)y * out->rowbytes, 0, (size_t)out->width * sizeof(P));
    if (pre.copies.empty()) return;
    const int threads = std::clamp((int)std::thread::hardware_concurrency(), 1, 16);
    const int rows = out->height, chunk = (rows + threads - 1) / threads;
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; t++) {
        const int a = t * chunk, b = std::min(rows, a + chunk);
        if (a < b) pool.emplace_back([=, &pre] { DrawRows<P>(out, pre, worlds, a, b); });
    }
    DrawRows<P>(out, pre, worlds, 0, std::min(rows, chunk));
    for (auto& th : pool) th.join();
}

}  // namespace

PF_Err SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra)
{
    (void)out_data;
    const Pre* pre = (const Pre*)extra->input->pre_render_data;
    PF_EffectWorld* outW = nullptr;
    PF_EffectWorld* worlds[kLayers + 1] = {};
    PF_Err err = PF_Err_NONE;
    // the layers' pixels before the output (the other way round, AE answered BAD_CALLBACK_PARAM for the input)
    if (pre) {
        if (pre->has[kOwn]) {
            const PF_Err e = extra->cb->checkout_layer_pixels(in_data->effect_ref, kInputId, &worlds[kOwn]);
            if (e || !worlds[kOwn]) {
                Log("render: this layer's pixels: err %d (world %p)", (int)e, (void*)worlds[kOwn]);
                worlds[kOwn] = nullptr;
            }
        }
        for (int k = 0; k < kLayers; k++)
            if (pre->has[k] && extra->cb->checkout_layer_pixels(in_data->effect_ref, 1 + k, &worlds[k])) worlds[k] = nullptr;
    }
    err = extra->cb->checkout_output(in_data->effect_ref, &outW);
    if (err) Log("render: output failed (%d)", (int)err);
    if (!err && outW && pre) {
        PF_WorldSuite2* ws = nullptr;
        PF_PixelFormat fmt = PF_PixelFormat_INVALID;
        if (in_data->pica_basicP->AcquireSuite(kPFWorldSuite, kPFWorldSuiteVersion2, (const void**)&ws) == kSPNoError && ws) {
            ws->PF_GetPixelFormat(outW, &fmt);
            in_data->pica_basicP->ReleaseSuite(kPFWorldSuite, kPFWorldSuiteVersion2);
        }
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 50)
            Log("render: %zu copies, format %d, output %d x %d, own layer %s %d x %d", pre->copies.size(), (int)fmt,
                (int)outW->width, (int)outW->height, worlds[kOwn] ? "in" : "none", worlds[kOwn] ? (int)worlds[kOwn]->width : 0,
                worlds[kOwn] ? (int)worlds[kOwn]->height : 0);
        // every layer has the output's depth (AE converts layers to the project's depth)
        switch (fmt) {
        case PF_PixelFormat_ARGB32: Draw<PF_Pixel8>(outW, *pre, worlds); break;
        case PF_PixelFormat_ARGB64: Draw<PF_Pixel16>(outW, *pre, worlds); break;
        case PF_PixelFormat_ARGB128: Draw<PF_PixelFloat>(outW, *pre, worlds); break;
        default:
            Log("render: unknown pixel format %d", (int)fmt);
            err = PF_Err_BAD_CALLBACK_PARAM;
            break;
        }
    }
    if (err) Log("render: failed (%d)", (int)err);
    for (int k = 0; k < kLayers; k++)
        if (worlds[k]) extra->cb->checkin_layer_pixels(in_data->effect_ref, 1 + k);
    if (worlds[kOwn]) extra->cb->checkin_layer_pixels(in_data->effect_ref, kInputId);
    return err;
}

}  // namespace dx2
