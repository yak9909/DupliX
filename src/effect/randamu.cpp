// randamu effect entry: parameters, pass-through render, and (later tasks) registration of the
// layer for the sub-layer hooks.
#include "randamu.h"

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

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

PF_Err GlobalSetup(PF_InData* in_data, PF_OutData* out_data)
{
    out_data->my_version = PF_VERSION(0, 1, 0, PF_Stage_DEVELOP, 1);
    out_data->out_flags = PF_OutFlag_PIX_INDEPENDENT | PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_SEND_UPDATE_PARAMS_UI;
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE |
                           PF_OutFlag2_I_MIX_GUID_DEPENDENCIES | PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
    RLog("GlobalSetup: %s, host %d.%d", kBuildId, in_data->version.major, in_data->version.minor);
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
    try {
        switch (cmd) {
        case PF_Cmd_GLOBAL_SETUP:
            err = GlobalSetup(in_data, out_data);
            break;
        case PF_Cmd_PARAMS_SETUP:
            err = ParamsSetup(in_data, out_data);
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
    return err;
}
