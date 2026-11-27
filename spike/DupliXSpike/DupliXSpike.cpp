// THROWAWAY SPIKE — not product code.
//
// S1: can an effect on a small 2D layer return a comp-sized buffer that lines up
//     with comp coordinates, whatever the layer's own transform is?
// S2: can we declare 64 layer params, and what does checkout_layer return for them?
//     Do expressions on POINT_3D params give us toComp() positions?
//
// Everything interesting is written to the log file named by %DUPLIX_SPIKE_LOG%.

#include "DupliXSpike.h"

#include <algorithm>
#include <atomic>
#include <intrin.h>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <psapi.h>   // K32EnumProcessModules (S5c)

// ---------------------------------------------------------------- logging
static std::mutex g_logMutex;

static void Log(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    char path[1024] = {0};
    DWORD n = GetEnvironmentVariableA("DUPLIX_SPIKE_LOG", path, sizeof(path));
    if (n == 0 || n >= sizeof(path)) {
        GetTempPathA(sizeof(path), path);
        strcat_s(path, "duplix_spike.log");
    }
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") != 0 || !f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d][pid %lu tid %lu] ", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ---------------------------------------------------------------- math
struct V2 { double x, y; };

// comp = o + a*u + b*v   (u, v in full-res layer pixels)
struct Affine2 {
    V2 o, a, b;
    V2 Fwd(double u, double v) const { return {o.x + a.x * u + b.x * v, o.y + a.y * u + b.y * v}; }
    bool Inv(double cx, double cy, double* u, double* v) const
    {
        double det = a.x * b.y - b.x * a.y;
        if (std::fabs(det) < 1e-12) return false;
        double dx = cx - o.x, dy = cy - o.y;
        *u = (dx * b.y - b.x * dy) / det;
        *v = (a.x * dy - dx * a.y) / det;
        return true;
    }
};

// Perspective-correct screen -> plane mapping.
// For a plane under perspective, 1/z, u/z and v/z are affine in screen (x, y).
// Three corners with known (screen x, y, camera depth z, plane u, v) determine them exactly.
struct PlaneMap {
    double iz[3], uz[3], vz[3];   // coefficients a*x + b*y + c
    bool ok = false;

    static bool Solve3(const double x[3], const double y[3], const double f[3], double out[3])
    {
        // [x y 1] * [a b c]^T = f
        double det = x[0] * (y[1] - y[2]) - y[0] * (x[1] - x[2]) + (x[1] * y[2] - x[2] * y[1]);
        if (std::fabs(det) < 1e-12) return false;
        out[0] = (f[0] * (y[1] - y[2]) - y[0] * (f[1] - f[2]) + (f[1] * y[2] - f[2] * y[1])) / det;
        out[1] = (x[0] * (f[1] - f[2]) - f[0] * (x[1] - x[2]) + (x[1] * f[2] - x[2] * f[1])) / det;
        out[2] = (x[0] * (y[1] * f[2] - y[2] * f[1]) - y[0] * (x[1] * f[2] - x[2] * f[1]) +
                  f[0] * (x[1] * y[2] - x[2] * y[1])) / det;
        return true;
    }

    // corners: plane (0,0), (100,0), (0,100); p.z = camera-space depth
    void Build(const PF_Point3DDef& c0, const PF_Point3DDef& c1, const PF_Point3DDef& c2)
    {
        const double x[3] = {c0.x_value, c1.x_value, c2.x_value};
        const double y[3] = {c0.y_value, c1.y_value, c2.y_value};
        const double z[3] = {c0.z_value, c1.z_value, c2.z_value};
        if (z[0] <= 0 || z[1] <= 0 || z[2] <= 0) { ok = false; return; }
        const double u[3] = {0, 100, 0}, v[3] = {0, 0, 100};
        double fiz[3], fuz[3], fvz[3];
        for (int i = 0; i < 3; ++i) { fiz[i] = 1 / z[i]; fuz[i] = u[i] / z[i]; fvz[i] = v[i] / z[i]; }
        ok = Solve3(x, y, fiz, iz) && Solve3(x, y, fuz, uz) && Solve3(x, y, fvz, vz);
    }

    bool Map(double sx, double sy, double* u, double* v, double* depth) const
    {
        if (!ok) return false;
        double w = iz[0] * sx + iz[1] * sy + iz[2];
        if (w <= 0) return false;
        *u = (uz[0] * sx + uz[1] * sy + uz[2]) / w;
        *v = (vz[0] * sx + vz[1] * sy + vz[2]) / w;
        *depth = 1 / w;
        return true;
    }
};

static Affine2 MakeAffine(const PF_Point3DDef& o, const PF_Point3DDef& x100, const PF_Point3DDef& y100)
{
    Affine2 m;
    m.o = {o.x_value, o.y_value};
    m.a = {(x100.x_value - o.x_value) / 100.0, (x100.y_value - o.y_value) / 100.0};
    m.b = {(y100.x_value - o.x_value) / 100.0, (y100.y_value - o.y_value) / 100.0};
    return m;
}

// ---------------------------------------------------------------- pixels (premultiplied)
struct RGBA { float r, g, b, a; };

static RGBA GetPx(const PF_EffectWorld* w, int bpc, int x, int y)
{
    const char* row = (const char*)w->data + (size_t)y * w->rowbytes;
    if (bpc == 32) {
        const PF_PixelFloat* p = (const PF_PixelFloat*)row + x;
        return {p->red, p->green, p->blue, p->alpha};
    } else if (bpc == 16) {
        const PF_Pixel16* p = (const PF_Pixel16*)row + x;
        const float k = 1.0f / PF_MAX_CHAN16;
        return {p->red * k, p->green * k, p->blue * k, p->alpha * k};
    } else {
        const PF_Pixel8* p = (const PF_Pixel8*)row + x;
        const float k = 1.0f / PF_MAX_CHAN8;
        return {p->red * k, p->green * k, p->blue * k, p->alpha * k};
    }
}

static void PutPx(PF_EffectWorld* w, int bpc, int x, int y, RGBA c)
{
    char* row = (char*)w->data + (size_t)y * w->rowbytes;
    auto clamp01 = [](float v) { return v < 0 ? 0.0f : (v > 1 ? 1.0f : v); };
    if (bpc == 32) {
        PF_PixelFloat* p = (PF_PixelFloat*)row + x;
        p->red = c.r; p->green = c.g; p->blue = c.b; p->alpha = c.a;
    } else if (bpc == 16) {
        PF_Pixel16* p = (PF_Pixel16*)row + x;
        p->red = (A_u_short)(clamp01(c.r) * PF_MAX_CHAN16 + 0.5f);
        p->green = (A_u_short)(clamp01(c.g) * PF_MAX_CHAN16 + 0.5f);
        p->blue = (A_u_short)(clamp01(c.b) * PF_MAX_CHAN16 + 0.5f);
        p->alpha = (A_u_short)(clamp01(c.a) * PF_MAX_CHAN16 + 0.5f);
    } else {
        PF_Pixel8* p = (PF_Pixel8*)row + x;
        p->red = (A_u_char)(clamp01(c.r) * PF_MAX_CHAN8 + 0.5f);
        p->green = (A_u_char)(clamp01(c.g) * PF_MAX_CHAN8 + 0.5f);
        p->blue = (A_u_char)(clamp01(c.b) * PF_MAX_CHAN8 + 0.5f);
        p->alpha = (A_u_char)(clamp01(c.a) * PF_MAX_CHAN8 + 0.5f);
    }
}

// ---------------------------------------------------------------- S5: AEGP helpers
// S5: render the layer named "Target" through AEGP (AE's own render of the layer, which
// should include its masks and effects), instead of the layer-param checkout ("source").
static AEGP_PluginID g_pluginId = 0;
static bool g_registered = false;

template <class T>
struct Suite {
    SPBasicSuite* sp;
    const char* name;
    int32 ver;
    T* p = nullptr;
    Suite(SPBasicSuite* s, const char* n, int32 v) : sp(s), name(n), ver(v)
    {
        const void* q = nullptr;
        if (sp && sp->AcquireSuite(name, ver, &q) == kSPNoError) p = (T*)q;
    }
    ~Suite() { if (p) sp->ReleaseSuite(name, ver); }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};
#define SUITE(T, NAME, VER) Suite<T>(in_data->pica_basicP, NAME, VER)

static double NowMs()
{
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static std::string Utf16ToUtf8(const A_UTF16Char* s)
{
    if (!s) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t*)s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, (const wchar_t*)s, -1, &out[0], n, nullptr, nullptr);
    return out;
}

static std::string MemHandleUtf16(SPBasicSuite* sp, AEGP_MemHandle h)
{
    if (!h) return std::string();
    Suite<AEGP_MemorySuite1> mem(sp, kAEGPMemorySuite, kAEGPMemorySuiteVersion1);
    std::string s;
    if (mem) {
        void* ptr = nullptr;
        if (mem->AEGP_LockMemHandle(h, &ptr) == A_Err_NONE && ptr) {
            s = Utf16ToUtf8((const A_UTF16Char*)ptr);
            mem->AEGP_UnlockMemHandle(h);
        }
        mem->AEGP_FreeMemHandle(h);
    }
    return s;
}

static void EnsureRegistered(PF_InData* in_data, const char* where)
{
    if (g_registered) return;
    auto util = SUITE(AEGP_UtilitySuite6, kAEGPUtilitySuite, kAEGPUtilitySuiteVersion6);
    if (!util) { Log("S5 register(%s): no UtilitySuite6", where); return; }
    A_Err e = util->AEGP_RegisterWithAEGP(nullptr, "DupliXSpike", &g_pluginId);
    g_registered = (e == A_Err_NONE);
    Log("S5 register(%s): err=%d plugin_id=%d", where, e, (int)g_pluginId);
}

// A read-only view of pixels, either from a PF world or from an AEGP world.
struct Img {
    const char* base = nullptr;
    size_t rowbytes = 0;
    int w = 0, h = 0;
    int offX = 0, offY = 0;   // layer coords of pixel (0,0), full-res
    int bpc = 8;
    bool compSpace = false;   // pixels are already in comp coords (text/shape: AE projects them)
};

static RGBA GetPxImg(const Img& im, int x, int y)
{
    const char* row = im.base + (size_t)y * im.rowbytes;
    if (im.bpc == 32) {
        const PF_PixelFloat* p = (const PF_PixelFloat*)row + x;
        return {p->red, p->green, p->blue, p->alpha};
    } else if (im.bpc == 16) {
        const PF_Pixel16* p = (const PF_Pixel16*)row + x;
        const float k = 1.0f / PF_MAX_CHAN16;
        return {p->red * k, p->green * k, p->blue * k, p->alpha * k};
    } else {
        const PF_Pixel8* p = (const PF_Pixel8*)row + x;
        const float k = 1.0f / PF_MAX_CHAN8;
        return {p->red * k, p->green * k, p->blue * k, p->alpha * k};
    }
}

static void LogOpaque(const char* tag, const Img& im)
{
    long n = 0;
    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x)
            if (GetPxImg(im, x, y).a > 0.5f) {
                ++n; x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
            }
    if (n) Log("  %s opaque px=%ld bbox(buffer) L%d T%d R%d B%d  bbox(+off) L%d T%d R%d B%d", tag, n, x0, y0, x1 + 1,
               y1 + 1, x0 + im.offX, y0 + im.offY, x1 + 1 + im.offX, y1 + 1 + im.offY);
    else Log("  %s opaque px=0", tag);
}

// Holds an AEGP frame receipt; checked in when the pre-render data is deleted.
struct AegpFrame {
    SPBasicSuite* sp = nullptr;
    AEGP_FrameReceiptH receipt = nullptr;
    Img img;
    bool ok = false;
    bool noWorld = false;   // render succeeded but returned no pixels (collapsed precomp)
    void Release()
    {
        if (receipt && sp) {
            Suite<AEGP_RenderSuite5> rs(sp, kAEGPRenderSuite, kAEGPRenderSuiteVersion5);
            if (rs) {
                A_Err e = rs->AEGP_CheckinFrame(receipt);
                if (e) Log("S5 CheckinFrame err=%d", e);
            }
        }
        receipt = nullptr;
        ok = false;
    }
};

static bool FindTarget(PF_InData* in_data, AEGP_LayerH* outLayer, std::string* compName, A_long* targetIndex)
{
    *outLayer = nullptr;
    auto pfi = SUITE(AEGP_PFInterfaceSuite1, kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1);
    auto ls = SUITE(AEGP_LayerSuite9, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    auto cs = SUITE(AEGP_CompSuite12, kAEGPCompSuite, kAEGPCompSuiteVersion12);
    auto is = SUITE(AEGP_ItemSuite9, kAEGPItemSuite, kAEGPItemSuiteVersion9);
    if (!pfi || !ls || !cs || !is) { Log("S5 FindTarget: suite missing"); return false; }
    AEGP_LayerH self = nullptr;
    AEGP_CompH comp = nullptr;
    A_Err e = pfi->AEGP_GetEffectLayer(in_data->effect_ref, &self);
    if (!e) e = ls->AEGP_GetLayerParentComp(self, &comp);
    if (e || !comp) { Log("S5 FindTarget: GetEffectLayer/ParentComp err=%d", e); return false; }
    AEGP_ItemH compItem = nullptr;
    if (!cs->AEGP_GetItemFromComp(comp, &compItem) && compItem) {
        AEGP_MemHandle nm = nullptr;
        if (!is->AEGP_GetItemName(g_pluginId, compItem, &nm)) *compName = MemHandleUtf16(in_data->pica_basicP, nm);
    }
    A_long n = 0;
    ls->AEGP_GetCompNumLayers(comp, &n);
    for (A_long i = 0; i < n; ++i) {
        AEGP_LayerH L = nullptr;
        if (ls->AEGP_GetCompLayerByIndex(comp, i, &L) || !L || L == self) continue;
        AEGP_MemHandle nm = nullptr, src = nullptr;
        if (ls->AEGP_GetLayerName(g_pluginId, L, &nm, &src)) continue;
        std::string name = MemHandleUtf16(in_data->pica_basicP, nm);
        std::string srcName = MemHandleUtf16(in_data->pica_basicP, src);
        if (name.empty()) name = srcName;   // a solid named at creation has only a source name
        if (name == "Target") { *outLayer = L; *targetIndex = i; return true; }
    }
    Log("S5 FindTarget: no layer named Target in comp '%s' (%d layers)", compName->c_str(), n);
    return false;
}

// ---------------------------------------------------------------- S5c: widen the layer-render ROI
// AEGP_NewFromLayer (AEGPDriver.aex) sets the ROI to the layer's nominal bounds via the exported
// C++ method BEE_RenderOptions::SetROI(const M_Rect&). AEGP_LayerRenderOptionsH is the
// BEE_LayerRenderOptions* itself. M_Rect = 4 x int16 {top, left, bottom, right}
// (from AEGP_SetRegionOfInterest / AEGP_GetRegionOfInterest in AEGPDriver.aex).
struct MRect { short top, left, bottom, right; };
typedef void (*BeeSetROI)(void* self, const MRect* r);
typedef const MRect* (*BeeGetROI)(const void* self);
static BeeSetROI g_setROI = nullptr;
static BeeGetROI g_getROI = nullptr;
static std::string g_roiModule;

static void ResolveBeeROI()
{
    static bool done = [] {
        HMODULE mods[1024];
        DWORD need = 0;
        if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return true;
        for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; ++i) {
            FARPROC s = GetProcAddress(mods[i], "?SetROI@BEE_RenderOptions@@QEAAXAEBUM_Rect@@@Z");
            FARPROC g = GetProcAddress(mods[i], "?GetROI@BEE_RenderOptions@@QEBAAEBUM_Rect@@XZ");
            if (s && g) {
                g_setROI = (BeeSetROI)s;
                g_getROI = (BeeGetROI)g;
                char path[MAX_PATH] = {0};
                GetModuleFileNameA(mods[i], path, MAX_PATH);
                g_roiModule = path;
                break;
            }
        }
        return true;
    }();
    (void)done;
}

