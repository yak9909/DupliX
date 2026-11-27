// P0 spike (design v4, sections 7 and 8): can a separate window (the graph editor) rewrite the effect's graph data
// (one Arbitrary Data parameter) so that one Ctrl+Z takes the change back?
//
// The effect fills the layer with the color stored in its graph data, so a test reads the graph data from the
// rendered pixels (tests/ae/p0_graph.jsx). The editor is a plain Win32 window made on AE's main thread when
// "Open Editor" is switched on (USER_CHANGED_PARAM). Its "write" (a button, or a timer when DUPLIX_P0_AUTO is set)
// does: AEGP_StartUndoGroup -> AEGP_GetNewStreamValue (the graph data) -> a new handle with the new color and the
// next revision -> AEGP_SetStreamValue -> AEGP_EndUndoGroup. Every step's error code is logged (DUPLIX_LOG).
// Throwaway code (spike/README.md).
#include "AEConfig.h"
#include <Windows.h>

#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "AE_EffectCBSuites.h"
#include "AE_GeneralPlug.h"
#include "AE_EffectSuites.h"
#include "AE_AdvEffectSuites.h"
#include "AE_EffectUI.h"
#include "Param_Utils.h"
#include "String_Utils.h"

#include <chrono>
#include <cmath>
#include <mutex>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

enum { P_INPUT = 0, P_OPEN, P_GRAPH, P_MACRO1, P_OVERVIEW, P_ALPHA_LAYER, P_SAMPLE_TIME, P_NUM };
enum { D_OPEN = 1, D_GRAPH, D_MACRO1, D_OVERVIEW, D_ALPHA_LAYER, D_SAMPLE_TIME };
constexpr A_u_long kMagic = 0x31475844;  // "DXG1"
void* const kArbRefcon = (void*)0x50304758;

#pragma pack(push, 4)
struct GraphHeader {
    A_u_long magic;
    A_u_long revision;    // +1 on every write from the editor
    float r, g, b;        // the color the effect fills with
    char macroName[32];   // the name Macro 1 is shown with (PF_UpdateParamUI)
    float macroMax;       // Macro 1's slider maximum
    A_u_long payload;     // bytes after the header (a stand-in for a graph of that size)
};
#pragma pack(pop)

SPBasicSuite* g_pica = nullptr;
AEGP_PluginID g_pluginId = 0;

void Log(const char* fmt, ...)
{
    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("DUPLIX_LOG", path, sizeof(path))) return;
    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "a") || !fp) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(fp, "[%02d:%02d:%02d.%03d][tid %lu] p0: ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fprintf(fp, "\n");
    fclose(fp);
}

template <class T>
struct Suite {
    T* s = nullptr;
    const char* name;
    int32 ver;
    Suite(const char* n, int32 v) : name(n), ver(v)
    {
        if (!g_pica || g_pica->AcquireSuite(n, v, (const void**)&s) != kSPNoError) s = nullptr;
    }
    ~Suite()
    {
        if (s) g_pica->ReleaseSuite(name, ver);
    }
    T* operator->() const { return s; }
    explicit operator bool() const { return s != nullptr; }
};

// ---- the graph data (Arbitrary Data) ----
PF_Handle NewGraph(float r, float g, float b, A_u_long revision, A_u_long payload, const char* macroName = "Macro 1",
                   float macroMax = 100)
{
    Suite<PF_HandleSuite1> hs(kPFHandleSuite, kPFHandleSuiteVersion1);
    if (!hs) return nullptr;
    PF_Handle h = hs->host_new_handle(sizeof(GraphHeader) + payload);
    if (!h) return nullptr;
    auto* p = (GraphHeader*)hs->host_lock_handle(h);
    std::memset(p, 0, sizeof(GraphHeader) + payload);
    p->magic = kMagic, p->revision = revision, p->r = r, p->g = g, p->b = b, p->payload = payload;
    strncpy_s(p->macroName, macroName, _TRUNCATE);
    p->macroMax = macroMax;
    for (A_u_long i = 0; i < payload; i++) ((unsigned char*)(p + 1))[i] = (unsigned char)(i * 7 + revision);
    hs->host_unlock_handle(h);
    return h;
}

