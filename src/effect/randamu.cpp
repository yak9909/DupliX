// randamu effect entry: parameters, pass-through render, and (later tasks) registration of the
// layer for the sub-layer hooks.
#include "randamu.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

#include "effect/gpu_copy.h"
#include "hook/bee_hooks.h"
#include "core/fast_plane.h"
#include "core/raster.h"
#include "hook/fast_registry.h"
#include "hook/registry.h"
#include "hook/timing.h"

#include <cmath>
#include <thread>
#include <vector>

namespace {

const char kBuildId[] = "randamu build " __DATE__ " " __TIME__;

std::mutex g_logMutex;

// AE reads parameter names in the system ANSI code page (CP932 on Japanese Windows), not UTF-8.
// Returns the Japanese name converted to CP932 on CP932 systems, the English name elsewhere.
// The returned pointer stays valid for the process lifetime (checkbox labels are kept by pointer).
const char* UiName(const char* utf8, const char* english)
{
    static std::mutex m;
    static std::deque<std::string> store;
    std::lock_guard<std::mutex> lk(m);
    if (GetACP() != 932) {
        store.emplace_back(english);
        return store.back().c_str();
    }
    wchar_t w[256] = {0};
    char a[256] = {0};
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 256) || !WideCharToMultiByte(932, 0, w, -1, a, 256, nullptr, nullptr)) {
        store.emplace_back(english);
    } else {
        store.emplace_back(a);
    }
    return store.back().c_str();
}

}  // namespace

