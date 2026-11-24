// randamu effect entry: parameters, pass-through render, and (later tasks) registration of the
// layer for the sub-layer hooks.
#include "randamu.h"

#include <atomic>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

#include "hook/bee_hooks.h"
#include "hook/registry.h"

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
    static bool init = false;
    static bool enabled = false;
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (!init) {
        enabled = GetEnvironmentVariableA("RANDAMU_LOG", path, sizeof(path)) > 0;
        init = true;
    }
    if (!enabled) return;
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

enum class SelfState { Ready, NoHooks, Not3D, Off, Unknown };

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
    return p;
}

// Where RegisterSelf takes the parameter values from.
enum class ParamSource {
    Array,      // the params[] AE passed with the command
    SeqData,    // the copy kept in sequence data (SEQUENCE_RESETUP: no params[] yet)
    Aegp,       // read the effect through AEGP (only while rendering: during SEQUENCE_SETUP and while AE
                // duplicates an effect, reading it fails AE's check "child not found in parent")
};

// Registers this effect's layer (key + parameter snapshot) for the hooks.
// writeSeq: store key and parameters in sequence data (only in commands that may write it).
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

    A_Time t = {in_data->current_time, (A_u_long)in_data->time_scale};
    if (t.scale == 0) t.scale = 1;
    rnd::ParamSnapshot snap;
    bool read = false;
    if (src == ParamSource::Aegp) {
        AEGP_EffectRefH eff = nullptr;
        if (!pfi->AEGP_GetNewEffectForEffect(g_pluginId, in_data->effect_ref, &eff) && eff) {
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
        RLog("register: comp %d layer %d state=%d count=%d", key.compItemId, key.layerId, (int)st, snap.p.count);
    return st;
}

const char* StatusText(SelfState st)
{
    static const char* ready = UiName("準備できています", "Ready");
    static const char* noHooks = UiName("この AE の版ではコピーを作れません", "Not supported in this AE version");
    static const char* not3d = UiName("3D レイヤーにしてください", "Make the layer 3D");
    static const char* off = UiName("エフェクトがオフです", "Effect is off");
    static const char* unknown = UiName("状態を確認できません", "Unknown");
    switch (st) {
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
                           PF_OutFlag2_I_MIX_GUID_DEPENDENCIES | PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
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

    PF_ADD_FLOAT_SLIDERX(UiName("位置の範囲 X", "Position Range X"), 0, 100000, 0, 2000, 200, PF_Precision_TENTHS, PF_ValueDisplayFlag_PIXEL, 0, RD_POS_X);
    PF_ADD_FLOAT_SLIDERX(UiName("位置の範囲 Y", "Position Range Y"), 0, 100000, 0, 2000, 200, PF_Precision_TENTHS, PF_ValueDisplayFlag_PIXEL, 0, RD_POS_Y);
    PF_ADD_FLOAT_SLIDERX(UiName("位置の範囲 Z", "Position Range Z"), 0, 100000, 0, 2000, 200, PF_Precision_TENTHS, PF_ValueDisplayFlag_PIXEL, 0, RD_POS_Z);

    PF_ADD_FLOAT_SLIDERX(UiName("回転の範囲 X", "Rotation Range X"), 0, 180, 0, 180, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, RD_ROT_X);
    PF_ADD_FLOAT_SLIDERX(UiName("回転の範囲 Y", "Rotation Range Y"), 0, 180, 0, 180, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, RD_ROT_Y);
    PF_ADD_FLOAT_SLIDERX(UiName("回転の範囲 Z", "Rotation Range Z"), 0, 180, 0, 180, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, RD_ROT_Z);

    PF_ADD_FLOAT_SLIDERX(UiName("拡大縮小の最小", "Scale Min"), 0, 10000, 0, 400, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, RD_SCALE_MIN);
    PF_ADD_FLOAT_SLIDERX(UiName("拡大縮小の最大", "Scale Max"), 0, 10000, 0, 400, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, RD_SCALE_MAX);
    PF_ADD_FLOAT_SLIDERX(UiName("不透明度の最小", "Opacity Min"), 0, 100, 0, 100, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, RD_OPACITY_MIN);
    PF_ADD_FLOAT_SLIDERX(UiName("不透明度の最大", "Opacity Max"), 0, 100, 0, 100, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, RD_OPACITY_MAX);

    PF_ADD_CHECKBOXX(UiName("1 枚目は元の位置", "Keep First at Original"), FALSE, 0, RD_KEEP_FIRST);

    out_data->num_params = RP_NUM_PARAMS;
    return PF_Err_NONE;
}

PF_Err PreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* extra)
{
    PF_Err err = PF_Err_NONE;
    RegisterSelf(in_data, out_data, ParamSource::Aegp, nullptr, false);
    if (extra->cb->GuidMixInPtr) extra->cb->GuidMixInPtr(in_data->effect_ref, sizeof(kBuildId), kBuildId);

    PF_RenderRequest req = extra->input->output_request;
    PF_CheckoutResult in_result;
    ERR(extra->cb->checkout_layer(in_data->effect_ref, RP_INPUT, RP_INPUT, &req, in_data->current_time,
                                  in_data->time_step, in_data->time_scale, &in_result));
    if (!err) {
        extra->output->result_rect = in_result.result_rect;
        extra->output->max_result_rect = in_result.max_result_rect;
    }
    return err;
}

PF_Err SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra)
{
    PF_Err err = PF_Err_NONE;
    PF_EffectWorld* inW = nullptr;
    PF_EffectWorld* outW = nullptr;
    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, RP_INPUT, &inW));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &outW));
    if (!err && inW && outW) {
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
        default:
            break;
        }
    } catch (...) {
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    if (d <= 200) RLog("dbg: cmd %d leave err=%d", (int)cmd, (int)err);
    return err;
}