bool ReadGraph(PF_Handle h, GraphHeader* out)
{
    Suite<PF_HandleSuite1> hs(kPFHandleSuite, kPFHandleSuiteVersion1);
    if (!hs || !h || hs->host_get_handle_size(h) < sizeof(GraphHeader)) return false;
    auto* p = (const GraphHeader*)hs->host_lock_handle(h);
    *out = *p;
    hs->host_unlock_handle(h);
    return out->magic == kMagic;
}

PF_Handle CopyHandle(PF_Handle src)
{
    Suite<PF_HandleSuite1> hs(kPFHandleSuite, kPFHandleSuiteVersion1);
    if (!hs || !src) return nullptr;
    const A_HandleSize n = hs->host_get_handle_size(src);
    PF_Handle d = hs->host_new_handle(n);
    if (!d) return nullptr;
    std::memcpy(hs->host_lock_handle(d), hs->host_lock_handle(src), n);
    hs->host_unlock_handle(src);
    hs->host_unlock_handle(d);
    return d;
}

PF_Err HandleArb(PF_ArbParamsExtra* x)
{
    Suite<PF_HandleSuite1> hs(kPFHandleSuite, kPFHandleSuiteVersion1);
    if (!hs) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    switch (x->which_function) {
    case PF_Arbitrary_NEW_FUNC:
        *x->u.new_func_params.arbPH = NewGraph(0.5f, 0.5f, 0.5f, 0, 0);
        return *x->u.new_func_params.arbPH ? PF_Err_NONE : PF_Err_OUT_OF_MEMORY;
    case PF_Arbitrary_DISPOSE_FUNC:
        if (x->u.dispose_func_params.arbH) hs->host_dispose_handle(x->u.dispose_func_params.arbH);
        return PF_Err_NONE;
    case PF_Arbitrary_COPY_FUNC:
        *x->u.copy_func_params.dst_arbPH = CopyHandle(x->u.copy_func_params.src_arbH);
        return *x->u.copy_func_params.dst_arbPH ? PF_Err_NONE : PF_Err_OUT_OF_MEMORY;
    case PF_Arbitrary_FLAT_SIZE_FUNC:
        *x->u.flat_size_func_params.flat_data_sizePLu = (A_u_long)hs->host_get_handle_size(x->u.flat_size_func_params.arbH);
        return PF_Err_NONE;
    case PF_Arbitrary_FLATTEN_FUNC: {
        const A_HandleSize n = hs->host_get_handle_size(x->u.flatten_func_params.arbH);
        if (x->u.flatten_func_params.buf_sizeLu < n) return PF_Err_INTERNAL_STRUCT_DAMAGED;
        std::memcpy(x->u.flatten_func_params.flat_dataPV, hs->host_lock_handle(x->u.flatten_func_params.arbH), n);
        hs->host_unlock_handle(x->u.flatten_func_params.arbH);
        return PF_Err_NONE;
    }
    case PF_Arbitrary_UNFLATTEN_FUNC: {
        const A_u_long n = x->u.unflatten_func_params.buf_sizeLu;
        if (n < sizeof(GraphHeader) || ((const GraphHeader*)x->u.unflatten_func_params.flat_dataPV)->magic != kMagic) {
            *x->u.unflatten_func_params.arbPH = NewGraph(0.5f, 0.5f, 0.5f, 0, 0);
        } else {
            PF_Handle h = hs->host_new_handle(n);
            std::memcpy(hs->host_lock_handle(h), x->u.unflatten_func_params.flat_dataPV, n);
            hs->host_unlock_handle(h);
            *x->u.unflatten_func_params.arbPH = h;
        }
        return PF_Err_NONE;
    }
    case PF_Arbitrary_INTERP_FUNC:  // CANNOT_TIME_VARY: never between two keys; the left one
        *x->u.interp_func_params.interpPH = CopyHandle(x->u.interp_func_params.left_arbH);
        return PF_Err_NONE;
    case PF_Arbitrary_COMPARE_FUNC: {
        PF_Handle a = x->u.compare_func_params.a_arbH, b = x->u.compare_func_params.b_arbH;
        const A_HandleSize na = hs->host_get_handle_size(a), nb = hs->host_get_handle_size(b);
        bool same = na == nb && !std::memcmp(hs->host_lock_handle(a), hs->host_lock_handle(b), na);
        if (na == nb) hs->host_unlock_handle(a), hs->host_unlock_handle(b);
        *x->u.compare_func_params.compareP = same ? PF_ArbCompare_EQUAL : PF_ArbCompare_NOT_EQUAL;
        return PF_Err_NONE;
    }
    case PF_Arbitrary_PRINT_SIZE_FUNC:
        *x->u.print_size_func_params.print_sizePLu = 64;
        return PF_Err_NONE;
    case PF_Arbitrary_PRINT_FUNC: {
        GraphHeader g{};
        if (x->u.print_func_params.print_sizeLu && x->u.print_func_params.print_bufferPC)
            snprintf(x->u.print_func_params.print_bufferPC, x->u.print_func_params.print_sizeLu, "graph rev %u",
                     ReadGraph(x->u.print_func_params.arbH, &g) ? (unsigned)g.revision : 0u);
        return PF_Err_NONE;
    }
    case PF_Arbitrary_SCAN_FUNC:
        return PF_Err_UNRECOGNIZED_PARAM_TYPE;
    }
    return PF_Err_NONE;
}