// DUPLIX_S5C_ROI: unset = "margin:30000", "none" = keep default, "margin:N" = grow by N px each side, "zero" = {0,0,0,0}
static void ApplyRoiExperiment(AEGP_LayerRenderOptionsH ro)
{
    ResolveBeeROI();
    if (!g_setROI || !g_getROI) { Log("  S5c ROI: BEE_RenderOptions::SetROI/GetROI not found"); return; }
    MRect d = *g_getROI(ro);
    char v[64] = {0};
    if (!GetEnvironmentVariableA("DUPLIX_S5C_ROI", v, sizeof(v)) || !v[0]) strcpy_s(v, "margin:30000");
    MRect n = d;
    int margin = 0;
    if (strncmp(v, "margin:", 7) == 0) {
        margin = atoi(v + 7);
        n.top = (short)std::max(-32000, d.top - margin);
        n.left = (short)std::max(-32000, d.left - margin);
        n.bottom = (short)std::min(32000, d.bottom + margin);
        n.right = (short)std::min(32000, d.right + margin);
        g_setROI(ro, &n);
    } else if (strcmp(v, "zero") == 0) {
        n = {0, 0, 0, 0};
        g_setROI(ro, &n);
    }
    // S5c-2: read-only look at other knobs that might control how far down the layer pipeline
    // AE renders (track matte?). Getters only.
    {
        typedef short (*BeeGetShort)(const void*);
        HMODULE bee = GetModuleHandleA("BEE.dll");
        auto gs = bee ? (BeeGetShort)GetProcAddress(bee, "?GetLayerStage@BEE_LayerRenderOptions@@QEBAFXZ") : nullptr;
        auto gt = bee ? (BeeGetShort)GetProcAddress(bee, "?GetLayerRenderType@BEE_LayerRenderOptions@@QEBAFXZ") : nullptr;
        auto ge = bee ? (BeeGetShort)GetProcAddress(bee, "?GetEffectsToRender@BEE_LayerRenderOptions@@QEBAFXZ") : nullptr;
        Log("  S5c knobs: LayerStage=%d LayerRenderType=%d EffectsToRender=%d", gs ? gs(ro) : -999,
            gt ? gt(ro) : -999, ge ? ge(ro) : -999);
        // DUPLIX_S5C_STAGE=N / DUPLIX_S5C_RTYPE=N: try other values (experiment only).
        typedef void (*BeeSetShort)(void*, short);
        char sv[16] = {0};
        if (bee && GetEnvironmentVariableA("DUPLIX_S5C_STAGE", sv, sizeof(sv)) && sv[0]) {
            auto ss = (BeeSetShort)GetProcAddress(bee, "?SetLayerStage@BEE_LayerRenderOptions@@QEAAXF@Z");
            if (ss) ss(ro, (short)atoi(sv));
        }
        if (bee && GetEnvironmentVariableA("DUPLIX_S5C_RTYPE", sv, sizeof(sv)) && sv[0]) {
            auto st = (BeeSetShort)GetProcAddress(bee, "?SetLayerRenderType@BEE_LayerRenderOptions@@QEAAXF@Z");
            if (st) st(ro, (short)atoi(sv));
        }
        Log("  S5c knobs after: LayerStage=%d LayerRenderType=%d", gs ? gs(ro) : -999, gt ? gt(ro) : -999);
    }
    MRect a = *g_getROI(ro);
    Log("  S5c ROI mode='%s' (%s) default T%d L%d B%d R%d -> now T%d L%d B%d R%d", v, g_roiModule.c_str(), d.top,
        d.left, d.bottom, d.right, a.top, a.left, a.bottom, a.right);
}

static thread_local bool t_quietOpaque = false;