void RLog(const char* fmt, ...)
{
    static char path[MAX_PATH] = {0};
    // Decided once (thread-safe static init); when logging is off, return without taking the lock.
    static const bool enabled = GetEnvironmentVariableA("RANDAMU_LOG", path, sizeof(path)) > 0;
    if (!enabled) return;
    std::lock_guard<std::mutex> lk(g_logMutex);
    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "a") || !fp) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(fp, "[%02d:%02d:%02d.%03d][tid %lu] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

namespace {

AEGP_PluginID g_pluginId = 0;

template <class T>
struct Suite {
    SPBasicSuite* sp;
    T* s = nullptr;
    const char* name;
    int32 ver;
    Suite(SPBasicSuite* p, const char* n, int32 v) : sp(p), name(n), ver(v)
    {
        if (sp->AcquireSuite(n, v, (const void**)&s) != kSPNoError) s = nullptr;
    }
    ~Suite()
    {
        if (s) sp->ReleaseSuite(name, ver);
    }
    T* operator->() const { return s; }
    explicit operator bool() const { return s != nullptr; }
};

// Sequence data: the key this instance registered last and its last parameter values
// (plain data, no flattening needed; saved with the project).
struct SeqData {
    int compItemId;
    int layerId;
    rnd::ScatterParams p;
    bool hasParams;
};
// Sequence data is saved in projects as raw bytes: its size must never change. ScatterParams::fast sits in
// the padding after keepFirst; old projects may hold any byte there, so the SeqData path ignores it.
static_assert(sizeof(rnd::ScatterParams) == 96, "ScatterParams layout is saved in projects");
static_assert(sizeof(SeqData) == 112, "SeqData layout is saved in projects");

enum class SelfState { Ready, NoHooks, Not3D, Off, Unknown, FastLights };

rnd::ScatterParams DefaultParams()
{
    rnd::ScatterParams p;  // same defaults as ParamsSetup
    p.count = 10;
    p.pos[0] = p.pos[1] = p.pos[2] = 200;
    return p;
}

rnd::ScatterParams ParamsFromArray(PF_ParamDef* params[])
{
    rnd::ScatterParams p;
    p.count = params[RP_COUNT]->u.sd.value;
    p.seed = params[RP_SEED]->u.sd.value;
    p.pos[0] = params[RP_POS_X]->u.fs_d.value;
    p.pos[1] = params[RP_POS_Y]->u.fs_d.value;
    p.pos[2] = params[RP_POS_Z]->u.fs_d.value;
    p.rotDeg[0] = params[RP_ROT_X]->u.fs_d.value;
    p.rotDeg[1] = params[RP_ROT_Y]->u.fs_d.value;
    p.rotDeg[2] = params[RP_ROT_Z]->u.fs_d.value;
    p.scaleMin = params[RP_SCALE_MIN]->u.fs_d.value;
    p.scaleMax = params[RP_SCALE_MAX]->u.fs_d.value;
    p.opacityMin = params[RP_OPACITY_MIN]->u.fs_d.value;
    p.opacityMax = params[RP_OPACITY_MAX]->u.fs_d.value;
    p.keepFirst = params[RP_KEEP_FIRST]->u.bd.value != 0;
    p.fast = params[RP_FAST]->u.bd.value != 0;
    return p;
}

// AEGP index of `eff` on layer L (matched by installed effect key; the first match), or -1.
// Only for render commands: reading effects through AEGP while AE adds or duplicates one fails.
int EffectIndexOnLayer(AEGP_EffectSuite4* es, AEGP_LayerH L, AEGP_EffectRefH eff)
{
    AEGP_InstalledEffectKey mine = AEGP_InstalledEffectKey_NONE;
    A_long n = 0;
    if (es->AEGP_GetInstalledKeyFromLayerEffect(eff, &mine) || es->AEGP_GetLayerNumEffects(L, &n)) return -1;
    for (A_long i = 0; i < n; i++) {
        AEGP_EffectRefH e = nullptr;
        AEGP_InstalledEffectKey k = AEGP_InstalledEffectKey_NONE;
        if (es->AEGP_GetLayerEffectByIndex(g_pluginId, L, i, &e) || !e) continue;
        bool same = !es->AEGP_GetInstalledKeyFromLayerEffect(e, &k) && k == mine;
        es->AEGP_DisposeEffect(e);
        if (same) return (int)i;
    }
    return -1;
}

// Where RegisterSelf takes the parameter values from.
enum class ParamSource {
    Array,      // the params[] AE passed with the command
    SeqData,    // the copy kept in sequence data (SEQUENCE_RESETUP: no params[] yet)
    Aegp,       // read the effect through AEGP (only while rendering: during SEQUENCE_SETUP and while AE
                // duplicates an effect, reading it fails AE's check "child not found in parent")
};

// The layer's "Accepts Lights" switch at layer time t (false if unreadable).
bool AcceptsLights(SPBasicSuite* sp, AEGP_LayerH L, const A_Time& t)
{
    Suite<AEGP_StreamSuite6> ss(sp, kAEGPStreamSuite, kAEGPStreamSuiteVersion6);
    if (!ss) return false;
    AEGP_StreamRefH s = nullptr;
    if (ss->AEGP_GetNewLayerStream(g_pluginId, L, AEGP_LayerStream_ACCEPTS_LIGHTS, &s) || !s) return false;
    AEGP_StreamValue2 v;
    std::memset(&v, 0, sizeof(v));
    bool on = false;
    if (!ss->AEGP_GetNewStreamValue(g_pluginId, s, AEGP_LTimeMode_LayerTime, &t, FALSE, &v)) {
        on = v.val.one_d != 0;
        ss->AEGP_DisposeStreamValue(&v);
    }
    ss->AEGP_DisposeStream(s);
    return on;
}

// Registers this effect's layer (key + parameter snapshot) for the hooks.
// writeSeq: store key and parameters in sequence data (only in commands that may write it).
thread_local int g_lastCompId = 0;  // comp of this thread's last registration (per-frame timing only)

SelfState RegisterSelf(PF_InData* in_data, PF_OutData* out_data, ParamSource src, PF_ParamDef* params[], bool writeSeq)
{
    SPBasicSuite* sp = in_data->pica_basicP;
    Suite<AEGP_PFInterfaceSuite1> pfi(sp, kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1);
    Suite<AEGP_LayerSuite9> ls(sp, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    Suite<AEGP_CompSuite12> cs(sp, kAEGPCompSuite, kAEGPCompSuiteVersion12);
    Suite<AEGP_ItemSuite9> is(sp, kAEGPItemSuite, kAEGPItemSuiteVersion9);
    Suite<AEGP_EffectSuite4> es(sp, kAEGPEffectSuite, kAEGPEffectSuiteVersion4);
    if (!pfi || !ls || !cs || !is || !es) return SelfState::Unknown;
    AEGP_LayerH L = nullptr;
    AEGP_CompH comp = nullptr;
    AEGP_ItemH item = nullptr;
    A_long itemId = 0;
    AEGP_LayerIDVal layerId = 0;
    if (pfi->AEGP_GetEffectLayer(in_data->effect_ref, &L) || !L || ls->AEGP_GetLayerParentComp(L, &comp) || !comp ||
        cs->AEGP_GetItemFromComp(comp, &item) || !item || is->AEGP_GetItemID(item, &itemId) ||
        ls->AEGP_GetLayerID(L, &layerId))
        return SelfState::Unknown;
    rnd::LayerKey key = {(int)itemId, (int)layerId};
    g_lastCompId = key.compItemId;
    // Fast path for render commands (PreRender comes ~2x per copy per frame): an existing valid
    // registration is enough. The hooks read the parameters at each query's time on render threads,
    // and check the 3D switch and the effect's presence/switch themselves (LayerIsLive).
    if (src == ParamSource::Aegp && rnd::EffectTypeKeyKnown()) {
        rnd::ParamSnapshot cur;
        if (rnd::RegLookup(key, &cur) && cur.valid) {
            rnd::RegTouch();  // new render: values cached by the hooks may be stale (keyframes edited)
            return SelfState::Ready;
        }
    }

    A_Time t = {in_data->current_time, (A_u_long)in_data->time_scale};
    if (t.scale == 0) t.scale = 1;
    rnd::ParamSnapshot snap;
    bool read = false;
    if (src == ParamSource::Aegp) {
        AEGP_EffectRefH eff = nullptr;
        if (!pfi->AEGP_GetNewEffectForEffect(g_pluginId, in_data->effect_ref, &eff) && eff) {
            if (!rnd::EffectTypeKeyKnown()) rnd::ResolveEffectTypeKey(L, EffectIndexOnLayer(es.s, L, eff));
            read = rnd::ReadParamsFromEffect(eff, L, t, &snap);
            es->AEGP_DisposeEffect(eff);
        }
    } else {
        snap.p = DefaultParams();
        if (src == ParamSource::Array && params) {
            snap.p = ParamsFromArray(params);
        } else if (in_data->sequence_data) {
            auto* sd = (SeqData*)PF_LOCK_HANDLE(in_data->sequence_data);
            if (sd->hasParams) snap.p = sd->p;
            snap.p.fast = false;  // may be an old project's padding byte; render threads read the real value
            PF_UNLOCK_HANDLE(in_data->sequence_data);
        }
        read = rnd::ReadLayerAnchor(L, t, &snap.anchorX, &snap.anchorY);
        snap.valid = true;
    }
    AEGP_LayerFlags lflags = 0;
    ls->AEGP_GetLayerFlags(L, &lflags);
    SelfState st = rnd::GetHookStatus() != rnd::HookStatus::Installed ? SelfState::NoHooks
                   : !(lflags & AEGP_LayerFlag_LAYER_IS_3D)            ? SelfState::Not3D
                   : (!read || !snap.valid)                            ? SelfState::Off
                                                                       : SelfState::Ready;
    if (st != SelfState::Ready) snap.valid = false;
    rnd::RegRegister(key, snap);
    // Fast mode draws without lights; AE would light the one plane as a flat board (spec 12.11).
    if (st == SelfState::Ready && snap.p.fast && src != ParamSource::Aegp && AcceptsLights(sp, L, t))
        st = SelfState::FastLights;

    if (writeSeq) {
        if (!out_data->sequence_data && in_data->sequence_data) out_data->sequence_data = in_data->sequence_data;
        if (!out_data->sequence_data) {
            out_data->sequence_data = PF_NEW_HANDLE(sizeof(SeqData));
            if (out_data->sequence_data) {
                auto* z = (SeqData*)PF_LOCK_HANDLE(out_data->sequence_data);
                std::memset(z, 0, sizeof(SeqData));
                PF_UNLOCK_HANDLE(out_data->sequence_data);
            }
        }
        if (out_data->sequence_data) {
            // A duplicated effect starts with the original's sequence data: just take the new key.
            // (Do not unregister the old key here: it belongs to the original instance.)
            auto* sd = (SeqData*)PF_LOCK_HANDLE(out_data->sequence_data);
            sd->compItemId = key.compItemId;
            sd->layerId = key.layerId;
            if (src != ParamSource::SeqData) {
                sd->p = snap.p;
                sd->hasParams = true;
            }
            PF_UNLOCK_HANDLE(out_data->sequence_data);
        }
    }
    static std::atomic<int> logged{0};
    if (++logged <= 40)
        RLog("register: comp %d layer %d state=%d count=%d hasfx=%d", key.compItemId, key.layerId, (int)st,
             snap.p.count, (int)rnd::LayerHasThisEffect(L));
    return st;
}

const char* StatusText(SelfState st)
{
    static const char* ready = UiName("準備できています", "Ready");
    static const char* noHooks = UiName("この AE の版ではコピーを作れません", "Not supported in this AE version");
    static const char* not3d = UiName("3D レイヤーにしてください", "Make the layer 3D");
    static const char* off = UiName("エフェクトがオフです", "Effect is off");
    static const char* unknown = UiName("状態を確認できません", "Unknown");
    static const char* fastLights = UiName("高速モードではライトは使えません。『ライトを受ける』をオフにしてください",
                                           "Fast Mode ignores lights. Turn off Accepts Lights");
    switch (st) {
    case SelfState::FastLights: return fastLights;
    case SelfState::Ready: return ready;
    case SelfState::NoHooks: return noHooks;
    case SelfState::Not3D: return not3d;
    case SelfState::Off: return off;
    default: return unknown;
    }
}

PF_Err UpdateParamsUi(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[])
{
    SelfState st = RegisterSelf(in_data, out_data, ParamSource::Array, params, false);
    PF_ParamDef copy = *params[RP_STATUS];
    copy.ui_flags |= PF_PUI_DISABLED;
    copy.u.bd.u.nameptr = StatusText(st);
    PF_ParamUtilsSuite3* pu = nullptr;
    if (in_data->pica_basicP->AcquireSuite(kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3, (const void**)&pu) ==
            kSPNoError &&
        pu) {
        pu->PF_UpdateParamUI(in_data->effect_ref, RP_STATUS, &copy);
        in_data->pica_basicP->ReleaseSuite(kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3);
    }
    return PF_Err_NONE;
}

PF_Err SequenceSetdown(PF_InData* in_data, PF_OutData* out_data)
{
    if (in_data->sequence_data) {
        auto* sd = (SeqData*)PF_LOCK_HANDLE(in_data->sequence_data);
        if (sd->compItemId || sd->layerId) {
            rnd::RegUnregister({sd->compItemId, sd->layerId});
            RLog("unregister: comp %d layer %d", sd->compItemId, sd->layerId);
        }
        PF_UNLOCK_HANDLE(in_data->sequence_data);
        PF_DISPOSE_HANDLE(in_data->sequence_data);
        out_data->sequence_data = nullptr;
    }
    return PF_Err_NONE;
}

PF_Err GlobalSetup(PF_InData* in_data, PF_OutData* out_data)
{
    out_data->my_version = PF_VERSION(0, 1, 0, PF_Stage_DEVELOP, 1);
    out_data->out_flags = PF_OutFlag_PIX_INDEPENDENT | PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_SEND_UPDATE_PARAMS_UI;
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE |
                           PF_OutFlag2_I_MIX_GUID_DEPENDENCIES | PF_OutFlag2_SUPPORTS_THREADED_RENDERING |
                           PF_OutFlag2_SUPPORTS_GPU_RENDER_F32 | PF_OutFlag2_SUPPORTS_DIRECTX_RENDERING;
    RLog("GlobalSetup: %s, host %d.%d", kBuildId, in_data->version.major, in_data->version.minor);
    Suite<AEGP_UtilitySuite6> util(in_data->pica_basicP, kAEGPUtilitySuite, kAEGPUtilitySuiteVersion6);
    if (util && !util->AEGP_RegisterWithAEGP(nullptr, "randamu", &g_pluginId))
        rnd::InstallHooks(in_data->pica_basicP, g_pluginId);
    else
        RLog("GlobalSetup: AEGP_RegisterWithAEGP failed; hooks not installed");
    return PF_Err_NONE;
}

PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data)
{
    PF_ParamDef def;

    // Status line: a checkbox the user cannot operate; its label text shows the state.
    AEFX_CLR_STRUCT(def);
    def.ui_flags = PF_PUI_DISABLED;
    PF_ADD_CHECKBOX(UiName("状態", "Status"), UiName("準備できています", "Ready"), FALSE, PF_ParamFlag_CANNOT_TIME_VARY, RD_STATUS);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER(UiName("コピー数", "Copies"), 0, 100000, 1, 1000, 10, RD_COUNT);
    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER(UiName("乱数の種", "Random Seed"), 0, 1000000, 0, 1000, 0, RD_SEED);

    PF_ADD_FLOAT_SLIDERX(UiName("位置の範囲 X", "Position Range X"), 0, 100000, 0, 2000, 200, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, RD_POS_X);
    PF_ADD_FLOAT_SLIDERX(UiName("位置の範囲 Y", "Position Range Y"), 0, 100000, 0, 2000, 200, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, RD_POS_Y);
    PF_ADD_FLOAT_SLIDERX(UiName("位置の範囲 Z", "Position Range Z"), 0, 100000, 0, 2000, 200, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, RD_POS_Z);

    PF_ADD_FLOAT_SLIDERX(UiName("回転の範囲 X", "Rotation Range X"), 0, 180, 0, 180, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, RD_ROT_X);
    PF_ADD_FLOAT_SLIDERX(UiName("回転の範囲 Y", "Rotation Range Y"), 0, 180, 0, 180, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, RD_ROT_Y);
    PF_ADD_FLOAT_SLIDERX(UiName("回転の範囲 Z", "Rotation Range Z"), 0, 180, 0, 180, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, RD_ROT_Z);

    PF_ADD_FLOAT_SLIDERX(UiName("拡大縮小の最小", "Scale Min"), 0, 10000, 0, 400, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, RD_SCALE_MIN);
    PF_ADD_FLOAT_SLIDERX(UiName("拡大縮小の最大", "Scale Max"), 0, 10000, 0, 400, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, RD_SCALE_MAX);
    PF_ADD_FLOAT_SLIDERX(UiName("不透明度の最小", "Opacity Min"), 0, 100, 0, 100, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, RD_OPACITY_MIN);
    PF_ADD_FLOAT_SLIDERX(UiName("不透明度の最大", "Opacity Max"), 0, 100, 0, 100, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, RD_OPACITY_MAX);

    PF_ADD_CHECKBOXX(UiName("1 枚目は元の位置", "Keep First at Original"), FALSE, 0, RD_KEEP_FIRST);
    PF_ADD_CHECKBOXX(UiName("高速モード", "Fast Mode"), FALSE, 0, RD_FAST);

    out_data->num_params = RP_NUM_PARAMS;
    return PF_Err_NONE;
}

// ---- fast mode (spec 12.11): all copies drawn here into one screen-sized image, shown by the hooks as
// one plane facing the camera that maps the effect's output px (x, y) to comp px (x, y).

// What SmartRender needs, worked out in PreRender (pre_render_data).
struct FastPre {
    rnd::CameraView cam;
    double W[16];             // layer -> world
    rnd::ScatterParams p;
    double anchorX, anchorY;  // layer px
    double dsX, dsY;
    PF_LRect inRect;          // the input's result rect (downsampled layer px)
};
void DeleteFastPre(void* p) { delete (FastPre*)p; }

// The parameter values at the effect's current time (smart render: params[] is not passed).
bool CheckoutScatterParams(PF_InData* in_data, rnd::ScatterParams* out)
{
    PF_ParamDef defs[RP_NUM_PARAMS];
    PF_ParamDef* ptrs[RP_NUM_PARAMS] = {};
    bool ok = true;
    int last = RP_COUNT - 1;
    for (int i = RP_COUNT; i < RP_NUM_PARAMS && ok; i++) {
        AEFX_CLR_STRUCT(defs[i]);
        ok = PF_CHECKOUT_PARAM(in_data, i, in_data->current_time, in_data->time_step, in_data->time_scale,
                               &defs[i]) == PF_Err_NONE;
        if (ok) last = i, ptrs[i] = &defs[i];
    }
    if (ok) *out = ParamsFromArray(ptrs);
    for (int i = RP_COUNT; i <= last; i++) PF_CHECKIN_PARAM(in_data, &defs[i]);
    return ok;
}

bool FastModeOn(PF_InData* in_data)
{
    PF_ParamDef d;
    AEFX_CLR_STRUCT(d);
    if (PF_CHECKOUT_PARAM(in_data, RP_FAST, in_data->current_time, in_data->time_step, in_data->time_scale, &d))
        return false;
    bool on = d.u.bd.value != 0;
    PF_CHECKIN_PARAM(in_data, &d);
    return on;
}

// Camera, layer transform, comp size and anchor at the effect's current time; registers the plane.
bool FastSetup(PF_InData* in_data, FastPre* f, int* compW, int* compH)
{
    SPBasicSuite* sp = in_data->pica_basicP;
    Suite<AEGP_PFInterfaceSuite1> pfi(sp, kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1);
    Suite<AEGP_LayerSuite9> ls(sp, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    Suite<AEGP_CompSuite12> cs(sp, kAEGPCompSuite, kAEGPCompSuiteVersion12);
    Suite<AEGP_ItemSuite9> is(sp, kAEGPItemSuite, kAEGPItemSuiteVersion9);
    if (!pfi || !ls || !cs || !is) return false;
    A_Time ct = {0, 1};  // comp time: what the camera and layer-to-world reads take
    AEGP_LayerH L = nullptr;
    AEGP_CompH comp = nullptr;
    AEGP_ItemH item = nullptr;
    A_long itemId = 0;
    AEGP_LayerIDVal layerId = 0;
    if (pfi->AEGP_ConvertEffectToCompTime(in_data->effect_ref, in_data->current_time, in_data->time_scale, &ct) ||
        pfi->AEGP_GetEffectLayer(in_data->effect_ref, &L) || ls->AEGP_GetLayerParentComp(L, &comp) ||
        cs->AEGP_GetItemFromComp(comp, &item) || is->AEGP_GetItemID(item, &itemId) || ls->AEGP_GetLayerID(L, &layerId))
        return false;
    A_long w = 0, h = 0;
    A_Matrix4 cam, l2w;
    A_FpLong dist = 0;
    A_short ipw = 0, iph = 0;
    if (is->AEGP_GetItemDimensions(item, &w, &h) || w <= 0 || h <= 0 ||
        pfi->AEGP_GetEffectCameraMatrix(in_data->effect_ref, &ct, &cam, &dist, &ipw, &iph) || !(dist > 0) ||
        ls->AEGP_GetLayerToWorldXform(L, &ct, &l2w))
        return false;
    A_Time lt = {in_data->current_time, (A_u_long)(in_data->time_scale ? in_data->time_scale : 1)};
    if (!rnd::ReadLayerAnchor(L, lt, &f->anchorX, &f->anchorY)) return false;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            f->cam.camToWorld[r * 4 + c] = cam.mat[r][c];
            f->W[r * 4 + c] = l2w.mat[r][c];
        }
    f->cam.dist = dist;
    f->cam.compW = (double)w;
    f->cam.compH = (double)h;
    // The plane's depth is the anchor's (spec 12.11). At or behind the camera any positive depth shows the
    // same 1:1 picture, so keep it just in front of the camera.
    double depth = std::max(rnd::AnchorDepth(f->cam, f->W, f->anchorX, f->anchorY), 2.0);
    double m4[16];
    if (!rnd::PlaneM4(f->cam, f->W, depth, m4)) return false;
    rnd::FastSetPlane({(int)itemId, (int)layerId}, in_data->current_time, lt.scale, m4);
    *compW = (int)w;
    *compH = (int)h;
    return true;
}

PF_Err FastPreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* extra)
{
    RegisterSelf(in_data, out_data, ParamSource::Aegp, nullptr, false);
    auto* f = new FastPre();
    f->dsX = in_data->downsample_x.den ? (double)in_data->downsample_x.num / in_data->downsample_x.den : 1.0;
    f->dsY = in_data->downsample_y.den ? (double)in_data->downsample_y.num / in_data->downsample_y.den : 1.0;
    int cw = 0, ch = 0;
    bool ok = CheckoutScatterParams(in_data, &f->p) && FastSetup(in_data, f, &cw, &ch);
    // The whole source is needed whatever part of the screen is asked for.
    PF_RenderRequest req = extra->input->output_request;
    req.rect.left = req.rect.top = -30000;
    req.rect.right = req.rect.bottom = 30000;
    PF_CheckoutResult in_result;
    AEFX_CLR_STRUCT(in_result);
    PF_Err err = extra->cb->checkout_layer(in_data->effect_ref, RP_INPUT, RP_INPUT, &req, in_data->current_time,
                                           in_data->time_step, in_data->time_scale, &in_result);
    f->inRect = in_result.result_rect;
    // The output depends on the camera, the layer's transform, the comp size, the time and the downsample:
    // mix them all into the cache key (required with I_MIX_GUID_DEPENDENCIES in any case).
    double key[64] = {0};
    int n = 0;
    for (int i = 0; i < 16; i++) key[n++] = f->cam.camToWorld[i];
    for (int i = 0; i < 16; i++) key[n++] = f->W[i];
    const double more[] = {f->cam.dist, f->cam.compW, f->cam.compH, f->dsX, f->dsY, (double)in_data->current_time,
                           (double)in_data->time_scale, f->anchorX, f->anchorY, ok ? 1.0 : 0.0};
    for (double v : more) key[n++] = v;
    if (extra->cb->GuidMixInPtr) {
        extra->cb->GuidMixInPtr(in_data->effect_ref, sizeof(kBuildId), kBuildId);
        extra->cb->GuidMixInPtr(in_data->effect_ref, sizeof(double) * n, key);
    }
    if (err) {
        delete f;
        return err;
    }
    if (ok) {
        PF_LRect full = {0, 0, (A_long)std::ceil(cw * f->dsX), (A_long)std::ceil(ch * f->dsY)};  // l, t, r, b
        const PF_LRect& q = extra->input->output_request.rect;
        PF_LRect x = {std::max(q.left, full.left), std::max(q.top, full.top), std::min(q.right, full.right),
                      std::min(q.bottom, full.bottom)};
        if (x.right < x.left) x.right = x.left;
        if (x.bottom < x.top) x.bottom = x.top;
        extra->output->result_rect = x;
        extra->output->max_result_rect = full;
        extra->output->pre_render_data = f;
        extra->output->delete_pre_render_data_func = DeleteFastPre;
    } else {
        // Nothing to show (camera or transform unreadable): an empty output, so no stale plane shows.
        delete f;
        PF_LRect none = {0, 0, 0, 0};
        extra->output->result_rect = none;
        extra->output->max_result_rect = none;
    }
    static std::atomic<int> logged{0};
    if (++logged <= 6)
        RLog("fast: prerender ok=%d comp %dx%d ds %.3f/%.3f in=(%d,%d,%d,%d) out=(%d,%d,%d,%d) t=%d/%d", (int)ok, cw,
             ch, ok ? f->dsX : 0.0, ok ? f->dsY : 0.0, (int)in_result.result_rect.left, (int)in_result.result_rect.top,
             (int)in_result.result_rect.right, (int)in_result.result_rect.bottom, (int)extra->output->result_rect.left,
             (int)extra->output->result_rect.top, (int)extra->output->result_rect.right,
             (int)extra->output->result_rect.bottom, (int)in_data->current_time, (int)in_data->time_scale);
    return PF_Err_NONE;
}

int WorldBpc(PF_InData* in_data, PF_EffectWorld* w)
{
    PF_WorldSuite2* ws = nullptr;
    PF_PixelFormat fmt = PF_PixelFormat_INVALID;
    if (in_data->pica_basicP->AcquireSuite(kPFWorldSuite, kPFWorldSuiteVersion2, (const void**)&ws) != kSPNoError || !ws)
        return 0;
    ws->PF_GetPixelFormat(w, &fmt);
    in_data->pica_basicP->ReleaseSuite(kPFWorldSuite, kPFWorldSuiteVersion2);
    switch (fmt) {
    case PF_PixelFormat_ARGB32: return 8;
    case PF_PixelFormat_ARGB64: return 16;
    case PF_PixelFormat_ARGB128: return 32;
    default: return 0;
    }
}

PF_Err FastSmartRender(PF_InData* in_data, const FastPre* f, PF_EffectWorld* inW, PF_EffectWorld* outW)
{
    int bpc = WorldBpc(in_data, outW);
    if (!bpc) return PF_Err_BAD_CALLBACK_PARAM;
    rnd::RasterImage dst = {outW->data, (int)outW->width, (int)outW->height, (long)outW->rowbytes, bpc,
                            (int)outW->origin_x, (int)outW->origin_y};
    rnd::RasterImage src = {nullptr, 0, 0, 0, bpc, 0, 0};
    std::vector<rnd::RasterCopy> copies;
    if (f && inW && inW->data && WorldBpc(in_data, inW) == bpc) {
        src = {inW->data, (int)inW->width, (int)inW->height, (long)inW->rowbytes, bpc, (int)f->inRect.left,
               (int)f->inRect.top};
        // the source in full-resolution layer px
        const double srcRect[4] = {f->inRect.left / f->dsX, f->inRect.top / f->dsY,
                                   (f->inRect.left + inW->width) / f->dsX, (f->inRect.top + inW->height) / f->dsY};
        const double cx = (srcRect[0] + srcRect[2]) / 2, cy = (srcRect[1] + srcRect[3]) / 2;
        copies.reserve((size_t)std::max(f->p.count, 0));
        for (int i = 0; i < f->p.count; i++) {
            rnd::CopyXform x;
            rnd::ComputeCopy(f->p, i, f->anchorX, f->anchorY, &x);
            rnd::RasterCopy rc;
            if (x.opacity <= 0 || !rnd::CopyHomography(f->cam, f->W, x.m4, srcRect, rc.screenToSrc)) continue;
            rc.opacity = x.opacity;
            rc.depth = rnd::CopyDepth(f->cam, f->W, x.m4, cx, cy);
            copies.push_back(rc);
        }
        static std::atomic<int> logged{0};
        if (++logged <= 3)
            RLog("fast: render bpc %d in %dx%d origin(%d,%d) inRect(%d,%d) out %dx%d origin(%d,%d) copies %zu/%d", bpc,
                 (int)inW->width, (int)inW->height, (int)inW->origin_x, (int)inW->origin_y, (int)f->inRect.left,
                 (int)f->inRect.top, (int)outW->width, (int)outW->height, (int)outW->origin_x, (int)outW->origin_y,
                 copies.size(), f->p.count);
    }
    unsigned hw = std::thread::hardware_concurrency();
    rnd::RasterizeCopies(src, copies.data(), (int)copies.size(), f ? f->dsX : 1.0, f ? f->dsY : 1.0, dst,
                         hw ? (int)hw : 4);
    return PF_Err_NONE;
}

PF_Err PreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* extra)
{
    if (FastModeOn(in_data)) return FastPreRender(in_data, out_data, extra);
    PF_Err err = PF_Err_NONE;
    long long tStart = rnd::TimingNow();
    struct MarkAtExit {
        PF_InData* in;
        long long t0;
        ~MarkAtExit()
        {
            if (rnd::TimingEnabled() && in->time_scale)
                rnd::TimingMark(g_lastCompId, (double)in->current_time / in->time_scale, rnd::TK_PRERENDER, t0,
                                rnd::TimingNow());
        }
    } mark{in_data, tStart};
    {
        static std::atomic<long long> ticks{0}, calls{0};
        LARGE_INTEGER t0, t1, f;
        QueryPerformanceCounter(&t0);
        RegisterSelf(in_data, out_data, ParamSource::Aegp, nullptr, false);
        QueryPerformanceCounter(&t1);
        ticks += t1.QuadPart - t0.QuadPart;
        long long c = ++calls;
        if ((c & (c - 1)) == 0 && c >= 16) {
            QueryPerformanceFrequency(&f);
            RLog("prof: PreRender register %lld calls %.1fms", c, ticks.load() * 1000.0 / f.QuadPart);
        }
    }
    if (extra->cb->GuidMixInPtr) extra->cb->GuidMixInPtr(in_data->effect_ref, sizeof(kBuildId), kBuildId);

    PF_RenderRequest req = extra->input->output_request;
    PF_CheckoutResult in_result;
    ERR(extra->cb->checkout_layer(in_data->effect_ref, RP_INPUT, RP_INPUT, &req, in_data->current_time,
                                  in_data->time_step, in_data->time_scale, &in_result));
    if (!err) {
        extra->output->result_rect = in_result.result_rect;
        extra->output->max_result_rect = in_result.max_result_rect;
        extra->output->flags |= PF_RenderOutputFlag_GPU_RENDER_POSSIBLE;
    }
    return err;
}