// ---- the editor window ----
AEGP_EffectRefH g_effect = nullptr;  // the instance the editor edits (kept while the window is open)
HWND g_wnd = nullptr;
int g_autoStep = 0;
A_u_long g_editorRev = 0;  // the revision the editor shows (design v4 section 7: synced with AE's undo by it)

// The graph data's revision now (read through AEGP; the idle hook compares it with the editor's).
bool CurrentRevision(A_u_long* rev)
{
    Suite<AEGP_StreamSuite6> ss(kAEGPStreamSuite, kAEGPStreamSuiteVersion6);
    if (!ss || !g_effect) return false;
    AEGP_StreamRefH st = nullptr;
    if (ss->AEGP_GetNewEffectStreamByIndex(g_pluginId, g_effect, P_GRAPH, &st) || !st) return false;
    AEGP_StreamValue2 v{};
    A_Time t{0, 1};
    bool ok = false;
    if (!ss->AEGP_GetNewStreamValue(g_pluginId, st, AEGP_LTimeMode_LayerTime, &t, FALSE, &v)) {
        GraphHeader g{};
        ok = ReadGraph((PF_Handle)v.val.arbH, &g);
        *rev = g.revision;
        ss->AEGP_DisposeStreamValue(&v);
    }
    ss->AEGP_DisposeStream(st);
    return ok;
}