// Render the target layer through AEGP. If mixGuid, feed the receipt GUID into the cache key.
static bool RenderTarget(PF_InData* in_data, AEGP_LayerH L, int bpc, const char* where, AegpFrame* fr,
                         void (*mix)(PF_InData*, void*, const void*, size_t), void* mixCtx)
{
    fr->sp = in_data->pica_basicP;
    auto lro = SUITE(AEGP_LayerRenderOptionsSuite2, kAEGPLayerRenderOptionsSuite, kAEGPLayerRenderOptionsSuiteVersion2);
    auto rs = SUITE(AEGP_RenderSuite5, kAEGPRenderSuite, kAEGPRenderSuiteVersion5);
    auto ws = SUITE(AEGP_WorldSuite3, kAEGPWorldSuite, kAEGPWorldSuiteVersion3);
    auto pfi = SUITE(AEGP_PFInterfaceSuite1, kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1);
    auto ls = SUITE(AEGP_LayerSuite9, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    if (!lro || !rs || !ws || !pfi || !ls) { Log("S5 %s: suite missing", where); return false; }

    AEGP_ObjectType ot = 0;
    A_Boolean hasMatte = FALSE, is3d = FALSE;
    ls->AEGP_GetLayerObjectType(L, &ot);
    ls->AEGP_DoesLayerHaveTrackMatte(L, &hasMatte);
    ls->AEGP_IsLayer3D(L, &is3d);

    A_Time compTime = {0, 1};
    A_Err e0 = pfi->AEGP_ConvertEffectToCompTime(in_data->effect_ref, in_data->current_time, in_data->time_scale,
                                                 &compTime);
    AEGP_LayerRenderOptionsH ro = nullptr;
    double t0 = NowMs();
    A_Err e1 = lro->AEGP_NewFromLayer(g_pluginId, L, &ro);
    A_Time defTime = {0, 1};
    if (!e1) lro->AEGP_GetTime(ro, &defTime);
    A_Err e2 = 0, e3 = 0, e4 = 0, e5 = 0;
    if (!e1) {
        // Unverified: whether SetTime wants comp time or layer time (all test layers start at 0).
        e2 = lro->AEGP_SetTime(ro, compTime);
        A_Time step = {in_data->time_step, (A_u_long)in_data->time_scale};
        e3 = lro->AEGP_SetTimeStep(ro, step);
        AEGP_WorldType wt = bpc == 32 ? AEGP_WorldType_32 : (bpc == 16 ? AEGP_WorldType_16 : AEGP_WorldType_8);
        e4 = lro->AEGP_SetWorldType(ro, wt);
        A_short dx = (A_short)std::max(1, (int)(in_data->downsample_x.den / std::max(1, (int)in_data->downsample_x.num)));
        A_short dy = (A_short)std::max(1, (int)(in_data->downsample_y.den / std::max(1, (int)in_data->downsample_y.num)));
        e5 = lro->AEGP_SetDownsampleFactor(ro, dx, dy);
        ApplyRoiExperiment(ro);
    }
    double t1 = NowMs();
    A_Err er = e1 ? e1 : rs->AEGP_RenderAndCheckoutLayerFrame(ro, nullptr, nullptr, &fr->receipt);
    double t2 = NowMs();
    if (ro) lro->AEGP_Dispose(ro);
    Log("S5 %s: objtype=%d 3d=%d hasMatte=%d effTime=%d/%d -> compTime=%d/%u (e%d) defaultTime=%d/%u "
        "NewFromLayer=%d SetTime=%d Step=%d WType=%d DS=%d Render=%d  setup %.1fms render %.1fms",
        where, (int)ot, (int)is3d, (int)hasMatte, in_data->current_time, in_data->time_scale, compTime.value,
        compTime.scale, e0, defTime.value, defTime.scale, e1, e2, e3, e4, e5, er, t1 - t0, t2 - t1);
    if (er || !fr->receipt) return false;

    AEGP_WorldH w = nullptr;
    Log("  -> GetReceiptWorld");
    A_Err eg = rs->AEGP_GetReceiptWorld(fr->receipt, &w);
    A_LRect reg = {0, 0, 0, 0};
    A_Err erg = -1;
    // S5 run 3: a collapsed precomp renders "successfully" with a NULL world, and calling
    // GetRenderedRegion on that receipt crashes AE. Never touch the receipt further then.
    if (!eg && w) {
        Log("  -> GetRenderedRegion (world=%p)", (void*)w);
        erg = rs->AEGP_GetRenderedRegion(fr->receipt, &reg);
    } else {
        Log("  receipt has no world (err=%d): skipping GetRenderedRegion", eg);
        fr->noWorld = true;
    }
    Log("  GetReceiptWorld=%d world=%p  GetRenderedRegion=%d L%d T%d R%d B%d", eg, (void*)w, erg, reg.left, reg.top,
        reg.right, reg.bottom);

    if (mix && (eg || !w)) {
        static const char kNoWorld[] = "S5 no world";
        mix(in_data, mixCtx, kNoWorld, sizeof(kNoWorld));
    } else if (mix) {
        Log("  -> GetReceiptGuid");
        AEGP_MemHandle gm = nullptr;
        A_Err eguid = rs->AEGP_GetReceiptGuid(fr->receipt, &gm);
        if (!eguid && gm) {
            Suite<AEGP_MemorySuite1> mem(in_data->pica_basicP, kAEGPMemorySuite, kAEGPMemorySuiteVersion1);
            void* p = nullptr;
            AEGP_MemSize sz = 0;
            mem->AEGP_GetMemHandleSize(gm, &sz);
            if (!mem->AEGP_LockMemHandle(gm, &p) && p) {
                const unsigned char* b = (const unsigned char*)p;
                char hex[80] = {0};
                for (int i = 0; i < (int)std::min<AEGP_MemSize>(sz, 32); ++i) sprintf_s(hex + i * 2, 3, "%02x", b[i]);
                Log("  receipt GUID (%u bytes) %s", (unsigned)sz, hex);
                mix(in_data, mixCtx, p, (size_t)sz);
                mem->AEGP_UnlockMemHandle(gm);
            }
            mem->AEGP_FreeMemHandle(gm);
        } else {
            Log("  GetReceiptGuid err=%d", eguid);
        }
    }

    if (eg || !w) return false;
    Log("  -> world info");
    A_long ww = 0, wh = 0;
    A_u_long rb = 0;
    AEGP_WorldType wt = 0;
    ws->AEGP_GetSize(w, &ww, &wh);
    ws->AEGP_GetRowBytes(w, &rb);
    ws->AEGP_GetType(w, &wt);
    void* base = nullptr;
    A_Err eb = 0;
    int wbpc = 8;
    if (wt == AEGP_WorldType_32) { eb = ws->AEGP_GetBaseAddr32(w, (PF_PixelFloat**)&base); wbpc = 32; }
    else if (wt == AEGP_WorldType_16) { eb = ws->AEGP_GetBaseAddr16(w, (PF_Pixel16**)&base); wbpc = 16; }
    else { eb = ws->AEGP_GetBaseAddr8(w, (PF_Pixel8**)&base); wbpc = 8; }
    Log("  world %dx%d rowbytes=%u type=%d base err=%d", ww, wh, rb, (int)wt, eb);
    if (eb || !base) return false;
    fr->img.base = (const char*)base;
    fr->img.rowbytes = rb;
    fr->img.w = ww;
    fr->img.h = wh;
    fr->img.bpc = wbpc;
    // If the world is exactly the rendered region, its pixel (0,0) is the region's top-left.
    bool regMatches = (reg.right - reg.left == ww) && (reg.bottom - reg.top == wh);
    fr->img.offX = regMatches ? reg.left : 0;
    fr->img.offY = regMatches ? reg.top : 0;
    // Seen in S5 run 1: text and shape layers come back as a comp-sized, already projected image.
    // S5c: with a widened ROI, a collapsed precomp also comes back comp-space (projected).
    AEGP_LayerFlags lflags = 0;
    ls->AEGP_GetLayerFlags(L, &lflags);
    const bool collapsed = (lflags & AEGP_LayerFlag_COLLAPSE) != 0;
    fr->img.compSpace = (ot == AEGP_ObjectType_TEXT || ot == AEGP_ObjectType_VECTOR || collapsed);
    Log("  using offset (%d,%d) (region %s world size) compSpace=%d collapsed=%d", fr->img.offX, fr->img.offY,
        regMatches ? "==" : "!=", (int)fr->img.compSpace, (int)collapsed);
    if (!t_quietOpaque) LogOpaque("aegp", fr->img);   // full-buffer scan: skipped for many layers (S7)
    fr->ok = true;
    return true;
}

// ---------------------------------------------------------------- S5b: geometry from AEGP
// The corner expressions (toComp / fromWorld) cost 300-700 ms per frame (S4). Compute the same
// numbers from AEGP instead: layer -> world matrix, then the effect camera (world -> camera,
// perspective divide by camera z, scaled by the distance to the image plane).
// A_Matrix4 is row-major with row vectors: p' = [x y z 1] * M.
// DUPLIX_S5B: unset/"0" = expressions only, "compare" = read both and log the difference
// (geometry from AEGP), "1" = AEGP only (expression params are never checked out).
static int S5bMode()
{
    char v[16] = {0};
    DWORD n = GetEnvironmentVariableA("DUPLIX_S5B", v, sizeof(v));
    if (n == 0 || n >= sizeof(v)) return 0;
    if (_stricmp(v, "compare") == 0) return 2;
    return v[0] == '1' ? 1 : 0;
}

static void XformPt(const A_Matrix4& m, double x, double y, double z, double o[3])
{
    for (int i = 0; i < 3; ++i) o[i] = x * m.mat[0][i] + y * m.mat[1][i] + z * m.mat[2][i] + m.mat[3][i];
}

static bool Invert4(const A_Matrix4& in, A_Matrix4* out)
{
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? in.mat[r][c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-12) return false;
        if (p != c) for (int k = 0; k < 8; ++k) std::swap(a[p][k], a[c][k]);
        const double d = a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = a[r][c];
            for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out->mat[r][c] = a[r][c + 4];
    return true;
}

struct AegpGeom {
    bool ok = false;
    PF_Point3DDef cs = {}, so = {}, sx = {}, sy = {}, c[3] = {}, m[3] = {};
};

// Same values the expressions produce: [comp x, comp y, camera-space z] of layer pixels
// (0,0), (100,0), (0,100). 2D layers: world == comp, z = 1 (affine).
static bool LayerCorners(PF_InData* in_data, AEGP_LayerH L, const A_Time& t, const A_Matrix4& w2c, double dist,
                         double cw, double ch, bool haveCam, PF_Point3DDef out[3])
{
    auto ls = SUITE(AEGP_LayerSuite9, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    if (!ls || !L) return false;
    A_Matrix4 l2w;
    if (ls->AEGP_GetLayerToWorldXform(L, &t, &l2w)) return false;
    A_Boolean is3d = FALSE;
    ls->AEGP_IsLayer3D(L, &is3d);
    const double uv[3][2] = {{0, 0}, {100, 0}, {0, 100}};
    for (int i = 0; i < 3; ++i) {
        double w[3];
        XformPt(l2w, uv[i][0], uv[i][1], 0, w);
        if (is3d && haveCam) {
            double c[3];
            XformPt(w2c, w[0], w[1], w[2], c);
            if (c[2] <= 1e-9) return false;
            out[i].x_value = c[0] * dist / c[2] + cw / 2;
            out[i].y_value = c[1] * dist / c[2] + ch / 2;
            out[i].z_value = c[2];
        } else {
            out[i].x_value = w[0];
            out[i].y_value = w[1];
            out[i].z_value = 1;
        }
    }
    return true;
}

static bool GeomFromAegp(PF_InData* in_data, AEGP_LayerH target, AEGP_LayerH matte, AegpGeom* g)
{
    auto pfi = SUITE(AEGP_PFInterfaceSuite1, kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1);
    auto ls = SUITE(AEGP_LayerSuite9, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    auto cs = SUITE(AEGP_CompSuite12, kAEGPCompSuite, kAEGPCompSuiteVersion12);
    auto is = SUITE(AEGP_ItemSuite9, kAEGPItemSuite, kAEGPItemSuiteVersion9);
    if (!pfi || !ls || !cs || !is) return false;
    A_Time t = {0, 1};
    pfi->AEGP_ConvertEffectToCompTime(in_data->effect_ref, in_data->current_time, in_data->time_scale, &t);
    AEGP_LayerH self = nullptr;
    AEGP_CompH comp = nullptr;
    AEGP_ItemH ci = nullptr;
    if (pfi->AEGP_GetEffectLayer(in_data->effect_ref, &self) || ls->AEGP_GetLayerParentComp(self, &comp) ||
        cs->AEGP_GetItemFromComp(comp, &ci))
        return false;
    A_long cw = 0, ch = 0;
    is->AEGP_GetItemDimensions(ci, &cw, &ch);
    g->cs.x_value = cw;
    g->cs.y_value = ch;

    A_Matrix4 cam, w2c;
    A_FpLong dist = 0;
    A_short ipw = 0, iph = 0;
    AEGP_LayerH camL = nullptr;
    A_Err ec = pfi->AEGP_GetEffectCamera(in_data->effect_ref, &t, &camL);
    A_Err em = pfi->AEGP_GetEffectCameraMatrix(in_data->effect_ref, &t, &cam, &dist, &ipw, &iph);
    const bool haveCam = !em && dist > 0 && Invert4(cam, &w2c);
    Log("S5b camera: GetEffectCamera err=%d layer=%p  GetEffectCameraMatrix err=%d dist=%.3f plane=%dx%d  "
        "pos=(%.2f,%.2f,%.2f)",
        ec, (void*)camL, em, dist, ipw, iph, cam.mat[3][0], cam.mat[3][1], cam.mat[3][2]);

    PF_Point3DDef selfC[3];
    if (!LayerCorners(in_data, self, t, w2c, dist, cw, ch, haveCam, selfC)) return false;
    g->so = selfC[0];
    g->sx = selfC[1];
    g->sy = selfC[2];
    if (!LayerCorners(in_data, target, t, w2c, dist, cw, ch, haveCam, g->c)) return false;
    if (matte && !LayerCorners(in_data, matte, t, w2c, dist, cw, ch, haveCam, g->m)) return false;
    g->ok = true;
    return true;
}

static void LogDiff(const char* tag, const PF_Point3DDef& e, const PF_Point3DDef& a)
{
    Log("  S5b %-6s expr=(%.3f, %.3f, %.3f) aegp=(%.3f, %.3f, %.3f) diff=(%.4f, %.4f, %.4f)", tag, e.x_value,
        e.y_value, e.z_value, a.x_value, a.y_value, a.z_value, a.x_value - e.x_value, a.y_value - e.y_value,
        a.z_value - e.z_value);
}

// S4: the AEGP layer render does not include the layer's own opacity (AE applies it while
// compositing, like the track matte). Read it (0..1, after expressions) and apply it ourselves.
static double LayerOpacity(PF_InData* in_data, AEGP_LayerH L)
{
    auto ss = SUITE(AEGP_StreamSuite6, kAEGPStreamSuite, kAEGPStreamSuiteVersion6);
    auto pfi = SUITE(AEGP_PFInterfaceSuite1, kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1);
    if (!ss || !pfi || !L) return 1.0;
    A_Time t = {0, 1};
    pfi->AEGP_ConvertEffectToCompTime(in_data->effect_ref, in_data->current_time, in_data->time_scale, &t);
    AEGP_StreamRefH s = nullptr;
    double op = 1.0;
    if (!ss->AEGP_GetNewLayerStream(g_pluginId, L, AEGP_LayerStream_OPACITY, &s) && s) {
        AEGP_StreamValue2 v;
        AEFX_CLR_STRUCT(v);
        if (!ss->AEGP_GetNewStreamValue(g_pluginId, s, AEGP_LTimeMode_CompTime, &t, FALSE, &v)) {
            op = v.val.one_d / 100.0;
            ss->AEGP_DisposeStreamValue(&v);
        }
        ss->AEGP_DisposeStream(s);
    }
    return std::min(1.0, std::max(0.0, op));
}

// ---------------------------------------------------------------- S7: every layer is a target
// DUPLIX_S7=1: no "Target" by name. Every layer AE would draw (except ourselves, layers that
// carry this effect, and the spike's "BG") is rendered through AEGP; the overlay is their union.
static void MixIn(PF_InData* in_data, void* ctx, const void* data, size_t n);   // defined below

static bool S7Mode()
{
    char v[8] = {0};
    DWORD n = GetEnvironmentVariableA("DUPLIX_S7", v, sizeof(v));
    return n > 0 && n < sizeof(v) && v[0] == '1';
}

struct TargetInfo {
    AEGP_LayerH L = nullptr;
    AegpFrame img;
    PlaneMap plane;
    AEGP_LayerH matteL = nullptr;
    AegpFrame matte;
    PlaneMap mplane;
    AEGP_TrackMatte mode = AEGP_TrackMatte_NO_TRACK_MATTE;
    double opacity = 1.0, matteOpacity = 1.0;
    ~TargetInfo() { img.Release(); matte.Release(); }
};

// Camera / comp geometry, computed once per PreRender.
struct CamInfo {
    A_Time t = {0, 1};
    A_Matrix4 w2c = {};
    double dist = 0, cw = 0, ch = 0;
    bool haveCam = false;
};

static bool GetCamInfo(PF_InData* in_data, CamInfo* ci)
{
    auto pfi = SUITE(AEGP_PFInterfaceSuite1, kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1);
    auto ls = SUITE(AEGP_LayerSuite9, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    auto cs = SUITE(AEGP_CompSuite12, kAEGPCompSuite, kAEGPCompSuiteVersion12);
    auto is = SUITE(AEGP_ItemSuite9, kAEGPItemSuite, kAEGPItemSuiteVersion9);
    if (!pfi || !ls || !cs || !is) return false;
    pfi->AEGP_ConvertEffectToCompTime(in_data->effect_ref, in_data->current_time, in_data->time_scale, &ci->t);
    AEGP_LayerH self = nullptr;
    AEGP_CompH comp = nullptr;
    AEGP_ItemH item = nullptr;
    if (pfi->AEGP_GetEffectLayer(in_data->effect_ref, &self) || ls->AEGP_GetLayerParentComp(self, &comp) ||
        cs->AEGP_GetItemFromComp(comp, &item))
        return false;
    A_long w = 0, h = 0;
    is->AEGP_GetItemDimensions(item, &w, &h);
    ci->cw = w;
    ci->ch = h;
    A_Matrix4 cam;
    A_FpLong dist = 0;
    A_short ipw = 0, iph = 0;
    A_Err em = pfi->AEGP_GetEffectCameraMatrix(in_data->effect_ref, &ci->t, &cam, &dist, &ipw, &iph);
    ci->dist = dist;
    ci->haveCam = !em && dist > 0 && Invert4(cam, &ci->w2c);
    return true;
}

static bool CornersOf(PF_InData* in_data, AEGP_LayerH L, const CamInfo& ci, PF_Point3DDef out[3])
{
    return LayerCorners(in_data, L, ci.t, ci.w2c, ci.dist, ci.cw, ci.ch, ci.haveCam, out);
}

static bool LayerHasOurEffect(PF_InData* in_data, AEGP_LayerH L)
{
    auto es = SUITE(AEGP_EffectSuite5, kAEGPEffectSuite, kAEGPEffectSuiteVersion5);
    if (!es) return false;
    A_long n = 0;
    if (es->AEGP_GetLayerNumEffects(L, &n)) return false;
    for (A_long i = 0; i < n; ++i) {
        AEGP_EffectRefH ef = nullptr;
        if (es->AEGP_GetLayerEffectByIndex(g_pluginId, L, i, &ef) || !ef) continue;
        AEGP_InstalledEffectKey key = 0;
        A_char mn[AEGP_MAX_EFFECT_MATCH_NAME_SIZE + 1] = {0};
        bool ours = !es->AEGP_GetInstalledKeyFromLayerEffect(ef, &key) && !es->AEGP_GetEffectMatchName(key, mn) &&
                    strcmp(mn, "RNDM DupliXSpike") == 0;
        es->AEGP_DisposeEffect(ef);
        if (ours) return true;
    }
    return false;
}

// Why a layer is not a target (nullptr = it is one).
static const char* SkipReason(PF_InData* in_data, AEGP_LayerH L, AEGP_LayerH self, bool anySolo, const A_Time& t,
                              const std::string& name)
{
    auto ls = SUITE(AEGP_LayerSuite9, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    if (L == self) return "self";
    AEGP_ObjectType ot = 0;
    ls->AEGP_GetLayerObjectType(L, &ot);
    if (ot != AEGP_ObjectType_AV && ot != AEGP_ObjectType_TEXT && ot != AEGP_ObjectType_VECTOR) return "not drawable type";
    AEGP_LayerFlags f = 0;
    ls->AEGP_GetLayerFlags(L, &f);
    if (f & AEGP_LayerFlag_NULL_LAYER) return "null";
    if (f & AEGP_LayerFlag_GUIDE_LAYER) return "guide";
    if (f & AEGP_LayerFlag_ADJUSTMENT_LAYER) return "adjustment";
    if (anySolo && !(f & AEGP_LayerFlag_SOLO)) return "not solo";
    A_Boolean va = FALSE;
    ls->AEGP_IsVideoActive(L, AEGP_LTimeMode_CompTime, &t, &va);
    if (!va) return "video not active";
    if (name == "BG") return "spike BG";
    if (LayerHasOurEffect(in_data, L)) return "has this effect";
    return nullptr;
}

// Re-entrancy guard: rendering another layer through AEGP must never come back into our own
// PreRender for the same layer and time (another effect pulling our layer with its effects).
// thread_local catches same-thread nesting; the set catches nesting handed to another MFR thread.
static thread_local int t_s7Depth = 0;
static std::mutex g_s7Mutex;
static std::vector<std::pair<AEGP_LayerIDVal, A_long>> g_s7Active;

static void S7Collect(PF_InData* in_data, PF_PreRenderExtra* extra, std::vector<std::unique_ptr<TargetInfo>>* out,
                      PF_Point3DDef selfC[3], CamInfo* ci, std::string* compName)
{
    auto pfi = SUITE(AEGP_PFInterfaceSuite1, kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1);
    auto ls = SUITE(AEGP_LayerSuite9, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    auto cs = SUITE(AEGP_CompSuite12, kAEGPCompSuite, kAEGPCompSuiteVersion12);
    auto is = SUITE(AEGP_ItemSuite9, kAEGPItemSuite, kAEGPItemSuiteVersion9);
    if (!pfi || !ls || !cs || !is || !GetCamInfo(in_data, ci)) { Log("S7: setup failed"); return; }
    AEGP_LayerH self = nullptr;
    AEGP_CompH comp = nullptr;
    pfi->AEGP_GetEffectLayer(in_data->effect_ref, &self);
    ls->AEGP_GetLayerParentComp(self, &comp);
    AEGP_ItemH item = nullptr;
    if (!cs->AEGP_GetItemFromComp(comp, &item) && item) {
        AEGP_MemHandle nm = nullptr;
        if (!is->AEGP_GetItemName(g_pluginId, item, &nm)) *compName = MemHandleUtf16(in_data->pica_basicP, nm);
    }
    if (!CornersOf(in_data, self, *ci, selfC)) { Log("S7: self corners failed"); return; }

    AEGP_LayerIDVal selfId = 0;
    ls->AEGP_GetLayerID(self, &selfId);
    const std::pair<AEGP_LayerIDVal, A_long> me{selfId, ci->t.value * 1000 / (A_long)std::max<A_u_long>(1, ci->t.scale)};
    {
        std::lock_guard<std::mutex> lk(g_s7Mutex);
        bool nested = t_s7Depth > 0;
        for (auto& a : g_s7Active) if (a == me) nested = true;
        if (nested) {
            Log("S7: RE-ENTERED (depth=%d) comp='%s' -> no targets", t_s7Depth, compName->c_str());
            static const char kNested[] = "S7 nested";
            extra->cb->GuidMixInPtr(in_data->effect_ref, sizeof(kNested), kNested);
            return;
        }
        g_s7Active.push_back(me);
    }
    ++t_s7Depth;

    A_long n = 0;
    ls->AEGP_GetCompNumLayers(comp, &n);
    bool anySolo = false;
    for (A_long i = 0; i < n; ++i) {
        AEGP_LayerH L = nullptr;
        AEGP_LayerFlags f = 0;
        if (!ls->AEGP_GetCompLayerByIndex(comp, i, &L) && L && !ls->AEGP_GetLayerFlags(L, &f) &&
            (f & AEGP_LayerFlag_SOLO))
            anySolo = true;
    }
    const bool quiet = n > 8;
    t_quietOpaque = quiet;
    double tRender = 0;
    for (A_long i = 0; i < n; ++i) {
        AEGP_LayerH L = nullptr;
        if (ls->AEGP_GetCompLayerByIndex(comp, i, &L) || !L) continue;
        AEGP_MemHandle nm = nullptr, src = nullptr;
        std::string name;
        if (!ls->AEGP_GetLayerName(g_pluginId, L, &nm, &src)) {
            name = MemHandleUtf16(in_data->pica_basicP, nm);
            if (name.empty()) name = MemHandleUtf16(in_data->pica_basicP, src);
        }
        const char* why = SkipReason(in_data, L, self, anySolo, ci->t, name);
        if (why) {
            if (!quiet) Log("S7 layer %d '%s': skip (%s)", i, name.c_str(), why);
            continue;
        }
        auto ti = std::make_unique<TargetInfo>();
        ti->L = L;
        const double t0 = NowMs();
        RenderTarget(in_data, L, extra->input->bitdepth, quiet ? "S7q" : "S7", &ti->img, MixIn, extra);
        PF_Point3DDef c[3] = {};
        if (CornersOf(in_data, L, *ci, c)) ti->plane.Build(c[0], c[1], c[2]);
        ti->opacity = LayerOpacity(in_data, L);
        AEGP_LayerTransferMode tm;
        AEFX_CLR_STRUCT(tm);
        AEGP_LayerH mL = nullptr;
        if (!ls->AEGP_GetTrackMatteLayer(L, &mL) && mL && !ls->AEGP_GetLayerTransferMode(L, &tm)) {
            ti->matteL = mL;
            ti->mode = tm.track_matte;
            RenderTarget(in_data, mL, extra->input->bitdepth, quiet ? "S7q(matte)" : "S7(matte)", &ti->matte, MixIn,
                         extra);
            PF_Point3DDef mc[3] = {};
            if (CornersOf(in_data, mL, *ci, mc)) ti->mplane.Build(mc[0], mc[1], mc[2]);
            ti->matteOpacity = LayerOpacity(in_data, mL);
            const double mk[9] = {mc[0].x_value, mc[0].y_value, mc[0].z_value, mc[1].x_value, mc[1].y_value,
                                  mc[1].z_value, mc[2].x_value, mc[2].y_value, mc[2].z_value};
            MixIn(in_data, extra, mk, sizeof(mk));
        }
        tRender += NowMs() - t0;
        // cache key: layer identity + order, geometry, opacity, matte mode (receipts mixed above)
        AEGP_LayerIDVal id = 0;
        ls->AEGP_GetLayerID(L, &id);
        double key[13] = {(double)id, (double)i, c[0].x_value, c[0].y_value, c[0].z_value, c[1].x_value,
                          c[1].y_value, c[1].z_value, c[2].x_value, c[2].y_value, c[2].z_value, ti->opacity,
                          ti->matteOpacity};
        A_long mode = ti->mode;
        MixIn(in_data, extra, key, sizeof(key));
        MixIn(in_data, extra, &mode, sizeof(mode));
        if (!quiet)
            Log("S7 layer %d '%s': target opacity=%.3f matte=%s mode=%d", i, name.c_str(), ti->opacity,
                ti->matteL ? "yes" : "no", (int)ti->mode);
        out->push_back(std::move(ti));
    }
    // the self layer's own transform (S5b NOMIX showed it must be in the key)
    const double sk[9] = {selfC[0].x_value, selfC[0].y_value, selfC[0].z_value, selfC[1].x_value, selfC[1].y_value,
                          selfC[1].z_value, selfC[2].x_value, selfC[2].y_value, selfC[2].z_value};
    MixIn(in_data, extra, sk, sizeof(sk));
    const A_long cnt = (A_long)out->size();
    MixIn(in_data, extra, &cnt, sizeof(cnt));
    Log("S7 comp='%s' layers=%d targets=%d solo=%d targets+mattes render %.1fms", compName->c_str(), n, cnt,
        (int)anySolo, tRender);

    t_quietOpaque = false;
    --t_s7Depth;
    {
        std::lock_guard<std::mutex> lk(g_s7Mutex);
        for (size_t k = 0; k < g_s7Active.size(); ++k)
            if (g_s7Active[k] == me) { g_s7Active.erase(g_s7Active.begin() + k); break; }
    }
}

// ---------------------------------------------------------------- pre-render data
struct SpikePre {
    Affine2 self;       // self layer (full-res layer px) -> comp px
    PlaneMap slot1;     // comp px -> slot1 layer (full-res layer px), perspective-correct
    double compW, compH;
    double dsX, dsY;    // downsample factors
    bool haveSlot1;
    std::string compName;
    AEGP_LayerH target = nullptr;
    AegpFrame aegp;     // S5: AEGP render of the target (site "pre")
    // S6: track matte of the target (plan A: render the matte layer through AEGP too, and
    // apply it ourselves in comp space)
    PlaneMap slot2;     // comp px -> matte layer px (from S2 C0..C2 expressions on Slot 2)
    AegpFrame matte;
    AEGP_TrackMatte matteMode = AEGP_TrackMatte_NO_TRACK_MATTE;
    double targetOpacity = 1.0, matteOpacity = 1.0;   // S4: applied by us (not in the AEGP pixels)
    bool s7 = false;                                  // S7: every layer is a target
    std::vector<std::unique_ptr<TargetInfo>> targets;
};

// Pixel of `im` that lands on comp px `c` (nearest). Comp-space images (text/shape/collapsed)
// are indexed directly; layer-space images go through the plane map. Transparent outside.
static RGBA SampleAtComp(const Img& im, const PlaneMap& plane, V2 c, double dsX, double dsY)
{
    if (!im.base) return {0, 0, 0, 0};
    double bx, by;
    if (im.compSpace) {
        bx = c.x * dsX;
        by = c.y * dsY;
    } else {
        double su, sv, sz;
        if (!plane.Map(c.x, c.y, &su, &sv, &sz)) return {0, 0, 0, 0};
        bx = su * dsX;
        by = sv * dsY;
    }
    int sx = (int)std::floor(bx) - im.offX;
    int sy = (int)std::floor(by) - im.offY;
    if (sx < 0 || sy < 0 || sx >= im.w || sy >= im.h) return {0, 0, 0, 0};
    return GetPxImg(im, sx, sy);
}

static void DeletePre(void* p)
{
    SpikePre* s = (SpikePre*)p;
    s->aegp.Release();
    s->matte.Release();
    delete s;
}

static bool S5SiteSmart()
{
    char v[32] = {0};
    DWORD n = GetEnvironmentVariableA("DUPLIX_S5_SITE", v, sizeof(v));
    return n > 0 && n < sizeof(v) && _stricmp(v, "smart") == 0;
}

static void MixIn(PF_InData* in_data, void* ctx, const void* data, size_t n)
{
    PF_PreRenderExtra* extra = (PF_PreRenderExtra*)ctx;
    if (extra->cb->GuidMixInPtr) extra->cb->GuidMixInPtr(in_data->effect_ref, (A_u_long)n, data);
}

enum { CK_INPUT = 0, CK_SLOT1 = 1, CK_QUERY_INPUT = 90, CK_QUERY_SLOT1 = 91, CK_QUERY_SLOT2 = 92 };

static void LogRect(const char* tag, const PF_LRect& r)
{
    Log("  %s = L%d T%d R%d B%d (w%d h%d)", tag, r.left, r.top, r.right, r.bottom, r.right - r.left,
        r.bottom - r.top);
}

static void LogPt(const char* tag, const PF_Point3DDef& p)
{
    Log("  %s = (%.4f, %.4f, %.4f)", tag, p.x_value, p.y_value, p.z_value);
}

// ---------------------------------------------------------------- S5c: suite version probe
// Which versions of the render-related suites does this AE actually register?
// Only acquires/releases; never calls a function pointer.
static std::string ModuleOf(const void* addr)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &m) || !m)
        return "?";
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(m, path, MAX_PATH);
    char buf[MAX_PATH + 64];
    sprintf_s(buf, "%s+0x%llx", path, (unsigned long long)((const char*)addr - (const char*)m));
    return buf;
}

static void ProbeSuiteVersions(SPBasicSuite* sp)
{
    const char* names[] = {kAEGPLayerRenderOptionsSuite, kAEGPRenderOptionsSuite, kAEGPRenderSuite};
    for (const char* n : names) {
        for (int v = 1; v <= 20; ++v) {
            const void* q = nullptr;
            if (sp->AcquireSuite(n, v, &q) == kSPNoError && q) {
                const void* const* fp = (const void* const*)q;
                Log("S5c probe: '%s' v%d OK table=%p fn0=%s", n, v, q, ModuleOf(fp[0]).c_str());
                sp->ReleaseSuite(n, v);
            }
        }
    }
}

// ---------------------------------------------------------------- selectors
static PF_Err GlobalSetup(PF_InData* in_data, PF_OutData* out_data)
{
    out_data->my_version = PF_VERSION(0, 1, 0, PF_Stage_DEVELOP, 1);
    out_data->out_flags = PF_OutFlag_DEEP_COLOR_AWARE;
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE |
                           PF_OutFlag2_SUPPORTS_THREADED_RENDERING | PF_OutFlag2_I_MIX_GUID_DEPENDENCIES;
    char app[64] = {0};
    Log("GlobalSetup: host version %d.%d appl_id=%.4s", in_data->version.major, in_data->version.minor,
        (const char*)&in_data->appl_id);
    (void)app;
    EnsureRegistered(in_data, "GlobalSetup");
    ProbeSuiteVersions(in_data->pica_basicP);
    return PF_Err_NONE;
}

static PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data)
{
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;

    for (int i = 0; i < SPIKE_NUM_SLOTS; ++i) {
        AEFX_CLR_STRUCT(def);
        char name[32];
        sprintf_s(name, "Slot %d", i + 1);
        PF_ADD_LAYER(name, PF_LayerDefault_NONE, DISK_ID_SLOT_BASE + i);
    }
    PF_ADD_POINT_3D("CompSize", 0, 0, 0, DISK_ID_COMP_SIZE);
    PF_ADD_POINT_3D("Self O", 0, 0, 0, DISK_ID_SELF_O);
    PF_ADD_POINT_3D("Self X", 0, 0, 0, DISK_ID_SELF_X);
    PF_ADD_POINT_3D("Self Y", 0, 0, 0, DISK_ID_SELF_Y);
    PF_ADD_POINT_3D("S1 C0", 0, 0, 0, DISK_ID_S1_C0);
    PF_ADD_POINT_3D("S1 C1", 0, 0, 0, DISK_ID_S1_C1);
    PF_ADD_POINT_3D("S1 C2", 0, 0, 0, DISK_ID_S1_C2);
    PF_ADD_POINT_3D("S2 C0", 0, 0, 0, DISK_ID_S2_C0);
    PF_ADD_POINT_3D("S2 C1", 0, 0, 0, DISK_ID_S2_C1);
    PF_ADD_POINT_3D("S2 C2", 0, 0, 0, DISK_ID_S2_C2);

    out_data->num_params = SP_NUM_PARAMS;
    Log("ParamsSetup: num_params=%d (slots=%d) err=%d", SP_NUM_PARAMS, SPIKE_NUM_SLOTS, err);
    return err;
}

static PF_Err CheckoutPt(PF_InData* in_data, int index, PF_Point3DDef* out)
{
    PF_ParamDef p;
    AEFX_CLR_STRUCT(p);
    PF_Err err = PF_CHECKOUT_PARAM(in_data, index, in_data->current_time, in_data->time_step,
                                   in_data->time_scale, &p);
    if (!err) *out = p.u.point3d_d;
    // SmartFX: params checked out in pre-render are checked in automatically.
    return err;
}

static PF_Err PreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* extra)
{
    PF_Err err = PF_Err_NONE;
    const double tPre0 = NowMs();

    // AE's persistent disk cache does not know the binary changed: mix a per-build id into the cache key.
    static const char kBuildId[] = __DATE__ " " __TIME__;
    if (extra->cb->GuidMixInPtr) extra->cb->GuidMixInPtr(in_data->effect_ref, sizeof(kBuildId), kBuildId);
    // Per-run salt so a second aerender pass (e.g. MFR on) cannot reuse the first pass's disk cache.
    {
        char salt[64] = {0};
        DWORD n = GetEnvironmentVariableA("DUPLIX_S5_SALT", salt, sizeof(salt));
        if (n > 0 && n < sizeof(salt) && extra->cb->GuidMixInPtr)
            extra->cb->GuidMixInPtr(in_data->effect_ref, n, salt);
    }

    PF_Point3DDef cs = {}, so = {}, sx = {}, sy = {}, c0 = {}, c1 = {}, c2 = {}, m0 = {}, m1 = {}, m2 = {};
    const int s5b = S7Mode() ? 1 : S5bMode();
    if (s5b != 1) {
    ERR(CheckoutPt(in_data, SP_COMP_SIZE, &cs));
    ERR(CheckoutPt(in_data, SP_SELF_O, &so));
    ERR(CheckoutPt(in_data, SP_SELF_X, &sx));
    ERR(CheckoutPt(in_data, SP_SELF_Y, &sy));
    ERR(CheckoutPt(in_data, SP_S1_C0, &c0));
    ERR(CheckoutPt(in_data, SP_S1_C1, &c1));
    ERR(CheckoutPt(in_data, SP_S1_C2, &c2));
    ERR(CheckoutPt(in_data, SP_S2_C0, &m0));
    ERR(CheckoutPt(in_data, SP_S2_C1, &m1));
    ERR(CheckoutPt(in_data, SP_S2_C2, &m2));
    if (err) { Log("PreRender: param checkout failed err=%d", err); return err; }
    Log("S4 param checkout (10 expression points) %.1fms", NowMs() - tPre0);
    } else {
        Log("S5b mode 1: expression params not checked out");
    }

    SpikePre* pre = new SpikePre();
    pre->self = MakeAffine(so, sx, sy);
    pre->slot1.Build(c0, c1, c2);
    pre->slot2.Build(m0, m1, m2);
    pre->compW = cs.x_value;
    pre->compH = cs.y_value;
    pre->dsX = (double)in_data->downsample_x.num / in_data->downsample_x.den;
    pre->dsY = (double)in_data->downsample_y.num / in_data->downsample_y.den;

    Log("PreRender: time=%d/%d bitdepth=%d ds=(%.3f,%.3f)", in_data->current_time, in_data->time_scale,
        extra->input->bitdepth, pre->dsX, pre->dsY);
    LogRect("output_request.rect", extra->input->output_request.rect);
    LogPt("CompSize", cs);
    LogPt("Self O", so);
    LogPt("Self X", sx);
    LogPt("Self Y", sy);
    LogPt("S1 C0", c0);
    LogPt("S1 C1", c1);
    LogPt("S1 C2", c2);
    LogPt("S2 C0", m0);
    LogPt("S2 C1", m1);
    LogPt("S2 C2", m2);

    // --- input layer: query size, then check out everything
    PF_RenderRequest req = extra->input->output_request;
    PF_CheckoutResult q;
    AEFX_CLR_STRUCT(q);
    req.rect.left = req.rect.top = req.rect.right = req.rect.bottom = 0;
    ERR(extra->cb->checkout_layer(in_data->effect_ref, SP_INPUT, CK_QUERY_INPUT, &req, in_data->current_time,
                                  in_data->time_step, in_data->time_scale, &q));
    Log("  input query err=%d ref=%dx%d", err, q.ref_width, q.ref_height);
    LogRect("input max_result_rect", q.max_result_rect);
    PF_CheckoutResult inRes;
    AEFX_CLR_STRUCT(inRes);
    req.rect = q.max_result_rect;
    req.preserve_rgb_of_zero_alpha = TRUE;
    ERR(extra->cb->checkout_layer(in_data->effect_ref, SP_INPUT, CK_INPUT, &req, in_data->current_time,
                                  in_data->time_step, in_data->time_scale, &inRes));
    LogRect("input result_rect", inRes.result_rect);

    EnsureRegistered(in_data, "PreRender");
    if (S7Mode()) {
        pre->s7 = true;
        pre->haveSlot1 = false;
        PF_Point3DDef selfC[3] = {};
        CamInfo ci;
        const double t7 = NowMs();
        S7Collect(in_data, extra, &pre->targets, selfC, &ci, &pre->compName);
        pre->self = MakeAffine(selfC[0], selfC[1], selfC[2]);
        pre->compW = ci.cw;
        pre->compH = ci.ch;
        Log("S7 collect total %.1fms", NowMs() - t7);
    } else {
    // --- slot 1 and slot 2 (slot 2 expected empty)
    PF_CheckoutResult s1q, s2q;
    AEFX_CLR_STRUCT(s1q);
    AEFX_CLR_STRUCT(s2q);
    PF_RenderRequest sreq = extra->input->output_request;
    sreq.rect.left = sreq.rect.top = sreq.rect.right = sreq.rect.bottom = 0;
    PF_Err e1 = extra->cb->checkout_layer(in_data->effect_ref, SP_SLOT_FIRST, CK_QUERY_SLOT1, &sreq,
                                          in_data->current_time, in_data->time_step, in_data->time_scale, &s1q);
    PF_Err e2 = extra->cb->checkout_layer(in_data->effect_ref, SP_SLOT_FIRST + 1, CK_QUERY_SLOT2, &sreq,
                                          in_data->current_time, in_data->time_step, in_data->time_scale, &s2q);
    Log("  slot1 query err=%d ref=%dx%d solid=%d", e1, s1q.ref_width, s1q.ref_height, s1q.solid);
    LogRect("slot1 max_result_rect", s1q.max_result_rect);
    Log("  slot2 query err=%d ref=%dx%d", e2, s2q.ref_width, s2q.ref_height);
    LogRect("slot2 max_result_rect", s2q.max_result_rect);

    pre->haveSlot1 = false;
    if (!e1 && s1q.max_result_rect.right > s1q.max_result_rect.left) {
        PF_CheckoutResult s1r;
        AEFX_CLR_STRUCT(s1r);
        sreq.rect = s1q.max_result_rect;
        sreq.preserve_rgb_of_zero_alpha = TRUE;
        PF_Err e = extra->cb->checkout_layer(in_data->effect_ref, SP_SLOT_FIRST, CK_SLOT1, &sreq,
                                             in_data->current_time, in_data->time_step, in_data->time_scale,
                                             &s1r);
        Log("  slot1 checkout err=%d", e);
        LogRect("slot1 result_rect", s1r.result_rect);
        pre->haveSlot1 = (e == PF_Err_NONE);
    }

    // --- S5: find "Target" and (site "pre") render it through AEGP here
    EnsureRegistered(in_data, "PreRender");
    A_long tIdx = -1;
    AEGP_LayerH s5bMatte = nullptr;
    if (FindTarget(in_data, &pre->target, &pre->compName, &tIdx)) {
        Log("S5 PreRender comp='%s' target index=%d site=%s", pre->compName.c_str(), tIdx,
            S5SiteSmart() ? "smart" : "pre");
        if (!S5SiteSmart()) {
            // S4 negative control: DUPLIX_S4_NOGUID=1 leaves the target's receipt GUID out of the
            // cache key, so content-only changes of the target (mask, effect) must come out stale.
            char ng[8] = {0};
            DWORD ngn = GetEnvironmentVariableA("DUPLIX_S4_NOGUID", ng, sizeof(ng));
            const bool noGuid = ngn > 0 && ngn < sizeof(ng) && ng[0] == '1';
            if (noGuid) Log("S4 NOGUID: target receipt GUID not mixed");
            RenderTarget(in_data, pre->target, extra->input->bitdepth, "PreRender", &pre->aegp,
                         noGuid ? nullptr : MixIn, extra);
            // S6: the track matte. AE applies it while compositing the comp, so the layer render
            // above does not include it (S5). Render the matte layer too and remember its mode.
            auto ls = SUITE(AEGP_LayerSuite9, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
            AEGP_LayerH matteL = nullptr;
            A_Err em = ls ? ls->AEGP_GetTrackMatteLayer(pre->target, &matteL) : -1;
            AEGP_LayerTransferMode tm;
            AEFX_CLR_STRUCT(tm);
            A_Err et = ls ? ls->AEGP_GetLayerTransferMode(pre->target, &tm) : -1;
            if (!em && matteL) {
                s5bMatte = matteL;
                AEGP_LayerFlags mf = 0;
                A_Boolean mvid = FALSE;
                ls->AEGP_GetLayerFlags(matteL, &mf);
                A_Time ct = {0, 1};
                ls->AEGP_IsVideoActive(matteL, AEGP_LTimeMode_CompTime, &ct, &mvid);
                Log("S6 matte: layer=%p mode=%d (GetTransferMode err=%d flags=0x%x videoActive=%d)", (void*)matteL,
                    (int)tm.track_matte, et, (unsigned)mf, (int)mvid);
                pre->matteMode = tm.track_matte;
                RenderTarget(in_data, matteL, extra->input->bitdepth, "PreRender(matte)", &pre->matte, MixIn,
                             extra);
                // the matte type is not part of either receipt: mix it in ourselves
                MixIn(in_data, extra, &pre->matteMode, sizeof(pre->matteMode));
            } else {
                Log("S6 matte: none (GetTrackMatteLayer err=%d layer=%p, mode=%d)", em, (void*)matteL,
                    (int)tm.track_matte);
            }
        } else {
            // Site "smart": the cache key only knows which comp/layer, not its content.
            auto ls = SUITE(AEGP_LayerSuite9, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
            AEGP_LayerIDVal id = 0;
            if (ls) ls->AEGP_GetLayerID(pre->target, &id);
            MixIn(in_data, extra, &id, sizeof(id));
            MixIn(in_data, extra, pre->compName.data(), pre->compName.size());
        }
    }

    // --- S5b: geometry from AEGP instead of the corner expressions
    if (s5b != 0 && pre->target) {
        const double tg0 = NowMs();
        AegpGeom g;
        if (GeomFromAegp(in_data, pre->target, s5bMatte, &g)) {
            Log("S5b geometry from AEGP %.2fms (matte=%d)", NowMs() - tg0, s5bMatte ? 1 : 0);
            if (s5b == 2) {
                LogDiff("Comp", cs, g.cs);
                LogDiff("SelfO", so, g.so);
                LogDiff("SelfX", sx, g.sx);
                LogDiff("SelfY", sy, g.sy);
                LogDiff("S1C0", c0, g.c[0]);
                LogDiff("S1C1", c1, g.c[1]);
                LogDiff("S1C2", c2, g.c[2]);
                if (s5bMatte) {
                    LogDiff("S2C0", m0, g.m[0]);
                    LogDiff("S2C1", m1, g.m[1]);
                    LogDiff("S2C2", m2, g.m[2]);
                }
            }
            pre->self = MakeAffine(g.so, g.sx, g.sy);
            pre->slot1.Build(g.c[0], g.c[1], g.c[2]);
            pre->slot2.Build(g.m[0], g.m[1], g.m[2]);
            pre->compW = g.cs.x_value;
            pre->compH = g.cs.y_value;
            // the expressions used to put these numbers into our params (= the cache key)
            // (numbers only: struct padding bytes are undefined and would break cache hits)
            const PF_Point3DDef* pts[] = {&g.cs, &g.so, &g.sx, &g.sy, &g.c[0], &g.c[1], &g.c[2],
                                          &g.m[0], &g.m[1], &g.m[2]};
            double key[30];
            for (int i = 0; i < 10; ++i) {
                key[i * 3] = pts[i]->x_value;
                key[i * 3 + 1] = pts[i]->y_value;
                key[i * 3 + 2] = pts[i]->z_value;
            }
            // negative control: DUPLIX_S5B_NOMIX=1 leaves the geometry out of the cache key
            char nm[8] = {0};
            DWORD nmn = GetEnvironmentVariableA("DUPLIX_S5B_NOMIX", nm, sizeof(nm));
            if (nmn > 0 && nmn < sizeof(nm) && nm[0] == '1') Log("S5b NOMIX: geometry not mixed");
            else MixIn(in_data, extra, key, sizeof(key));
        } else {
            Log("S5b geometry from AEGP FAILED");
        }
    }

    // --- S4: opacity of the target and of its matte (not part of the AEGP pixels)
    if (pre->target) {
        pre->targetOpacity = LayerOpacity(in_data, pre->target);
        // AEGP renders a layer even when AE would not draw it (video switch off, outside its
        // in/out range). Such a layer must not hide anything: treat it as opacity 0.
        {
            auto ls = SUITE(AEGP_LayerSuite9, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
            auto pfi = SUITE(AEGP_PFInterfaceSuite1, kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1);
            A_Time ct = {0, 1};
            if (pfi) pfi->AEGP_ConvertEffectToCompTime(in_data->effect_ref, in_data->current_time,
                                                       in_data->time_scale, &ct);
            A_Boolean va = TRUE;
            A_Err ev = ls ? ls->AEGP_IsVideoActive(pre->target, AEGP_LTimeMode_CompTime, &ct, &va) : -1;
            Log("S4 target videoActive=%d (err=%d)", (int)va, ev);
            if (!ev && !va) pre->targetOpacity = 0;
        }
        pre->matteOpacity = s5bMatte ? LayerOpacity(in_data, s5bMatte) : 1.0;
        const double ops[2] = {pre->targetOpacity, pre->matteOpacity};
        MixIn(in_data, extra, ops, sizeof(ops));
        Log("S4 opacity target=%.3f matte=%.3f", ops[0], ops[1]);
    }
    }   // !S7Mode()

    // --- our output: the whole comp, expressed in (downsampled) layer coords
    PF_LRect out = {0, 0, 0, 0};
    if (pre->compW > 0 && pre->compH > 0) {
        double minU = 1e30, minV = 1e30, maxU = -1e30, maxV = -1e30;
        const double cx[4] = {0, pre->compW, 0, pre->compW};
        const double cy[4] = {0, 0, pre->compH, pre->compH};
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            double u, v;
            if (!pre->self.Inv(cx[i], cy[i], &u, &v)) { ok = false; break; }
            u *= pre->dsX;
            v *= pre->dsY;
            minU = std::min(minU, u); maxU = std::max(maxU, u);
            minV = std::min(minV, v); maxV = std::max(maxV, v);
        }
        if (ok) {
            out.left = (A_long)std::floor(minU);
            out.top = (A_long)std::floor(minV);
            out.right = (A_long)std::ceil(maxU);
            out.bottom = (A_long)std::ceil(maxV);
        }
    }
    extra->output->result_rect = out;
    extra->output->max_result_rect = out;
    extra->output->flags |= PF_RenderOutputFlag_RETURNS_EXTRA_PIXELS;
    extra->output->pre_render_data = pre;
    extra->output->delete_pre_render_data_func = DeletePre;
    LogRect("OUR result_rect", out);
    Log("S4 PreRender total %.1fms comp='%s'", NowMs() - tPre0, pre->compName.c_str());
    return err;
}

static PF_Err SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra)
{
    PF_Err err = PF_Err_NONE;
    const SpikePre* pre = (const SpikePre*)extra->input->pre_render_data;
    const int bpc = extra->input->bitdepth;
    PF_EffectWorld* outW = nullptr;
    PF_EffectWorld* inW = nullptr;
    PF_EffectWorld* s1W = nullptr;

    // AE requires at least one input checkout before checkout_output (error 516 otherwise).
    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, CK_INPUT, &inW));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &outW));
    if (!err && pre && pre->haveSlot1) {
        PF_Err e = extra->cb->checkout_layer_pixels(in_data->effect_ref, CK_SLOT1, &s1W);
        if (e) { Log("SmartRender: slot1 pixels err=%d", e); s1W = nullptr; }
    }
    if (err || !outW || !pre) { Log("SmartRender: early out err=%d", err); return err; }

    Log("SmartRender: bpc=%d out %dx%d origin(%d,%d) comp='%s'", bpc, outW->width, outW->height, outW->origin_x,
        outW->origin_y, pre->compName.c_str());

    // S5: pixel source for the overlay. AEGP render (site pre: from PreRender; site smart: render now).
    AegpFrame localFrame;
    const AegpFrame* fr = nullptr;
    if (pre->aegp.ok) {
        fr = &pre->aegp;
    } else if (S5SiteSmart() && pre->target) {
        if (RenderTarget(in_data, pre->target, bpc, "SmartRender", &localFrame, nullptr, nullptr)) fr = &localFrame;
    }
    Img src;
    const char* srcName = "none";
    if (pre->s7) {
        srcName = "aegp";   // S7: union of all targets (empty union is fine: nothing occludes)
        for (auto& t : pre->targets)
            if (!t->img.ok) { srcName = "s7_partial"; Log("  S7 target without pixels (noWorld=%d)", (int)t->img.noWorld); }
    } else if (fr) {
        src = fr->img;
        srcName = "aegp";
    } else if (s1W) {
        src.base = (const char*)s1W->data;
        src.rowbytes = s1W->rowbytes;
        src.w = s1W->width;
        src.h = s1W->height;
        src.offX = s1W->origin_x;
        src.offY = s1W->origin_y;
        src.bpc = bpc;
        srcName = "slot1";
        // Collapsed precomp: AEGP gives no pixels, but the slot checkout returns a comp-space,
        // already projected buffer (S2b). Use it as comp-space pixels at its origin.
        if (pre->aegp.noWorld) {
            src.compSpace = true;
            srcName = "slot1_comp";
        }
    }
    Log("  overlay source = %s", srcName);
    LogRect("request.rect", extra->input->output_request.rect);
    if (inW) Log("  input %dx%d origin(%d,%d)", inW->width, inW->height, inW->origin_x, inW->origin_y);
    if (s1W) Log("  slot1 %dx%d origin(%d,%d)", s1W->width, s1W->height, s1W->origin_x, s1W->origin_y);
    if (s1W && s1W->width > 0 && s1W->height > 0) {
        RGBA c = GetPx(s1W, bpc, s1W->width / 2, s1W->height / 2);
        Log("  slot1 center px = (%.3f %.3f %.3f a%.3f)", c.r, c.g, c.b, c.a);
        // where is the opaque content, in buffer coords + origin (= layer coords if origin means that)?
        long n = 0; int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
        for (int y = 0; y < s1W->height; ++y)
            for (int x = 0; x < s1W->width; ++x)
                if (GetPx(s1W, bpc, x, y).a > 0.5f) {
                    ++n; x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
                }
        if (n) Log("  slot1 opaque px=%ld bbox(buffer+origin) L%d T%d R%d B%d", n, x0 + s1W->origin_x,
                   y0 + s1W->origin_y, x1 + 1 + s1W->origin_x, y1 + 1 + s1W->origin_y);
        else Log("  slot1 opaque px=0");
    }

    // S6: track matte pixels (rendered in PreRender with the target)
    const AEGP_TrackMatte matteMode = fr ? pre->matteMode : AEGP_TrackMatte_NO_TRACK_MATTE;
    Img mimg;
    if (matteMode != AEGP_TrackMatte_NO_TRACK_MATTE && pre->matte.ok) mimg = pre->matte.img;
    if (matteMode != AEGP_TrackMatte_NO_TRACK_MATTE)
        Log("  S6 apply matte mode=%d pixels=%s compSpace=%d plane=%d", (int)matteMode, mimg.base ? "yes" : "NO",
            (int)mimg.compSpace, (int)pre->slot2.ok);

    for (int j = 0; j < outW->height; ++j) {
        for (int i = 0; i < outW->width; ++i) {
            // pixel center in full-res layer coords
            double u = (i + outW->origin_x + 0.5) / pre->dsX;
            double v = (j + outW->origin_y + 0.5) / pre->dsY;
            V2 c = pre->self.Fwd(u, v);   // comp px
            RGBA px = {0, 0, 0, 0};

            bool inComp = c.x >= 0 && c.y >= 0 && c.x < pre->compW && c.y < pre->compH;
            if (inComp && (c.x < 8 || c.y < 8 || c.x >= pre->compW - 8 || c.y >= pre->compH - 8))
                px = {0, 1, 0, 1};   // green comp border
            if (c.x >= 100 && c.x < 300 && c.y >= 100 && c.y < 300)
                px = {1, 0, 0, 1};   // red marker at comp (100..300)

            if (pre->s7) {
                // S7: coverage = max over all targets (each: own opacity x own matte)
                float best = 0;
                for (auto& tp : pre->targets) {
                    const TargetInfo& T = *tp;
                    if (!T.img.ok || T.opacity <= 0) continue;
                    float ta = SampleAtComp(T.img.img, T.plane, c, pre->dsX, pre->dsY).a * (float)T.opacity;
                    if (ta > 0 && T.mode != AEGP_TrackMatte_NO_TRACK_MATTE) {
                        RGBA m = T.matte.ok ? SampleAtComp(T.matte.img, T.mplane, c, pre->dsX, pre->dsY)
                                            : RGBA{0, 0, 0, 0};
                        float lum = 0.299f * m.r + 0.587f * m.g + 0.114f * m.b;
                        const float mo = (float)T.matteOpacity;
                        float f = 1;
                        switch (T.mode) {
                        case AEGP_TrackMatte_ALPHA: f = m.a * mo; break;
                        case AEGP_TrackMatte_NOT_ALPHA: f = 1 - m.a * mo; break;
                        case AEGP_TrackMatte_LUMA: f = lum * mo; break;
                        case AEGP_TrackMatte_NOT_LUMA: f = 1 - lum * mo; break;
                        default: break;
                        }
                        ta *= f;
                    }
                    best = std::max(best, ta);
                }
                if (best > 0.5f) px = {1, 0, 1, 1};
                PutPx(outW, bpc, i, j, px);
                continue;
            }
            RGBA t = SampleAtComp(src, pre->slot1, c, pre->dsX, pre->dsY);
            float a = t.a * (fr ? (float)pre->targetOpacity : 1.0f);
            if (a > 0 && matteMode != AEGP_TrackMatte_NO_TRACK_MATTE) {
                // S6: apply the track matte in comp space. Outside the matte's pixels it is transparent.
                RGBA m = mimg.base ? SampleAtComp(mimg, pre->slot2, c, pre->dsX, pre->dsY) : RGBA{0, 0, 0, 0};
                float lum = 0.299f * m.r + 0.587f * m.g + 0.114f * m.b;   // premultiplied colour
                float f = 1;
                // the matte layer's own opacity scales its pixels (premultiplied) before use
                const float mo = (float)pre->matteOpacity;
                switch (matteMode) {
                case AEGP_TrackMatte_ALPHA: f = m.a * mo; break;
                case AEGP_TrackMatte_NOT_ALPHA: f = 1 - m.a * mo; break;
                case AEGP_TrackMatte_LUMA: f = lum * mo; break;
                case AEGP_TrackMatte_NOT_LUMA: f = 1 - lum * mo; break;
                default: break;
                }
                a *= f;
            }
            // opaque magenta wherever the target has coverage (colour-space independent check)
            if (a > 0.5f) px = {1, 0, 1, 1};
            PutPx(outW, bpc, i, j, px);
        }
    }
    (void)inW;
    localFrame.Release();
    return err;
}