PF_Err GpuDeviceSetup(PF_InData* in_data, PF_OutData* out_data, PF_GPUDeviceSetupExtra* extra)
{
    PF_GPUDeviceInfo info;
    AEFX_CLR_STRUCT(info);
    Suite<PF_GPUDeviceSuite1> gs(in_data->pica_basicP, kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1);
    if (!gs || gs->GetDeviceInfo(in_data->effect_ref, extra->input->device_index, &info)) return PF_Err_NONE;
    void* data = nullptr;
    bool ok = rnd::GpuDeviceSetup(extra->input->what_gpu, info, &data);
    extra->output->gpu_data = data;
    if (ok) out_data->out_flags2 = PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;
    RLog("gpu: device %u framework %d -> %s", (unsigned)extra->input->device_index, (int)extra->input->what_gpu,
         ok ? "supported" : "not supported (CPU)");
    return PF_Err_NONE;
}

PF_Err GpuDeviceSetdown(PF_GPUDeviceSetdownExtra* extra)
{
    rnd::GpuDeviceSetdown(extra->input->what_gpu, extra->input->gpu_data);
    return PF_Err_NONE;
}

// Per-frame timing of the effect's own render (comp = this thread's last registration, close enough).
struct RenderMark {
    PF_InData* in;
    long long t0 = rnd::TimingNow();
    ~RenderMark()
    {
        if (rnd::TimingEnabled() && in->time_scale)
            rnd::TimingMark(g_lastCompId, (double)in->current_time / in->time_scale, rnd::TK_RENDER, t0,
                            rnd::TimingNow());
    }
};