// The editor's write: one undo step that replaces the graph data.
void WriteGraph(float r, float g, float b, const char* why)
{
    if (!g_effect) return;
    const auto t0 = std::chrono::steady_clock::now();
    Suite<AEGP_UtilitySuite6> util(kAEGPUtilitySuite, kAEGPUtilitySuiteVersion6);
    Suite<AEGP_StreamSuite6> ss(kAEGPStreamSuite, kAEGPStreamSuiteVersion6);
    if (!util || !ss) {
        Log("write: suites missing");
        return;
    }
    Log("write (%s): start", why);
    A_Err e0 = util->AEGP_StartUndoGroup("Edit Graph");
    AEGP_StreamRefH st = nullptr;
    A_Err e1 = ss->AEGP_GetNewEffectStreamByIndex(g_pluginId, g_effect, P_GRAPH, &st);
    AEGP_StreamValue2 v{};
    A_Time t{0, 1};
    A_Err e2 = st ? ss->AEGP_GetNewStreamValue(g_pluginId, st, AEGP_LTimeMode_LayerTime, &t, FALSE, &v) : -1;
    Log("write (%s): got the value (start %d stream %d get %d)", why, (int)e0, (int)e1, (int)e2);
    GraphHeader cur{};
    const bool ok = !e2 && ReadGraph((PF_Handle)v.val.arbH, &cur);
    char pay[32] = {0};
    const A_u_long payload = GetEnvironmentVariableA("DUPLIX_P0_PAYLOAD", pay, sizeof(pay)) ? strtoul(pay, nullptr, 10) : 0;
    // the macro's name and range travel in the graph data (red: "Spread" 0..300, green: "Density" 0..500)
    PF_Handle nh = NewGraph(r, g, b, cur.revision + 1, payload, g > 0.5f ? "Density" : "Spread", g > 0.5f ? 500.f : 300.f);
    A_Err e3 = -1;
    if (ok && nh) {
        AEGP_StreamValue2 nv = v;
        nv.val.arbH = (AEGP_ArbBlockVal)nh;
        e3 = ss->AEGP_SetStreamValue(g_pluginId, st, &nv);
        if (!e3) g_editorRev = cur.revision + 1;
    }
    if (nh) {
        Suite<PF_HandleSuite1> hs(kPFHandleSuite, kPFHandleSuiteVersion1);
        if (hs) hs->host_dispose_handle(nh);  // SetStreamValue does not adopt it (it copies through COPY_FUNC)
    }
    if (!e2) ss->AEGP_DisposeStreamValue(&v);
    if (st) ss->AEGP_DisposeStream(st);
    A_Err e4 = util->AEGP_EndUndoGroup();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Log("write (%s): rgb %.2f %.2f %.2f rev %u -> %u payload %lu: start %d stream %d get %d (read %d) set %d end %d, %.2f ms",
        why, r, g, b, (unsigned)cur.revision, (unsigned)cur.revision + 1, (unsigned long)payload, (int)e0, (int)e1,
        (int)e2, (int)ok, (int)e3, (int)e4, ms);
}

// Path (b) of the design (DUPLIX_P0_PATH=idle): the window only queues the write; AE's idle hook does it.
struct Pending {
    bool on = false;
    float r = 0, g = 0, b = 0;
    const char* why = "";
} g_pending;
bool IdlePath()
{
    char v[16] = {0};
    return GetEnvironmentVariableA("DUPLIX_P0_PATH", v, sizeof(v)) > 0 && !strcmp(v, "idle");
}
void RequestWrite(float r, float g, float b, const char* why)
{
    if (!IdlePath()) {
        WriteGraph(r, g, b, why);
        return;
    }
    g_pending = {true, r, g, b, why};
    Log("write (%s): queued for the idle hook", why);
}
A_Err IdleHook(AEGP_GlobalRefcon, AEGP_IdleRefcon, A_long* max_sleep)
{
    if (g_pending.on) {
        Pending p = g_pending;
        g_pending.on = false;
        WriteGraph(p.r, p.g, p.b, p.why);
        if (GetEnvironmentVariableA("DUPLIX_P0_REFRESH", nullptr, 0) > 0) {
            // the Overview must show the edit at once (UPDATE_PARAMS_UI does not come after an AEGP write)
            Suite<PF_AdvAppSuite2> adv(kPFAdvAppSuite, kPFAdvAppSuiteVersion2);
            const PF_Err e = adv ? adv->PF_RefreshAllWindows() : -1;
            Log("refresh all windows after the write: err %d", (int)e);
        }
    } else if (g_wnd) {
        // AE's own undo / redo (or a script) changed the graph data: the editor reloads it
        A_u_long rev = 0;
        const auto t0 = std::chrono::steady_clock::now();
        if (CurrentRevision(&rev) && rev != g_editorRev) {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            Log("sync: the graph data changed outside the editor: rev %u -> %u (reload; read %.3f ms)",
                (unsigned)g_editorRev, (unsigned)rev, ms);
            g_editorRev = rev;
        }
    }
    if (g_wnd && max_sleep && *max_sleep > 6) *max_sleep = 6;  // the editor is open: come back soon
    return A_Err_NONE;
}