// ---------------------------------------------------------------- probe: hidden "Layer Sets"
// AEGP_LayerH is the BEE_Layer* itself (AEGPDriver's CreateNullLayer passes it straight to
// BEE_CmdSetLayerSwitch). DUPLIX_PROBE_SETS=create -> BEE_AVLayer::CreateSet on "Target";
// =switch -> only set switches 0x80000 and 128 (what CreateSet adds on top of the stream group).
// ---- per-instance vtable copy with logging wrappers (only this layer object is affected)
static const int kVtSlots = 243;
static void* g_vtCopy[kVtSlots + 1];  // [0] = RTTI COL, then slots
static void* g_vtOrig[kVtSlots];
static std::atomic<int> g_vtCalls[kVtSlots];
typedef uintptr_t (*Fwd8)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef double (*FwdD)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static unsigned LayerSetsCountOf(void* layer)
{
    // vtable +0x670 GetLayerSets (non-const) -> BEE_LayerSets*; BEE_LayerSets::GetLayerSetsCount (exported)
    typedef void* (*VGet)(void*);
    typedef unsigned (*Cnt)(void*);
    static Cnt cnt = (Cnt)GetProcAddress(GetModuleHandleA("BEE.dll"), "?GetLayerSetsCount@BEE_LayerSets@@QEBAIXZ");
    void* sets = ((VGet)g_vtOrig[0x670 / 8])(layer);
    return (sets && cnt) ? cnt(sets) : 0xFFFFFFFFu;
}
template <int SLOT> static uintptr_t VtLog8(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e,
                                            uintptr_t f, uintptr_t g, uintptr_t h)
{
    int n = ++g_vtCalls[SLOT];
    bool verbose = n <= 400;
    if (verbose) Log("VT ENTER slot %d (+0x%X) #%d this=%p", SLOT, SLOT * 8, n, (void*)a);
    uintptr_t r = ((Fwd8)g_vtOrig[SLOT])(a, b, c, d, e, f, g, h);
    if (verbose) {
        if (SLOT == 217)
            Log("VT LEAVE slot %d #%d this=%p -> 0x%llX sets=%u", SLOT, n, (void*)a, (unsigned long long)r,
                LayerSetsCountOf((void*)a));
        else
            Log("VT LEAVE slot %d #%d this=%p -> 0x%llX", SLOT, n, (void*)a, (unsigned long long)r);
    }
    return r;
}
template <int SLOT> static double VtLogD(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    double r = ((FwdD)g_vtOrig[SLOT])(a, b, c, d);
    int n = ++g_vtCalls[SLOT];
    if (n <= 6 || (n % 100) == 0) Log("VT slot %d (+0x%X) call#%d b=%p -> %f", SLOT, SLOT * 8, n, (void*)b, r);
    return r;
}
static bool g_vtOrigCaptured = false;
static void CaptureVtOrig(void** vt)
{
    if (g_vtOrigCaptured) return;
    for (int i = 0; i < kVtSlots; i++) g_vtOrig[i] = vt[i];
    g_vtOrigCaptured = true;
}
static void InstallVtLogger(void* obj)
{
    void** vt = *(void***)obj;
    CaptureVtOrig(vt);
    g_vtCopy[0] = vt[-1];
    for (int i = 0; i < kVtSlots; i++) g_vtCopy[i + 1] = g_vtOrig[i];
#define WRAP8(s) g_vtCopy[(s) + 1] = (void*)&VtLog8<(s)>
#define WRAPD(s) g_vtCopy[(s) + 1] = (void*)&VtLogD<(s)>
    WRAP8(143);  // GetSourceRectAccountingForSubLayerMotionBlur
    WRAP8(162);  // SubLayersLookAtCamera
    WRAP8(168);  // GetSubLayerLocalMatrix3D
    WRAP8(215);  // CanSubLayersRenderSeparately
    WRAP8(217);  // SubLayersRenderSeparately
    WRAP8(219);  // SubLayerRenderCount
    WRAP8(220);  // GetSubLayerRenderBounds
    WRAP8(221); WRAP8(222); WRAP8(223); WRAP8(224);  // GetSubLayerXform2D x4
    WRAP8(225);  // GetSubLayerXform3D
    WRAP8(216);  // SetSubLayersRenderSeparately
    WRAP8(230);  // CmdResetStreamsForSubLayerDimensionChange
    WRAP8(228);  // GetSubLayerSourceRectFromCache
    WRAP8(229);  // GetBoundsFromCache
    WRAP8(235);  // Rasterize2DGraph
    WRAP8(233);  // GetSubLayerRects
    WRAP8(234);  // HasSubLayerXform
    WRAP8(237);  // NewXformCache2D
#undef WRAP8
#undef WRAPD
    char mode[8] = {0};
    GetEnvironmentVariableA("DUPLIX_PROBE_VTLOG", mode, sizeof(mode));
    if (mode[0] == 'g') {
        // patch the shared vtable in place (every BEE_AVLayer, including render-thread clones)
        static bool done = false;
        if (done) return;
        DWORD old = 0;
        if (VirtualProtect(vt, kVtSlots * sizeof(void*), PAGE_READWRITE, &old)) {
            for (int i = 0; i < kVtSlots; i++)
                if (g_vtCopy[i + 1] != g_vtOrig[i]) vt[i] = g_vtCopy[i + 1];
            VirtualProtect(vt, kVtSlots * sizeof(void*), old, &old);
            done = true;
            Log("VT logger patched shared vtable %p", (void*)vt);
        } else {
            Log("VT logger: VirtualProtect failed %lu", GetLastError());
        }
        return;
    }
    *(void***)obj = &g_vtCopy[1];
    Log("VT logger installed on %p (orig vt %p)", obj, (void*)vt);
}