PF_Err SmartRenderGpu(PF_InData* in_data, PF_OutData*, PF_SmartRenderExtra* extra)
{
    RenderMark mark{in_data};
    PF_Err err = PF_Err_NONE;
    PF_EffectWorld* inW = nullptr;
    PF_EffectWorld* outW = nullptr;
    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, RP_INPUT, &inW));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &outW));
    if (!err && inW && outW) {
        Suite<PF_GPUDeviceSuite1> gs(in_data->pica_basicP, kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1);
        PF_GPUDeviceInfo info;
        AEFX_CLR_STRUCT(info);
        void* src = nullptr;
        void* dst = nullptr;
        if (!gs || gs->GetDeviceInfo(in_data->effect_ref, extra->input->device_index, &info) ||
            gs->GetGPUWorldData(in_data->effect_ref, inW, &src) || gs->GetGPUWorldData(in_data->effect_ref, outW, &dst) ||
            !rnd::GpuCopy(extra->input->what_gpu, info, extra->input->gpu_data, src, dst, inW->rowbytes, outW->rowbytes,
                          std::min(inW->height, outW->height)))
            err = PF_Err_INTERNAL_STRUCT_DAMAGED;
        static std::atomic<int> logged{0};
        if (++logged <= 3)
            RLog("gpu: render framework %d %dx%d rowbytes %d/%d err=%d", (int)extra->input->what_gpu, (int)inW->width,
                 (int)inW->height, (int)inW->rowbytes, (int)outW->rowbytes, (int)err);
    }
    if (inW) extra->cb->checkin_layer_pixels(in_data->effect_ref, RP_INPUT);
    return err;
}