LRESULT CALLBACK EditorProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == 1) RequestWrite(1, 0, 0, "button red");
        if (LOWORD(w) == 2) RequestWrite(0, 1, 0, "button green");
        return 0;
    case WM_TIMER: {  // DUPLIX_P0_AUTO: the same writes as the buttons, from the window's own message loop
        static bool busy = false;
        if (busy) {  // AEGP calls may run the message loop: a timer inside a write is not a new write
            Log("timer inside a write (step %d): skipped", g_autoStep);
            return 0;
        }
        busy = true;
        if (++g_autoStep == 1) RequestWrite(1, 0, 0, "auto red");
        else if (g_autoStep == 2) {
            KillTimer(h, 1);
            RequestWrite(0, 1, 0, "auto green");
        }
        busy = false;
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (g_effect) {
            Suite<AEGP_EffectSuite5> es(kAEGPEffectSuite, kAEGPEffectSuiteVersion5);
            if (es) es->AEGP_DisposeEffect(g_effect);
            g_effect = nullptr;
        }
        g_wnd = nullptr;
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

void OpenEditor(PF_InData* in_data)
{
    if (g_wnd) {
        SetForegroundWindow(g_wnd);
        return;
    }
    Suite<AEGP_PFInterfaceSuite1> pfi(kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1);
    A_Err e = pfi ? pfi->AEGP_GetNewEffectForEffect(g_pluginId, in_data->effect_ref, &g_effect) : -1;
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleA("DupliXP0.aex");
    if (!registered) {
        WNDCLASSA wc{};
        wc.lpfnWndProc = EditorProc;
        wc.hInstance = inst;
        wc.lpszClassName = "DupliXP0Editor";
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        registered = RegisterClassA(&wc) != 0;
    }
    g_wnd = CreateWindowExA(0, "DupliXP0Editor", "DupliX P0 Editor", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            320, 140, nullptr, nullptr, inst, nullptr);
    if (g_wnd) {
        CreateWindowA("BUTTON", "Red", WS_CHILD | WS_VISIBLE, 10, 10, 90, 30, g_wnd, (HMENU)1, inst, nullptr);
        CreateWindowA("BUTTON", "Green", WS_CHILD | WS_VISIBLE, 110, 10, 90, 30, g_wnd, (HMENU)2, inst, nullptr);
        ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
        g_autoStep = 0;
        if (GetEnvironmentVariableA("DUPLIX_P0_AUTO", nullptr, 0) > 0) SetTimer(g_wnd, 1, 1500, nullptr);
    }
    Log("editor opened: effect ref %p (err %d) window %p", (void*)g_effect, (int)e, (void*)g_wnd);
}

// ---- the effect ----
PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data)
{
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Open Editor", "", FALSE, PF_ParamFlag_SUPERVISE, D_OPEN);
    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_ARBITRARY_DATA;
    PF_STRNNCPY(def.PF_DEF_NAME, "Graph", sizeof(def.PF_DEF_NAME));
    def.flags = PF_ParamFlag_CANNOT_TIME_VARY;
    def.ui_flags = PF_PUI_NO_ECW_UI;
    def.uu.id = D_GRAPH;
    def.u.arb_d.id = D_GRAPH;
    def.u.arb_d.refconPV = kArbRefcon;
    def.u.arb_d.dephault = NewGraph(0.5f, 0.5f, 0.5f, 0, 0);
    if (!def.u.arb_d.dephault) return PF_Err_OUT_OF_MEMORY;
    if ((err = PF_ADD_PARAM(in_data, -1, &def))) return err;
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Macro 1", -1000000, 1000000, 0, 100, 0, PF_Precision_TENTHS, 0, 0, D_MACRO1);
    // the Overview's stand-in: a custom UI row whose draws are logged with the graph data's revision
    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_NO_DATA;
    PF_STRNNCPY(def.PF_DEF_NAME, "Overview", sizeof(def.PF_DEF_NAME));
    def.ui_flags = PF_PUI_CONTROL;
    def.ui_width = 200;
    def.ui_height = 40;
    def.uu.id = D_OVERVIEW;
    if ((err = PF_ADD_PARAM(in_data, -1, &def))) return err;
    // Layer Alpha (design v4 2: read at a fixed Sample Time, the point set cached)
    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Alpha Layer", PF_LayerDefault_NONE, D_ALPHA_LAYER);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Sample Time", -100000, 100000, 0, 10, 0, PF_Precision_HUNDREDTHS, 0, 0, D_SAMPLE_TIME);
    PF_CustomUIInfo ci;
    AEFX_CLR_STRUCT(ci);
    ci.events = PF_CustomEFlag_EFFECT;
    if ((err = (*in_data->inter.register_ui)(in_data->effect_ref, &ci))) return err;
    out_data->num_params = P_NUM;
    return err;
}