// ---- "fill": answer the sub-layer questions ourselves for one layer ID (shared vtable patch)
static std::atomic<int> g_fillLayerId{-1};
static int g_slotIsLayerType = 79, g_slotSep = 217, g_slotCount = 219, g_slotRects = 233;  // found at runtime
// DUPLIX_FILL_RANDOM=N: N copies, offsets/rotations from a Park-Miller sequence (same in probe_3d.jsx)
static int FillN()
{
    static int n = [] {
        char b[16] = {0};
        return GetEnvironmentVariableA("DUPLIX_FILL_RANDOM", b, sizeof(b)) ? atoi(b) : 5;
    }();
    return n;
}
#define kFillN FillN()
static const double* RandomXform(int i)  // {tx,ty,tz,rx,ry,rz}
{
    static std::vector<double> tbl = [] {
        std::vector<double> t;
        unsigned long long x = 12345;
        auto next = [&]() { x = (x * 16807ULL) % 2147483647ULL; return (double)x / 2147483647.0; };
        for (int k = 0; k < FillN(); k++) {
            double tx = (next() - 0.5) * 300, ty = (next() - 0.5) * 200, tz = (next() - 0.5) * 600;
            double rx = (next() - 0.5) * 120, ry = (next() - 0.5) * 120, rz = (next() - 0.5) * 120;
            t.insert(t.end(), {tx, ty, tz, rx, ry, rz});
        }
        return t;
    }();
    return &tbl[6 * i];
}
static bool FillRandom() { return GetEnvironmentVariableA("DUPLIX_FILL_RANDOM", nullptr, 0) > 0; }
typedef int (*BeeGetID)(const void* layer);
static int LayerIdOf(const void* layer)
{
    static BeeGetID f = (BeeGetID)GetProcAddress(GetModuleHandleA("BEE.dll"), "?GetID@BEE_Layer@@QEBA?BHXZ");
    return f ? f(layer) : -2;
}
struct RawVec { char* first; char* last; char* end; };
// Grow a std::vector (BEE's, MSVC STL, ucrtbase heap) to hold n elements; leaves it empty (last=first).
static bool VecReserve(RawVec* v, size_t elem, size_t n)
{
    typedef void* (*MallocF)(size_t);
    typedef void (*FreeF)(void*);
    static HMODULE ucrt = GetModuleHandleA("ucrtbase.dll");
    static MallocF m = (MallocF)GetProcAddress(ucrt, "malloc");
    static FreeF fr = (FreeF)GetProcAddress(ucrt, "free");
    if (!m || !fr) return false;
    size_t cap = (size_t)(v->end - v->first);
    v->last = v->first;
    if (cap >= elem * n) return true;
    size_t bytes = elem * n;
    char* p;
    if (bytes >= 4096) {  // MSVC _Allocate_manually_vector_aligned: raw at p[-1], 32-byte aligned
        char* raw = (char*)m(bytes + 8 + 31);
        if (!raw) return false;
        p = (char*)(((uintptr_t)raw + 8 + 31) & ~(uintptr_t)31);
        ((void**)p)[-1] = raw;
    } else {
        p = (char*)m(bytes);
        if (!p) return false;
    }
    if (v->first) {
        if (cap >= 4096) fr(((void**)v->first)[-1]);
        else fr(v->first);
    }
    v->first = v->last = p;
    v->end = p + bytes;
    return true;
}
static SPBasicSuite* g_pica = nullptr;
static thread_local double t_paramScale = 0;  // >0: scale read from the layer's slider (DUPLIX_FILL_PARAM)
// Read the first "ADBE Slider Control" value on `layer` (a render-thread clone is fine) at layer time.
// DUPLIX_FILL_BYEFFECT=1: "mine" = the layer carries an "ADBE Slider Control" (stand-in for our own effect).
static bool LayerHasMarker(void* layer)
{
    if (!g_pica) return false;
    Suite<AEGP_EffectSuite4> es(g_pica, kAEGPEffectSuite, kAEGPEffectSuiteVersion4);
    if (!es) return false;
    AEGP_LayerH L = (AEGP_LayerH)layer;
    A_long n = 0;
    if (es->AEGP_GetLayerNumEffects(L, &n) || n <= 0) return false;
    bool found = false;
    for (A_long k = 0; k < n && !found; k++) {
        AEGP_EffectRefH eff = nullptr;
        if (es->AEGP_GetLayerEffectByIndex(g_pluginId, L, k, &eff) || !eff) continue;
        AEGP_InstalledEffectKey key;
        A_char match[AEGP_MAX_EFFECT_MATCH_NAME_SIZE + 1] = {0};
        if (!es->AEGP_GetInstalledKeyFromLayerEffect(eff, &key) && !es->AEGP_GetEffectMatchName(key, match))
            found = !strcmp(match, "ADBE Slider Control");
        es->AEGP_DisposeEffect(eff);
    }
    return found;
}
// Registered layer IDs (no AEGP inside the hooks: calling AEGP from BEE virtuals on the UI thread hung AE).
static std::atomic<int> g_mineIds[64];
static std::atomic<int> g_mineCount{0};
static void RegisterMineId(int id)
{
    int n = g_mineCount;
    for (int i = 0; i < n; i++) if (g_mineIds[i] == id) return;
    if (n < 64) { g_mineIds[n] = id; g_mineCount = n + 1; Log("FILL registered layer id %d", id); }
}
static bool IsMine(void* layer)
{
    static bool byEffect = GetEnvironmentVariableA("DUPLIX_FILL_BYEFFECT", nullptr, 0) > 0;
    int id = LayerIdOf(layer);
    if (!byEffect) return id == g_fillLayerId;
    int n = g_mineCount;
    for (int i = 0; i < n; i++) if (g_mineIds[i] == id) return true;
    return false;
}
static bool ReadSliderOnLayer(void* layer, const int* tm, double* out)
{
    if (!g_pica) return false;
    Suite<AEGP_EffectSuite4> es(g_pica, kAEGPEffectSuite, kAEGPEffectSuiteVersion4);
    Suite<AEGP_StreamSuite6> ss(g_pica, kAEGPStreamSuite, kAEGPStreamSuiteVersion6);
    if (!es || !ss) return false;
    AEGP_LayerH L = (AEGP_LayerH)layer;
    A_long n = 0;
    if (es->AEGP_GetLayerNumEffects(L, &n)) return false;
    bool ok = false;
    for (A_long k = 0; k < n && !ok; k++) {
        AEGP_EffectRefH eff = nullptr;
        if (es->AEGP_GetLayerEffectByIndex(g_pluginId, L, k, &eff) || !eff) continue;
        AEGP_InstalledEffectKey key;
        A_char match[AEGP_MAX_EFFECT_MATCH_NAME_SIZE + 1] = {0};
        if (!es->AEGP_GetInstalledKeyFromLayerEffect(eff, &key) && !es->AEGP_GetEffectMatchName(key, match) &&
            !strcmp(match, "ADBE Slider Control")) {
            AEGP_StreamRefH s = nullptr;
            if (!ss->AEGP_GetNewEffectStreamByIndex(g_pluginId, eff, 1, &s) && s) {
                A_Time t = {tm ? tm[0] : 0, tm ? (A_u_long)tm[1] : 1};
                AEGP_StreamValue2 v = {};
                if (!ss->AEGP_GetNewStreamValue(g_pluginId, s, AEGP_LTimeMode_LayerTime, &t, FALSE, &v)) {
                    *out = v.val.one_d;
                    ok = true;
                    ss->AEGP_DisposeStreamValue(&v);
                }
                ss->AEGP_DisposeStream(s);
            }
        }
        es->AEGP_DisposeEffect(eff);
    }
    return ok;
}
static void FillOffset(int i, double* tx, double* ty, double* tz)
{
    static double spacing = [] {
        char b[32] = {0};
        return GetEnvironmentVariableA("DUPLIX_FILL_SPACING", b, sizeof(b)) ? atof(b) : 60.0;
    }();
    // DUPLIX_FILL_TABLE=1: fixed scattered offsets (same table in scenes/probe_3d.jsx)
    static const double table[5][3] = {{0, 0, 0}, {-110, -40, 80}, {110, 30, -120}, {-60, 60, -200}, {70, -60, 150}};
    static bool useTable = GetEnvironmentVariableA("DUPLIX_FILL_TABLE", nullptr, 0) > 0;
    if (FillRandom()) {
        const double* r = RandomXform(i);
        *tx = r[0]; *ty = r[1]; *tz = r[2];
        return;
    }
    if (useTable) {
        // DUPLIX_FILL_FILE: a text file holding a scale factor, re-read on every call (cache test)
        double scale = 1.0;
        if (t_paramScale > 0) {
            *tx = table[i][0] * t_paramScale;
            *ty = table[i][1] * t_paramScale;
            *tz = table[i][2] * t_paramScale;
            return;
        }
        char path[MAX_PATH] = {0};
        if (GetEnvironmentVariableA("DUPLIX_FILL_FILE", path, sizeof(path))) {
            FILE* fp = nullptr;
            if (!fopen_s(&fp, path, "r") && fp) {
                if (fscanf_s(fp, "%lf", &scale) != 1) scale = 1.0;
                fclose(fp);
            }
            if (i == 0) {
                static std::atomic<int> sl{0};
                if (++sl <= 30) Log("FILL file scale=%.3f", scale);
            }
        }
        *tx = table[i][0] * scale;
        *ty = table[i][1] * scale;
        *tz = table[i][2] * scale;
        return;
    }
    *tx = (i - (kFillN - 1) / 2.0) * spacing;
    *ty = 0;
    *tz = 0;
}
static uintptr_t FillSeparately(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f,
                                uintptr_t g, uintptr_t h)
{
    static std::atomic<long long> calls{0}, ticks{0};
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    bool mine = IsMine((void*)a);
    QueryPerformanceCounter(&t1);
    long long n = ++calls;
    ticks += t1.QuadPart - t0.QuadPart;
    if (n == 1 || n == 10 || n == 100 || n == 1000 || n == 10000 || n == 100000) {
        QueryPerformanceFrequency(&fq);
        Log("FILL calls slot217=%lld, id check total %.3f ms", n, ticks * 1000.0 / fq.QuadPart);
    }
    if (mine) return 1;
    return ((Fwd8)g_vtOrig[g_slotSep])(a, b, c, d, e, f, g, h);
}
static uintptr_t FillCount(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f,
                           uintptr_t g, uintptr_t h)
{
    // Rasterize2DGraph asks with an empty cache and switches to the layer-set raster node when >=2;
    // answer the original there so the normal (whole-layer) raster is used.
    static bool noCacheOrig = GetEnvironmentVariableA("DUPLIX_FILL_NOCACHE_ORIG", nullptr, 0) > 0;
    bool emptyCache = c && *(void**)c == nullptr;
    if (IsMine((void*)a) && !(noCacheOrig && emptyCache)) {
        int n = ++g_vtCalls[219];
        if (n <= 20) Log("FILL count this=%p -> %d", (void*)a, kFillN);
        return kFillN;
    }
    return ((Fwd8)g_vtOrig[g_slotCount])(a, b, c, d, e, f, g, h);
}
static uintptr_t FillRects(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f,
                           uintptr_t g, uintptr_t h)
{
    if (!IsMine((void*)a)) return ((Fwd8)g_vtOrig[g_slotRects])(a, b, c, d, e, f, g, h);
    struct Timer {  // total time spent in FillRects (logged at 1,10,100,1000 calls)
        LARGE_INTEGER t0;
        Timer() { QueryPerformanceCounter(&t0); }
        ~Timer()
        {
            static std::atomic<long long> calls{0}, ticks{0};
            LARGE_INTEGER t1, fq;
            QueryPerformanceCounter(&t1);
            ticks += t1.QuadPart - t0.QuadPart;
            long long n = ++calls;
            if (n == 1 || n == 10 || n == 100 || n == 1000 || n == 10000) {
                QueryPerformanceFrequency(&fq);
                Log("FILL rects calls=%lld total %.1f ms", n, ticks * 1000.0 / fq.QuadPart);
            }
        }
    } timer;
    typedef void (*M3Id)(void*);
    typedef void (*M3Tr)(void*, double, double);
    typedef void (*M4Tr)(void*, double, double, double);
    static HMODULE u = GetModuleHandleA("U.dll");
    static M3Id m3id = (M3Id)GetProcAddress(u, "?SetIdentity@M_Matrix3@@QEAAXXZ");
    static M3Tr m3tr = (M3Tr)GetProcAddress(u, "?Translate@M_Matrix3@@QEAAXNN@Z");
    static M3Id m4id = (M3Id)GetProcAddress(u, "?SetIdentity@M_Matrix4@@QEAAXXZ");
    static M4Tr m4tr = (M4Tr)GetProcAddress(u, "?Translate@M_Matrix4@@QEAAXNNN@Z");
    double* ret = (double*)b;
    const double* in = (const double*)d;
    RawVec* vR = (RawVec*)e;
    RawVec* v3 = (RawVec*)f;
    RawVec* v4 = (RawVec*)g;
    for (int k = 0; k < 4; k++) ret[k] = in[k];
    bool ok = m3id && m3tr && m4id && m4tr && VecReserve(vR, 32, kFillN) && VecReserve(v3, 72, kFillN) &&
              VecReserve(v4, 128, kFillN);
    if (ok) {
        static bool anim = GetEnvironmentVariableA("DUPLIX_FILL_ANIM", nullptr, 0) > 0;
        const int* tm = (const int*)c;
        static bool useParam = GetEnvironmentVariableA("DUPLIX_FILL_PARAM", nullptr, 0) > 0;
        t_paramScale = 0;
        if (useParam) {
            double sv = 0;
            bool got = ReadSliderOnLayer((void*)a, tm, &sv);
            t_paramScale = got ? (sv == 0 ? 1.0 : sv / 10.0) : 0;
            static std::atomic<int> pl{0};
            if (++pl <= 20) Log("FILL param slider got=%d value=%.3f -> scale %.3f (tid layer %p)", (int)got, sv, t_paramScale, (void*)a);
        }
        double tsec = (tm && tm[1]) ? (double)tm[0] / (double)(unsigned)tm[1] : 0.0;
        {
            static std::atomic<int> tl{0};
            if (anim && ++tl <= 40) Log("FILL time value=%d scale=%u -> %.5f s", tm ? tm[0] : 0, tm ? (unsigned)tm[1] : 0u, tsec);
        }
        for (int i = 0; i < kFillN; i++) {
            double tx, ty, tz;
            FillOffset(i, &tx, &ty, &tz);
            if (anim) tx += 300.0 * tsec;
            memcpy(vR->last, in, 32);
            vR->last += 32;
            m3id(v3->last);
            static bool m3id_only = GetEnvironmentVariableA("DUPLIX_FILL_M3ID", nullptr, 0) > 0;
            if (!m3id_only) m3tr(v3->last, tx, ty);
            v3->last += 72;
            m4id(v4->last);
            static bool m4id_only = GetEnvironmentVariableA("DUPLIX_FILL_M4ID", nullptr, 0) > 0;
            static bool useRot = GetEnvironmentVariableA("DUPLIX_FILL_ROT", nullptr, 0) > 0;
            if (useRot) {
                // rotation (degrees) about the rect centre, X then Y then Z, row vectors; then translate
                static const double rot[5][3] = {{0, 0, 0}, {0, 0, 30}, {0, 40, 0}, {35, 0, 0}, {20, 30, 40}};
                double ax = (in[0] + in[2]) / 2, ay = (in[1] + in[3]) / 2;
                double M[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {-ax, -ay, 0, 1}};
                auto mul = [&](const double R[4][4]) {
                    double o[4][4];
                    for (int r = 0; r < 4; r++)
                        for (int cc = 0; cc < 4; cc++) {
                            o[r][cc] = 0;
                            for (int k = 0; k < 4; k++) o[r][cc] += M[r][k] * R[k][cc];
                        }
                    memcpy(M, o, sizeof(o));
                };
                const double d2r = 3.14159265358979323846 / 180.0;
                const double* rr = FillRandom() ? RandomXform(i) + 3 : rot[i];
                double cx = cos(rr[0] * d2r), sx = sin(rr[0] * d2r);
                double cy = cos(rr[1] * d2r), sy = sin(rr[1] * d2r);
                double cz = cos(rr[2] * d2r), sz = sin(rr[2] * d2r);
                const double Rx[4][4] = {{1, 0, 0, 0}, {0, cx, sx, 0}, {0, -sx, cx, 0}, {0, 0, 0, 1}};
                const double Ry[4][4] = {{cy, 0, -sy, 0}, {0, 1, 0, 0}, {sy, 0, cy, 0}, {0, 0, 0, 1}};
                const double Rz[4][4] = {{cz, sz, 0, 0}, {-sz, cz, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
                const double Tb[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {ax + tx, ay + ty, tz, 1}};
                static std::string order = [] {
                    char b[8] = {0};
                    return std::string(GetEnvironmentVariableA("DUPLIX_FILL_ROTORDER", b, sizeof(b)) ? b : "XYZ");
                }();
                for (char ch : order) {
                    if (ch == 'X') mul(Rx);
                    if (ch == 'Y') mul(Ry);
                    if (ch == 'Z') mul(Rz);
                }
                mul(Tb);
                memcpy(v4->last, M, 128);
            } else if (!m4id_only) {
                m4tr(v4->last, tx, ty, tz);
            }
            static bool col = GetEnvironmentVariableA("DUPLIX_FILL_COL", nullptr, 0) > 0;
            if (col) {  // move translation from row 4 to column 4
                double* m = (double*)v4->last;
                m[3] = m[12]; m[7] = m[13]; m[11] = m[14];
                m[12] = m[13] = m[14] = 0;
            }
            v4->last += 128;
        }
    }
    int n = ++g_vtCalls[233];
    if (n <= 20) {
        Log("FILL rects this=%p in={%.1f,%.1f,%.1f,%.1f} ok=%d col=%d m3id=%d m4id=%d", (void*)a, in[0], in[1], in[2],
            in[3], (int)ok, GetEnvironmentVariableA("DUPLIX_FILL_COL", nullptr, 0) > 0,
            GetEnvironmentVariableA("DUPLIX_FILL_M3ID", nullptr, 0) > 0,
            GetEnvironmentVariableA("DUPLIX_FILL_M4ID", nullptr, 0) > 0);
        if (ok && v4->first + 128 < v4->last) {
            const double* m = (const double*)(v4->first + 128);
            const double* m3 = (const double*)(v3->first + 72);
            Log("FILL  M4[1] row3 = %.1f %.1f %.1f %.1f, col3 = %.1f %.1f %.1f | M3[1] = %.1f %.1f %.1f / %.1f %.1f %.1f / %.1f %.1f %.1f",
                m[12], m[13], m[14], m[15], m[3], m[7], m[11], m3[0], m3[1], m3[2], m3[3], m3[4], m3[5], m3[6], m3[7], m3[8]);
        }
    }
    return b;
}
// "textlog": call the original GetSubLayerRects (slot 233) and dump what it produced.
static void* g_textOrig233 = nullptr;
static uintptr_t LogRects233(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f,
                             uintptr_t g, uintptr_t h)
{
    uintptr_t r = ((Fwd8)g_textOrig233)(a, b, c, d, e, f, g, h);
    static std::atomic<int> calls{0};
    int n = ++calls;
    if (n > 4) return r;
    const double* in = (const double*)d;
    const double* ret = (const double*)b;
    RawVec* vR = (RawVec*)e;
    RawVec* v3 = (RawVec*)f;
    RawVec* v4 = (RawVec*)g;
    size_t nR = (vR->last - vR->first) / 32, n3 = (v3->last - v3->first) / 72, n4 = (v4->last - v4->first) / 128;
    Log("TEXT rects #%d this=%p in={%.2f,%.2f,%.2f,%.2f} ret={%.2f,%.2f,%.2f,%.2f} nR=%zu n3=%zu n4=%zu", n, (void*)a,
        in[0], in[1], in[2], in[3], ret[0], ret[1], ret[2], ret[3], nR, n3, n4);
    for (size_t k = 0; k < nR && k < 4; k++) {
        const double* R = (const double*)(vR->first + 32 * k);
        const double* m3 = (const double*)(v3->first + 72 * k);
        const double* m = (const double*)(v4->first + 128 * k);
        Log("TEXT  [%zu] rect={%.2f,%.2f,%.2f,%.2f}", k, R[0], R[1], R[2], R[3]);
        Log("TEXT  [%zu] M3= %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f", k, m3[0], m3[1], m3[2], m3[3], m3[4],
            m3[5], m3[6], m3[7], m3[8]);
        Log("TEXT  [%zu] M4= %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f", k,
            m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    }
    return r;
}
static void* g_textOrig[kVtSlots];
static void LogM3(const char* tag, int slot, int i, const double* m)
{
    if (m) Log("TEXT s%d i=%d %s M3= %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f", slot, i, tag, m[0], m[1], m[2], m[3],
               m[4], m[5], m[6], m[7], m[8]);
}
template <int SLOT> static uintptr_t TextOut(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e,
                                             uintptr_t f, uintptr_t g, uintptr_t h)
{
    uintptr_t r = ((Fwd8)g_textOrig[SLOT])(a, b, c, d, e, f, g, h);
    static std::atomic<int> calls{0};
    if (++calls > 12) return r;
    if (SLOT == 225) {
        const double* m = (const double*)d;
        Log("TEXT s225 i=%d M4= %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f",
            (int)b, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    } else if (SLOT == 223) {
        LogM3("a", 223, (int)b, (const double*)d);
    } else if (SLOT == 222) {
        LogM3("a", 222, (int)b, (const double*)d);
        LogM3("b", 222, (int)b, (const double*)e);
    } else if (SLOT == 221) {
        LogM3("a", 221, (int)b, (const double*)d);
        LogM3("b", 221, (int)b, (const double*)e);
        LogM3("c", 221, (int)b, (const double*)f);
    } else if (SLOT == 228 || SLOT == 229) {
        const double* R = (const double*)b;
        Log("TEXT s%d i=%d rect={%.2f,%.2f,%.2f,%.2f}", SLOT, (int)c, R[0], R[1], R[2], R[3]);
        char* cache = d ? *(char**)d : nullptr;
        if (cache) {
            RawVec* v4 = (RawVec*)(cache + 96);
            RawVec* v3 = (RawVec*)(cache + 120);
            RawVec* vR = (RawVec*)(cache + 144);
            size_t i = (size_t)(int)c;
            size_t n4 = (v4->last - v4->first) / 128, n3 = (v3->last - v3->first) / 72, nR = (vR->last - vR->first) / 32;
            Log("TEXT   cache=%p n4=%zu n3=%zu nR=%zu", cache, n4, n3, nR);
            if (i < nR) {
                const double* r = (const double*)(vR->first + 32 * i);
                Log("TEXT   rect[%zu]={%.2f,%.2f,%.2f,%.2f}", i, r[0], r[1], r[2], r[3]);
            }
            if (i < n3) LogM3("cache", 120, (int)i, (const double*)(v3->first + 72 * i));
            if (i < n4) {
                const double* m = (const double*)(v4->first + 128 * i);
                Log("TEXT   M4[%zu]= %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f", i,
                    m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
            }
        }
    }
    return r;
}
static void InstallTextLog(void* layer)
{
    void** vt = *(void***)layer;
    if (g_textOrig233) return;
    g_textOrig233 = vt[233];
    for (int i = 0; i < kVtSlots; i++) g_textOrig[i] = vt[i];
    DWORD old = 0;
    if (VirtualProtect(vt, kVtSlots * sizeof(void*), PAGE_READWRITE, &old)) {
        vt[233] = (void*)&LogRects233;
        Fwd8 w221 = &TextOut<221>, w222 = &TextOut<222>, w223 = &TextOut<223>, w225 = &TextOut<225>,
             w228 = &TextOut<228>, w229 = &TextOut<229>;
        vt[221] = (void*)w221;
        vt[222] = (void*)w222;
        vt[223] = (void*)w223;
        vt[225] = (void*)w225;
        vt[228] = (void*)w228;
        vt[229] = (void*)w229;
        VirtualProtect(vt, kVtSlots * sizeof(void*), old, &old);
        Log("TEXT log installed on vtable %p (orig 233 = %p)", (void*)vt, g_textOrig233);
    }
}

// IsLayerType (slot 79): sub_180385CD0 (Artisan: "layer must render flattened" if AV with effects)
// -> answer false for our layer only when called from there.
static uintptr_t g_cd0Begin = 0, g_cd0End = 0;  // fixed-address mode (DUPLIX_FILL_FXBYPASS=addr)
static uintptr_t g_betaFn = 0;                  // BEE_BetaFeature_AreExpressionEffectsAlwaysAllowedEnabled
// Is `ra` the return address of the IsLayerType(0) call in the Artisan "must flatten" check?
// Code shape there (AE 2025): call [rax+IsLayerType]; test al,al; jnz short; call BEE_BetaFeature_...;
// test al,al; jz short; mov rax,[rbx]; mov rcx,rbx; call [rax+disp32]  (the effects-count query).
static bool IsFlattenCheckSite(uintptr_t ra)
{
    static std::atomic<uintptr_t> yes[8], no[16];
    for (auto& y : yes) if (y == ra) return true;
    for (auto& n : no) if (n == ra) return false;
    const unsigned char* p = (const unsigned char*)ra;
    bool ok = p[0] == 0x84 && p[1] == 0xC0 && p[2] == 0x75 && p[4] == 0xE8;
    if (ok) {
        int rel = *(const int*)(p + 5);
        ok = (uintptr_t)(p + 9 + rel) == g_betaFn;
    }
    if (ok) ok = p[9] == 0x84 && p[10] == 0xC0 && p[11] == 0x74 && p[13] == 0x48 && p[14] == 0x8B && p[16] == 0x48 &&
                 p[17] == 0x8B && p[19] == 0xFF && p[20] == 0x90;
    // ...then (flatten check only, not the audio-synth check sub_18043E350):
    // [25] mov rdx,[rax]; mov rcx,rax; call [rdx+disp32]; [37] test eax,eax; jnz short;
    // [41] mov rax,[rbx]; mov rcx,rbx; call [rax+disp32] (UsesLayerStyles); [53] test al,al; jnz short;
    // [57] mov rcx,rbx; call BEE_GetSelectedTrackMatte
    if (ok) ok = p[25] == 0x48 && p[26] == 0x8B && p[27] == 0x10 && p[28] == 0x48 && p[29] == 0x8B && p[30] == 0xC8 &&
                 p[31] == 0xFF && p[32] == 0x92 && p[37] == 0x85 && p[38] == 0xC0 && p[39] == 0x75 && p[41] == 0x48 &&
                 p[42] == 0x8B && p[47] == 0xFF && p[48] == 0x90 && p[53] == 0x84 && p[54] == 0xC0 && p[55] == 0x75 &&
                 p[57] == 0x48 && p[58] == 0x8B && p[59] == 0xCB && p[60] == 0xE8;
    std::atomic<uintptr_t>* arr = ok ? yes : no;
    size_t cap = ok ? 8 : 16;
    for (size_t k = 0; k < cap; k++) {
        uintptr_t z = 0;
        if (arr[k].compare_exchange_strong(z, ra)) break;
    }
    Log("FILL IsLayerType(0) call site RA=%p (BEE+0x%llX) flatten-check=%d", (void*)ra,
        (unsigned long long)(ra - (uintptr_t)GetModuleHandleA("BEE.dll")), (int)ok);
    return ok;
}
static uintptr_t FillIsLayerType(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f,
                                 uintptr_t g, uintptr_t h)
{
    uintptr_t ra = (uintptr_t)_ReturnAddress();
    if ((int)b == 0 && IsMine((void*)a)) {
        bool hit = g_cd0End ? (ra >= g_cd0Begin && ra < g_cd0End) : IsFlattenCheckSite(ra);
        if (hit) {
            int n = ++g_vtCalls[79];
            if (n <= 5) Log("FILL IsLayerType(0) from flatten check -> false");
            return 0;
        }
    }
    return ((Fwd8)g_vtOrig[g_slotIsLayerType])(a, b, c, d, e, f, g, h);
}
// Find the vtable slot holding an exported function (robust against slot renumbering between versions).
static int FindSlot(void** vt, const char* mangled)
{
    void* fn = (void*)GetProcAddress(GetModuleHandleA("BEE.dll"), mangled);
    if (!fn) return -1;
    for (int i = 0; i < kVtSlots + 40; i++)
        if (vt[i] == fn) return i;
    return -1;
}

// GetSubLayerOpacity (double, per copy): DUPLIX_FILL_OPACITY=1 -> 0.3 + 0.7*((i*37)%10)/9 (same in jsx)
static int g_slotOpacity = -1;
static double FillOpacity(void* self, int i, const void* sp)
{
    if (IsMine(self)) {
        double op = 0.3 + 0.7 * ((i * 37) % 10) / 9.0;
        static std::atomic<int> n{0};
        if (++n <= 10) Log("FILL opacity i=%d -> %.4f", i, op);
        return op;
    }
    typedef double (*F)(void*, int, const void*);
    return ((F)g_vtOrig[g_slotOpacity])(self, i, sp);
}
// Per-copy opacity via AEGPDriver's import of BEE_GetArtisanLayerOpacity (the classic-3D Artisan path).
// The render trace gives the artisan layer: *(trace+16) - 48 = BEE_AVLayer*, -40 = sub-layer index.
typedef double (*ArtOpacityFn)(void*, void*, const void*, const void*);
static ArtOpacityFn g_origArtOpacity = nullptr;
static double HookArtOpacity(void* ctx, void* trace, const void* t, const void* bag)
{
    double op = g_origArtOpacity(ctx, trace, t, bag);
    if (trace) {
        char* v = *(char**)((char*)trace + 16);
        void* layer = v ? *(void**)(v - 48) : nullptr;
        int sub = v ? *(int*)(v - 40) : -1;
        if (layer && sub >= 0 && IsMine(layer)) {
            double f = 0.3 + 0.7 * ((sub * 37) % 10) / 9.0;
            static std::atomic<int> n{0};
            if (++n <= 10) Log("FILL artisan opacity sub=%d base=%.4f x %.4f", sub, op, f);
            op *= f;
        }
    }
    return op;
}
static bool PatchImport(HMODULE mod, const char* dll, const char* name, void* repl, void** orig)
{
    if (!mod) return false;
    auto base = (unsigned char*)mod;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((const char*)(base + d->Name), dll)) continue;
        auto names = (IMAGE_THUNK_DATA64*)(base + d->OriginalFirstThunk);
        auto iat = (IMAGE_THUNK_DATA64*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, name)) continue;
            DWORD old = 0;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
            *orig = (void*)iat->u1.Function;
            iat->u1.Function = (ULONGLONG)repl;
            VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}
static void PatchFillVtable(void** vt);
static void InstallFillHooks(void* layer)
{
    g_fillLayerId = LayerIdOf(layer);
    Log("FILL target layer=%p id=%d", layer, (int)g_fillLayerId);
    PatchFillVtable(*(void***)layer);
}
// GLOBAL_SETUP path (DUPLIX_FILL_GLOBAL=1): patch BEE_AVLayer's vtable without any layer instance;
// target layer ID from DUPLIX_FILL_LAYERID. Works for projects opened from disk / aerender.
static void InstallFillHooksGlobal(PF_InData* in_data)
{
    char b[32] = {0};
    if (!GetEnvironmentVariableA("DUPLIX_FILL_GLOBAL", b, sizeof(b))) return;
    g_pica = in_data->pica_basicP;
    if (GetEnvironmentVariableA("DUPLIX_FILL_LAYERID", b, sizeof(b))) g_fillLayerId = atoi(b);
    char ids[256] = {0};
    if (GetEnvironmentVariableA("DUPLIX_FILL_LAYERIDS", ids, sizeof(ids))) {  // "53,54" (BYEFFECT mode)
        for (char* s = ids; *s;) {
            RegisterMineId(atoi(s));
            while (*s && *s != ',') s++;
            if (*s == ',') s++;
        }
    }
    HMODULE bee = GetModuleHandleA("BEE.dll");
    void** vt = bee ? (void**)GetProcAddress(bee, "??_7BEE_AVLayer@@6B@") : nullptr;
    Log("FILL global: BEE=%p vtable=%p target id=%d", (void*)bee, (void*)vt, (int)g_fillLayerId);
    if (vt) PatchFillVtable(vt);
}
static void PatchFillVtable(void** vt)
{
    static bool done = false;
    if (done) return;
    CaptureVtOrig(vt);
    int sIs = FindSlot(vt, "?IsLayerType@BEE_AVLayer@@UEBAEH@Z");
    int sSep = FindSlot(vt, "?SubLayersRenderSeparately@BEE_AVLayer@@UEBA_NXZ");
    int sCnt = FindSlot(vt, "?SubLayerRenderCount@BEE_AVLayer@@UEBAHAEBUT_Time@@AEBV?$shared_ptr@VBEE_XformTimeCache@@@std@@PEBVTDB_ParamBag@@@Z");
    int sRc = FindSlot(vt, "?GetSubLayerRects@BEE_AVLayer@@UEBA?AUM_FloatRect@@AEBUT_Time@@AEBU2@AEAV?$vector@UM_FloatRect@@V?$allocator@UM_FloatRect@@@std@@@std@@AEAV?$vector@VM_Matrix3@@V?$allocator@VM_Matrix3@@@std@@@5@AEAV?$vector@VM_Matrix4@@V?$allocator@VM_Matrix4@@@std@@@5@PEBVTDB_ParamBag@@@Z");
    Log("FILL slots found: IsLayerType=%d Separately=%d Count=%d Rects=%d (analysed: 79 217 219 233)", sIs, sSep, sCnt, sRc);
    if (sIs < 0 || sSep < 0 || sCnt < 0 || sRc < 0 || sIs >= kVtSlots || sSep >= kVtSlots || sCnt >= kVtSlots ||
        sRc >= kVtSlots) {
        Log("FILL: slot lookup failed -> not installing");
        return;
    }
    g_slotIsLayerType = sIs; g_slotSep = sSep; g_slotCount = sCnt; g_slotRects = sRc;
    g_betaFn = (uintptr_t)GetProcAddress(GetModuleHandleA("BEE.dll"),
                                         "?BEE_BetaFeature_AreExpressionEffectsAlwaysAllowedEnabled@@YA_NXZ");
    DWORD old = 0;
    if (!VirtualProtect(vt, kVtSlots * sizeof(void*), PAGE_READWRITE, &old)) {
        Log("FILL: VirtualProtect failed %lu", GetLastError());
        return;
    }
    if (GetEnvironmentVariableA("DUPLIX_FILL_OPACITY", nullptr, 0) > 0) {
        g_slotOpacity = FindSlot(vt, "?GetSubLayerOpacity@BEE_AVLayer@@UEBANHAEBV?$shared_ptr@VBEE_XformCache@@@std@@@Z");
        Log("FILL opacity slot=%d (analysed 226)", g_slotOpacity);
        if (g_slotOpacity >= 0 && g_slotOpacity < kVtSlots) vt[g_slotOpacity] = (void*)&FillOpacity;
        bool ok = PatchImport(GetModuleHandleA("AEGPDriver.aex"), "BEE.dll",
                              "?BEE_GetArtisanLayerOpacity@@YANPEAXPEAVBEE_RenderTrace@@PEBUT_Time@@PEBVTDB_ParamBag@@@Z",
                              (void*)&HookArtOpacity, (void**)&g_origArtOpacity);
        Log("FILL artisan opacity import patch ok=%d (AEGPDriver=%p)", (int)ok, (void*)GetModuleHandleA("AEGPDriver.aex"));
    }
    vt[sSep] = (void*)&FillSeparately;
    vt[sCnt] = (void*)&FillCount;
    vt[sRc] = (void*)&FillRects;
    char fx[16] = {0};
    if (GetEnvironmentVariableA("DUPLIX_FILL_FXBYPASS", fx, sizeof(fx)) > 0) {
        if (!strcmp(fx, "addr")) {  // old fixed-address mode (AE 2025 build analysed)
            uintptr_t base = (uintptr_t)GetModuleHandleA("BEE.dll");
            g_cd0Begin = base + 0x385CD0;
            g_cd0End = base + 0x385DB0;
        }
        vt[sIs] = (void*)&FillIsLayerType;
        Log("FILL fx bypass on (%s, beta=%p)", g_cd0End ? "fixed range" : "code-shape match", (void*)g_betaFn);
    }
    VirtualProtect(vt, kVtSlots * sizeof(void*), old, &old);
    done = true;
    Log("FILL hooks installed on shared vtable %p", (void*)vt);
}

// DUPLIX_PROBE_SHADOWS=1: turn on "Casts Shadows" for every light, and for layers named Target/Copy*,
// in every comp of the project (setting it from ExtendScript blocks on a modal dialog in the hidden AE).
static void ProbeSetShadows(PF_InData* in_data)
{
    if (GetEnvironmentVariableA("DUPLIX_PROBE_SHADOWS", nullptr, 0) == 0) return;
    auto ps = SUITE(AEGP_ProjSuite6, kAEGPProjSuite, kAEGPProjSuiteVersion6);
    auto is = SUITE(AEGP_ItemSuite9, kAEGPItemSuite, kAEGPItemSuiteVersion9);
    auto cs = SUITE(AEGP_CompSuite12, kAEGPCompSuite, kAEGPCompSuiteVersion12);
    auto ls = SUITE(AEGP_LayerSuite9, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    auto ds = SUITE(AEGP_DynamicStreamSuite4, kAEGPDynamicStreamSuite, kAEGPDynamicStreamSuiteVersion4);
    auto ss = SUITE(AEGP_StreamSuite6, kAEGPStreamSuite, kAEGPStreamSuiteVersion6);
    auto ms = SUITE(AEGP_MemorySuite1, kAEGPMemorySuite, kAEGPMemorySuiteVersion1);
    if (!ps || !is || !cs || !ls || !ds || !ss || !ms) { Log("SHADOWS: suite missing"); return; }
    AEGP_ProjectH proj = nullptr;
    if (ps->AEGP_GetProjectByIndex(0, &proj) || !proj) return;
    auto setOne = [&](AEGP_LayerH L, const char* grp, const char* leaf) {
        AEGP_StreamRefH root = nullptr, g = nullptr, s = nullptr;
        bool ok = false;
        if (!ds->AEGP_GetNewStreamRefForLayer(g_pluginId, L, &root) && root &&
            !ds->AEGP_GetNewStreamRefByMatchname(g_pluginId, root, grp, &g) && g &&
            !ds->AEGP_GetNewStreamRefByMatchname(g_pluginId, g, leaf, &s) && s) {
            AEGP_StreamValue2 v = {};
            v.streamH = s;
            v.val.one_d = 1;
            ok = !ss->AEGP_SetStreamValue(g_pluginId, s, &v);
        }
        if (s) ss->AEGP_DisposeStream(s);
        if (g) ss->AEGP_DisposeStream(g);
        if (root) ss->AEGP_DisposeStream(root);
        return ok;
    };
    AEGP_ItemH item = nullptr;
    is->AEGP_GetFirstProjItem(proj, &item);
    while (item) {
        AEGP_ItemType t;
        if (!is->AEGP_GetItemType(item, &t) && t == AEGP_ItemType_COMP) {
            AEGP_CompH comp = nullptr;
            cs->AEGP_GetCompFromItem(item, &comp);
            A_long n = 0;
            if (comp) ls->AEGP_GetCompNumLayers(comp, &n);
            for (A_long i = 0; i < n; i++) {
                AEGP_LayerH L = nullptr;
                ls->AEGP_GetCompLayerByIndex(comp, i, &L);
                AEGP_ObjectType ot;
                if (!L || ls->AEGP_GetLayerObjectType(L, &ot)) continue;
                AEGP_MemHandle nameH = nullptr, srcH = nullptr;
                std::string name;
                if (!ls->AEGP_GetLayerName(g_pluginId, L, &nameH, &srcH) && nameH) {
                    A_UTF16Char* u = nullptr;
                    ms->AEGP_LockMemHandle(nameH, (void**)&u);
                    for (; u && *u; u++) name += (char)*u;
                    ms->AEGP_UnlockMemHandle(nameH);
                    ms->AEGP_FreeMemHandle(nameH);
                    if (srcH) ms->AEGP_FreeMemHandle(srcH);
                }
                if (ot == AEGP_ObjectType_LIGHT)
                    Log("SHADOWS: light '%s' -> %d", name.c_str(),
                        setOne(L, "ADBE Light Options Group", "ADBE Light Casts Shadows"));
                else if (name == "Target" || name.rfind("Copy", 0) == 0)
                    Log("SHADOWS: layer '%s' -> %d", name.c_str(),
                        setOne(L, "ADBE Material Options Group", "ADBE Casts Shadows"));
            }
        }
        AEGP_ItemH next = nullptr;
        is->AEGP_GetNextProjItem(proj, item, &next);
        item = next;
    }
}

typedef void* (*BeeCreateSet)(void* avlayer);
typedef int (*BeeCmdSetLayerSwitch)(void* layer, int sw, unsigned char on);
static void ProbeSets(PF_InData* in_data)
{
    char mode[32] = {0};
    if (!GetEnvironmentVariableA("DUPLIX_PROBE_SETS", mode, sizeof(mode))) return;
    AEGP_LayerH L = nullptr;
    std::string compName;
    A_long idx = -1;
    if (!FindTarget(in_data, &L, &compName, &idx) || !L) { Log("PROBE sets: no Target"); return; }
    HMODULE bee = GetModuleHandleA("BEE.dll");
    auto create = bee ? (BeeCreateSet)GetProcAddress(bee, "?CreateSet@BEE_AVLayer@@QEAAPEAVBEE_LayerSet@@XZ") : nullptr;
    auto sw = bee ? (BeeCmdSetLayerSwitch)GetProcAddress(bee, "?BEE_CmdSetLayerSwitch@@YAHPEAVBEE_Layer@@HE@Z") : nullptr;
    auto us = SUITE(AEGP_UtilitySuite6, kAEGPUtilitySuite, kAEGPUtilitySuiteVersion6);
    Log("PROBE sets: mode=%s layer=%p create=%p sw=%p", mode, (void*)L, (void*)create, (void*)sw);
    if (us) us->AEGP_StartUndoGroup("DupliX probe sets");
    try {
        if (!strcmp(mode, "create") && create) {
            void* set = create((void*)L);
            Log("PROBE sets: CreateSet -> %p", set);
        } else if (!strcmp(mode, "textlog")) {
            InstallTextLog((void*)L);
        } else if (!strcmp(mode, "fill")) {
            g_pica = in_data->pica_basicP;
            if (GetEnvironmentVariableA("DUPLIX_FILL_BYEFFECT", nullptr, 0) > 0) {
                // register every layer of this comp that carries the marker effect (safe here: SEQUENCE_SETUP)
                auto pfi = SUITE(AEGP_PFInterfaceSuite1, kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1);
                auto ls = SUITE(AEGP_LayerSuite9, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
                AEGP_LayerH self = nullptr;
                AEGP_CompH comp = nullptr;
                A_long n = 0;
                if (pfi && ls && !pfi->AEGP_GetEffectLayer(in_data->effect_ref, &self) && self &&
                    !ls->AEGP_GetLayerParentComp(self, &comp) && comp && !ls->AEGP_GetCompNumLayers(comp, &n)) {
                    for (A_long i = 0; i < n; i++) {
                        AEGP_LayerH Li = nullptr;
                        if (!ls->AEGP_GetCompLayerByIndex(comp, i, &Li) && Li && LayerHasMarker((void*)Li))
                            RegisterMineId(LayerIdOf((void*)Li));
                    }
                }
            }
            char vtlog[8] = {0};
            if (GetEnvironmentVariableA("DUPLIX_PROBE_VTLOG", vtlog, sizeof(vtlog))) InstallVtLogger((void*)L);
            InstallFillHooks((void*)L);
        } else if (!strcmp(mode, "prop") && create) {
            // CreateSet, then BEE_LayerSet::AddProperty(spec of Position). AddProperty adds the
            // property, sets its reference stream and one stop = position + (100,100,100).
            // Copy i of N is then offset by i*(stop-pos)/N (BEE_LayerSet::GetSubLayerRects).
            typedef void* (*BeeAddProperty)(void* set, void* retPath, const void* spec);
            typedef void* (*BeeSpecCtor)(void* spec, void* layer, const void* path);
            typedef void (*BeeDtor)(void* self);
            HMODULE tdb = GetModuleHandleA("TDB.dll");
            auto addProp = (BeeAddProperty)GetProcAddress(
                bee, "?AddProperty@BEE_LayerSet@@QEAA?AVTDB_StreamIDPath@@AEBVBEE_StreamSpec@@@Z");
            auto specCtor = (BeeSpecCtor)GetProcAddress(
                bee, "??0BEE_StreamSpec@@QEAA@PEAVBEE_Layer@@AEBVTDB_StreamIDPath@@@Z");
            auto specDtor = (BeeDtor)GetProcAddress(bee, "??1BEE_StreamSpec@@UEAA@XZ");
            auto pathDtor = tdb ? (BeeDtor)GetProcAddress(tdb, "??1TDB_StreamIDPath@@UEAA@XZ") : nullptr;
            Log("PROBE sets: addProp=%p specCtor=%p specDtor=%p pathDtor=%p", (void*)addProp, (void*)specCtor,
                (void*)specDtor, (void*)pathDtor);
            auto ss = SUITE(AEGP_StreamSuite6, kAEGPStreamSuite, kAEGPStreamSuiteVersion6);
            AEGP_StreamRefH pos = nullptr;
            if (addProp && specCtor && specDtor && pathDtor && ss &&
                !ss->AEGP_GetNewLayerStream(g_pluginId, L, AEGP_LayerStream_POSITION, &pos) && pos) {
                void* refLayer = *(void**)((char*)pos + 120);
                Log("PROBE sets: pos ref=%p layer@+120=%p (L=%p)", (void*)pos, refLayer, (void*)L);
                if (refLayer == (void*)L) {
                    void* set = create((void*)L);
                    Log("PROBE sets: CreateSet -> %p", set);
                    alignas(16) unsigned char spec[96] = {0};
                    alignas(16) unsigned char path[96] = {0};
                    specCtor(spec, (void*)L, (char*)pos + 80);
                    if (set) {
                        addProp(set, path, spec);
                        Log("PROBE sets: AddProperty done");
                        pathDtor(path);
                    }
                    specDtor(spec);
                    typedef unsigned long long (*BeeGetFlags)(void* self);
                    auto getFlags = [&]() { return (*(BeeGetFlags*)(*(char**)L + 0x2A8))((void*)L); };
                    Log("PROBE sets: layer flags after set = 0x%llX", getFlags());
                    {
                        // Ask the layer directly (vtable slots of BEE_AVLayer, see dump_sublayer).
                        char* vt = *(char**)L;
                        typedef bool (*VBool)(void*);
                        typedef int (*VCount)(void*, const void* time, const void* cacheSp, const void* bag);
                        typedef void* (*VRects)(void*, void* ret, const void* time, const void* inRect, void* vR,
                                                void* vM3, void* vM4, const void* bag);
                        int time[2] = {0, 1};
                        void* nullSp[2] = {nullptr, nullptr};
                        bool can = (*(VBool*)(vt + 0x6B8))((void*)L);
                        bool sep = (*(VBool*)(vt + 0x6C8))((void*)L);
                        int cnt = (*(VCount*)(vt + 0x6D8))((void*)L, time, nullSp, nullptr);
                        Log("PROBE sets: CanSeparately=%d Separately=%d SubLayerRenderCount(nocache)=%d", can, sep, cnt);
                        double inRect[4] = {0, 0, 100, 100};
                        double ret[4] = {0};
                        double* vR[3] = {nullptr, nullptr, nullptr};
                        double* vM3[3] = {nullptr, nullptr, nullptr};
                        double* vM4[3] = {nullptr, nullptr, nullptr};
                        (*(VRects*)(vt + 0x748))((void*)L, ret, time, inRect, vR, vM3, vM4, nullptr);
                        size_t nR = (vR[1] - vR[0]) / 4, n3 = (vM3[1] - vM3[0]) / 9, n4 = (vM4[1] - vM4[0]) / 16;
                        Log("PROBE sets: GetSubLayerRects ret={%.1f,%.1f,%.1f,%.1f} nRect=%zu nM3=%zu nM4=%zu", ret[0],
                            ret[1], ret[2], ret[3], nR, n3, n4);
                        for (size_t k = 0; k < n4 && k < 3; k++) {
                            double* m = vM4[0] + 16 * k;
                            Log("PROBE sets:  M4[%zu] = %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | "
                                "%.2f %.2f %.2f %.2f",
                                k, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12],
                                m[13], m[14], m[15]);
                        }
                        // vectors were allocated by BEE (ucrtbase heap); leak them in this probe.
                    }
                    char vtlog[8] = {0};
                    if (GetEnvironmentVariableA("DUPLIX_PROBE_VTLOG", vtlog, sizeof(vtlog))) InstallVtLogger((void*)L);
                    char extra[32] = {0};
                    if (GetEnvironmentVariableA("DUPLIX_PROBE_SW", extra, sizeof(extra)) && sw) {
                        int bit = (int)strtol(extra, nullptr, 0);
                        int r = sw((void*)L, bit, 1);
                        Log("PROBE sets: switch 0x%X -> %d, flags now 0x%llX", bit, r, getFlags());
                    }
                }
                ss->AEGP_DisposeStream(pos);
            }
        } else if (!strcmp(mode, "switch") && sw) {
            int r1 = sw((void*)L, 0x80000, 1);
            int r2 = sw((void*)L, 128, 1);
            Log("PROBE sets: switch 0x80000 -> %d, 128 -> %d", r1, r2);
        }
    } catch (int& e) {
        Log("PROBE sets: threw int %d", e);
    } catch (...) {
        Log("PROBE sets: threw unknown");
    }
    if (us) us->AEGP_EndUndoGroup();
}

// ---------------------------------------------------------------- entry
extern "C" DllExport PF_Err PluginDataEntryFunction2(PF_PluginDataPtr inPtr, PF_PluginDataCB2 inPluginDataCallBackPtr,
                                                     SPBasicSuite* inSPBasicSuitePtr, const char* inHostName,
                                                     const char* inHostVersion)
{
    PF_Err result = PF_Err_INVALID_CALLBACK;
    result = PF_REGISTER_EFFECT_EXT2(inPtr, inPluginDataCallBackPtr, "DupliXSpike", "RNDM DupliXSpike",
                                     "DupliX Dev", AE_RESERVED_INFO, "EffectMain", "https://example.invalid");
    return result;
}

PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                  PF_LayerDef* output, void* extra)
{
    PF_Err err = PF_Err_NONE;
    try {
        switch (cmd) {
        case PF_Cmd_ABOUT:
            strcpy_s(out_data->return_msg, "DupliXSpike (throwaway)");
            break;
        case PF_Cmd_GLOBAL_SETUP:
            err = GlobalSetup(in_data, out_data);
            InstallFillHooksGlobal(in_data);
            break;
        case PF_Cmd_PARAMS_SETUP:
            err = ParamsSetup(in_data, out_data);
            break;
        case PF_Cmd_SEQUENCE_SETUP:
            ProbeSetShadows(in_data);
            ProbeSets(in_data);
            break;
        case PF_Cmd_SMART_PRE_RENDER:
            err = PreRender(in_data, out_data, (PF_PreRenderExtra*)extra);
            break;
        case PF_Cmd_SMART_RENDER:
            err = SmartRender(in_data, out_data, (PF_SmartRenderExtra*)extra);
            break;
        default:
            break;
        }
    } catch (PF_Err& e) {
        err = e;
    } catch (...) {
        Log("EffectMain: unknown exception cmd=%d", cmd);
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    return err;
}