PF_Err SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra)
{
    RenderMark mark{in_data};
    PF_Err err = PF_Err_NONE;
    PF_EffectWorld* inW = nullptr;
    PF_EffectWorld* outW = nullptr;
    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, RP_INPUT, &inW));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &outW));
    if (!err && outW && FastModeOn(in_data)) {
        err = FastSmartRender(in_data, (const FastPre*)extra->input->pre_render_data, inW, outW);
    } else if (!err && inW && outW) {
        PF_WorldTransformSuite1* wts = nullptr;
        if (in_data->pica_basicP->AcquireSuite(kPFWorldTransformSuite, kPFWorldTransformSuiteVersion1,
                                               (const void**)&wts) == kSPNoError &&
            wts) {
            err = wts->copy(in_data->effect_ref, inW, outW, nullptr, nullptr);
            in_data->pica_basicP->ReleaseSuite(kPFWorldTransformSuite, kPFWorldTransformSuiteVersion1);
        }
    }
    if (inW) extra->cb->checkin_layer_pixels(in_data->effect_ref, RP_INPUT);
    return err;
}

}  // namespace

extern "C" DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                                       PF_LayerDef* output, void* extra)
{
    PF_Err err = PF_Err_NONE;
    static std::atomic<int> dbg{0};
    int d = ++dbg;
    if (d <= 200) RLog("dbg: cmd %d enter", (int)cmd);
    try {
        switch (cmd) {
        case PF_Cmd_GLOBAL_SETUP:
            err = GlobalSetup(in_data, out_data);
            break;
        case PF_Cmd_PARAMS_SETUP:
            err = ParamsSetup(in_data, out_data);
            break;
        case PF_Cmd_SEQUENCE_SETUP:
            // Reading the effect through AEGP while AE is still adding it fails AE's internal check
            // ("child not found in parent") and the effect is not added. SEQUENCE_RESETUP follows at once.
            break;
        case PF_Cmd_SEQUENCE_RESETUP:
            if (rnd::IsMainThread()) RegisterSelf(in_data, out_data, ParamSource::SeqData, nullptr, true);
            break;
        case PF_Cmd_USER_CHANGED_PARAM:
            if (rnd::IsMainThread()) RegisterSelf(in_data, out_data, ParamSource::Array, params, true);
            break;
        case PF_Cmd_SEQUENCE_SETDOWN:
            err = SequenceSetdown(in_data, out_data);
            break;
        case PF_Cmd_UPDATE_PARAMS_UI:
            err = UpdateParamsUi(in_data, out_data, params);
            break;
        case PF_Cmd_SMART_PRE_RENDER:
            err = PreRender(in_data, out_data, (PF_PreRenderExtra*)extra);
            break;
        case PF_Cmd_SMART_RENDER:
            err = SmartRender(in_data, out_data, (PF_SmartRenderExtra*)extra);
            break;
        case PF_Cmd_SMART_RENDER_GPU:
            err = SmartRenderGpu(in_data, out_data, (PF_SmartRenderExtra*)extra);
            break;
        case PF_Cmd_GPU_DEVICE_SETUP:
            err = GpuDeviceSetup(in_data, out_data, (PF_GPUDeviceSetupExtra*)extra);
            break;
        case PF_Cmd_GPU_DEVICE_SETDOWN:
            err = GpuDeviceSetdown((PF_GPUDeviceSetdownExtra*)extra);
            break;
        default:
            break;
        }
    } catch (...) {
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    if (d <= 200) RLog("dbg: cmd %d leave err=%d", (int)cmd, (int)err);
    return err;
}