// Macro 1 shown with the name and slider range in the graph data (design v4 1.1: PF_UpdateParamUI is not saved, so
// it is applied again on every UPDATE_PARAMS_UI).
void ApplyMacroNames(PF_InData* in_data, PF_ParamDef* params[])
{
    GraphHeader g{};
    if (!ReadGraph(params[P_GRAPH]->u.arb_d.value, &g)) return;
    Suite<PF_ParamUtilsSuite3> pu(kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3);
    if (!pu) return;
    PF_ParamDef d = *params[P_MACRO1];
    PF_STRNNCPY(d.PF_DEF_NAME, g.macroName, sizeof(d.PF_DEF_NAME));
    d.u.fs_d.slider_max = (PF_FpShort)g.macroMax;
    const PF_Err e = pu->PF_UpdateParamUI(in_data->effect_ref, P_MACRO1, &d);
    Log("params ui: Macro 1 shown as \"%s\" 0..%g (rev %u, err %d)", g.macroName, g.macroMax, (unsigned)g.revision,
        (int)e);
}

// Layer Alpha: the chosen layer read at the Sample Time; the points (opaque pixels on an 8 px grid) are rebuilt only
// when its alpha differs (an FNV hash of the alpha channel), not when other things change.
struct AlphaCache {
    std::mutex mu;
    unsigned long long hash = 0;
    int points = -1;
} g_alpha;
void ReadLayerAlpha(PF_InData* in_data, PF_ParamDef* params[])
{
    const double st = params[P_SAMPLE_TIME]->u.fs_d.value;
    PF_ParamDef pd;
    AEFX_CLR_STRUCT(pd);
    const A_long t = (A_long)std::llround(st * in_data->time_scale);
    if (PF_CHECKOUT_PARAM(in_data, P_ALPHA_LAYER, t, in_data->time_step, in_data->time_scale, &pd)) return;
    const PF_LayerDef* w = &pd.u.ld;
    if (w->data && w->width > 0 && w->height > 0) {
        const auto t0 = std::chrono::steady_clock::now();
        unsigned long long h = 1469598103934665603ULL ^ ((unsigned long long)w->width << 32 | (unsigned)w->height);
        for (A_long y = 0; y < w->height; y++) {
            const PF_Pixel8* row = (const PF_Pixel8*)((const char*)w->data + (size_t)y * w->rowbytes);
            for (A_long x = 0; x < w->width; x++) h = (h ^ row[x].alpha) * 1099511628211ULL;
        }
        const double hashMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::lock_guard<std::mutex> lk(g_alpha.mu);
        if (h == g_alpha.hash && g_alpha.points >= 0) {
            Log("alpha: reused %d points (hash %.3f ms)", g_alpha.points, hashMs);
        } else {
            int n = 0;
            for (A_long y = 0; y < w->height; y += 8) {
                const PF_Pixel8* row = (const PF_Pixel8*)((const char*)w->data + (size_t)y * w->rowbytes);
                for (A_long x = 0; x < w->width; x += 8) n += row[x].alpha > 128;
            }
            Log("alpha: rebuilt %d points from %dx%d at %.2f s (hash %.3f ms)", n, (int)w->width, (int)w->height, st, hashMs);
            g_alpha.hash = h, g_alpha.points = n;
        }
    }
    PF_CHECKIN_PARAM(in_data, &pd);
}

PF_Err Render(PF_InData* in_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    ReadLayerAlpha(in_data, params);
    GraphHeader g{};
    if (!ReadGraph(params[P_GRAPH]->u.arb_d.value, &g)) g.r = g.g = g.b = 0.25f;
    PF_Pixel c;
    c.alpha = 255;
    c.red = (A_u_char)(g.r * 255 + 0.5f);
    c.green = (A_u_char)(g.g * 255 + 0.5f);
    c.blue = (A_u_char)(g.b * 255 + 0.5f);
    Log("render: rev %u rgb %d %d %d", (unsigned)g.revision, c.red, c.green, c.blue);
    return PF_FILL(&c, nullptr, output);
}

}  // namespace

extern "C" DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                                       PF_LayerDef* output, void* extra)
{
    PF_Err err = PF_Err_NONE;
    g_pica = in_data->pica_basicP;
    switch (cmd) {
    case PF_Cmd_GLOBAL_SETUP: {
        out_data->my_version = PF_VERSION(0, 1, 0, PF_Stage_DEVELOP, 1);
        out_data->out_flags = PF_OutFlag_SEND_UPDATE_PARAMS_UI | PF_OutFlag_CUSTOM_UI;  // must match the PiPL
        Suite<AEGP_UtilitySuite6> util(kAEGPUtilitySuite, kAEGPUtilitySuiteVersion6);
        A_Err e = util ? util->AEGP_RegisterWithAEGP(nullptr, "DupliXP0", &g_pluginId) : -1;
        Suite<AEGP_RegisterSuite5> reg(kAEGPRegisterSuite, kAEGPRegisterSuiteVersion5);
        A_Err ei = (reg && !e) ? reg->AEGP_RegisterIdleHook(g_pluginId, IdleHook, nullptr) : -1;
        Log("global setup: plugin id %d (err %d), idle hook err %d", (int)g_pluginId, (int)e, (int)ei);
        break;
    }
    case PF_Cmd_PARAMS_SETUP:
        err = ParamsSetup(in_data, out_data);
        break;
    case PF_Cmd_USER_CHANGED_PARAM: {
        const int i = ((PF_UserChangedParamExtra*)extra)->param_index;
        Log("user changed param %d", i);
        if (i == P_OPEN && params[P_OPEN]->u.bd.value) {
            OpenEditor(in_data);
            params[P_OPEN]->u.bd.value = FALSE;  // a button: back off at once
            params[P_OPEN]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
        }
        break;
    }
    case PF_Cmd_EVENT: {
        auto* ev = (PF_EventExtra*)extra;
        if (ev->e_type == PF_Event_DRAW) {
            GraphHeader g{};
            ReadGraph(params[P_GRAPH]->u.arb_d.value, &g);
            Log("overview draw: rev %u", (unsigned)g.revision);
            ev->evt_out_flags = PF_EO_HANDLED_EVENT;
        }
        break;
    }
    case PF_Cmd_SEQUENCE_SETDOWN:
        // an instance goes away (deleted, its project closed): the editor must not keep its effect reference
        // (11/28: AE crashed when the idle hook read the reference after app.open replaced the project). The spike
        // closes the editor for any instance; the product compares the instance.
        Log("sequence setdown (editor open %d)", (int)(g_wnd != nullptr));
        if (g_wnd) DestroyWindow(g_wnd);
        break;
    case PF_Cmd_UPDATE_PARAMS_UI:
        ApplyMacroNames(in_data, params);
        break;
    case PF_Cmd_ARBITRARY_CALLBACK:
        err = HandleArb((PF_ArbParamsExtra*)extra);
        break;
    case PF_Cmd_RENDER:
        err = Render(in_data, params, output);
        break;
    default:
        break;
    }
    return err;
}
