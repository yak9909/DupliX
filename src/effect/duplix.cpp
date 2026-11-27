// DupliX effect entry: parameters, pass-through render, and (later tasks) registration of the
// layer for the sub-layer hooks.
#include "duplix.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

#include "effect/gpu_copy.h"
#include "gpu/gpu_raster.h"
#include "hook/bee_hooks.h"
#include "core/fast_plane.h"
#include "core/raster.h"
#include "hook/merge_scene.h"
#include "hook/registry.h"
#include "hook/timing.h"
#include "effect/transfer.h"

#include <map>
#include <cmath>
#include <thread>
#include <vector>

namespace {

const char kBuildId[] = "DupliX build " __DATE__ " " __TIME__;

// The identity of THIS build for AE's disk cache: the loaded .aex file's last-write time and size.
// __DATE__/__TIME__ alone only change when this .cpp is recompiled, so a fix in another file (raster.cpp,
// 11/24 23:00) kept the old key and AE served frames of the previous build from its disk cache.
const std::string& BuildId()
{
    static const std::string id = [] {
        std::string s = kBuildId;
        HMODULE self = nullptr;
        wchar_t path[MAX_PATH] = {0};
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)&BuildId, &self) &&
            GetModuleFileNameW(self, path, MAX_PATH) && GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
            char b[96];
            snprintf(b, sizeof(b), " file %08lx%08lx %lu", fa.ftLastWriteTime.dwHighDateTime,
                     fa.ftLastWriteTime.dwLowDateTime, fa.nFileSizeLow);
            s += b;
        }
        return s;
    }();
    return id;
}

std::mutex g_logMutex;

}  // namespace

namespace {
std::atomic<int> g_logGen{0};  // bumped when DUPLIX_LOG changes (a test switching logs between comps)
}

int RLogGeneration() { return g_logGen.load(std::memory_order_relaxed); }

bool LogLimit::Take(int limit, bool powersOfTwo, int* countOut)
{
    int g = RLogGeneration();
    if (gen.load(std::memory_order_relaxed) != g) {
        gen.store(g, std::memory_order_relaxed);
        n.store(0, std::memory_order_relaxed);
    }
    int k = ++n;
    if (countOut) *countOut = k;
    return k <= limit || (powersOfTwo && (k & (k - 1)) == 0);
}

void RLog(const char* fmt, ...)
{
    // Logging on or off is decided once (thread-safe static init); when off, return without taking the lock.
    static const bool enabled = GetEnvironmentVariableA("DUPLIX_LOG", nullptr, 0) > 0;
    if (!enabled) return;
    std::lock_guard<std::mutex> lk(g_logMutex);
    // The path is read on every line: a test script running in AE ($.setenv) gives each comp its own log.
    static char path[MAX_PATH] = {0};
    char now[MAX_PATH] = {0};
    if (GetEnvironmentVariableA("DUPLIX_LOG", now, sizeof(now)) > 0 && std::strcmp(now, path)) {
        if (path[0]) ++g_logGen;
        std::memcpy(path, now, sizeof(path));
    }
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
// Since the World Transform (11/25): SeqData followed by the world values. Projects saved before hold the
// 112-byte handle only: read the world part only when the handle is this large, grow it when writing.
struct SeqDataV2 {
    SeqData base;
    rnd::WorldXform world;
    int hasWorld;
    int reserved;
};
static_assert(sizeof(rnd::WorldXform) == 72, "WorldXform layout is saved in projects");
static_assert(sizeof(SeqDataV2) == 192, "SeqDataV2 layout is saved in projects");

enum class SelfState { Ready, NoHooks, Not3D, Off, Unknown, Need2D };

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
    p.fast = IsFastMode(params[RP_RENDER_MODE]->u.pd.value);
    p.merge = params[RP_MERGE]->u.bd.value != 0;
    return p;
}

rnd::WorldXform WorldFromArray(PF_ParamDef* params[])
{
    rnd::WorldXform w;
    for (int k = 0; k < 3; k++) {
        w.pos[k] = params[RP_WORLD_POS_X + k]->u.fs_d.value;
        w.rotDeg[k] = FIX_2_FLOAT(params[RP_WORLD_ROT_X + k]->u.ad.value);
        w.scale[k] = params[RP_WORLD_SCALE_X + k]->u.fs_d.value;
    }
    return w;
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

// Whether the values the hooks read for this layer at this render's time may differ from what they read before:
// a fingerprint of every parameter (PF_CHECKOUT_PARAM: the values AE evaluated for this render) and of the layer's
// anchor point, compared with the last one seen for the same layer and time. AE pre-renders the effect once or
// twice per copy and frame with the same values; advancing the hooks' cache generation on each of them made the
// hooks read the parameters through AEGP for every copy (~0.5 ms each). An edit (keyframe, expression, script,
// effect controls) shows up as a new fingerprint at that time. Unknown (or evicted) time: changed.
bool ParamsChangedAt(PF_InData* in_data, AEGP_LayerH L, rnd::LayerKey key)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        for (size_t i = 0; i < n; i++) h = (h ^ ((const unsigned char*)p)[i]) * 1099511628211ull;
    };
    for (int i = RP_COUNT; i < RP_NUM_PARAMS; i++) {
        PF_ParamDef d;
        AEFX_CLR_STRUCT(d);
        if (PF_CHECKOUT_PARAM(in_data, i, in_data->current_time, in_data->time_step, in_data->time_scale, &d))
            return true;
        mix(&d.param_type, sizeof(d.param_type));
        mix(&d.u, sizeof(d.u));
        PF_CHECKIN_PARAM(in_data, &d);
    }
    A_Time t = {in_data->current_time, (A_u_long)in_data->time_scale};
    if (t.scale == 0) t.scale = 1;
    double ax = 0, ay = 0;
    if (!rnd::ReadLayerAnchor(L, t, &ax, &ay)) return true;
    mix(&ax, sizeof(ax)), mix(&ay, sizeof(ay));
    struct Seen {
        rnd::LayerKey k;
        A_long tv;
        A_u_long ts;
        uint64_t h;
        bool used;
    };
    static std::mutex mu;
    static Seen seen[256] = {};
    static int next = 0;
    std::lock_guard<std::mutex> lk(mu);
    for (Seen& s : seen)
        if (s.used && s.k.compItemId == key.compItemId && s.k.layerId == key.layerId && s.tv == t.value && s.ts == t.scale) {
            const bool changed = s.h != h;
            s.h = h;
            return changed;
        }
    seen[next] = {key, t.value, t.scale, h, true};
    next = (next + 1) % 256;
    return true;
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
thread_local int g_lastCompId = 0;  // comp of this thread's last registration (per-frame timing only)
thread_local rnd::LayerKey g_lastKey = {0, 0};  // this thread's last registration

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
    g_lastKey = key;
    // Fast path for render commands (PreRender comes ~2x per copy per frame): an existing valid
    // registration is enough. The hooks read the parameters at each query's time on render threads,
    // and check the 3D switch and the effect's presence/switch themselves (LayerIsLive).
    if (src == ParamSource::Aegp && rnd::EffectTypeKeyKnown()) {
        rnd::ParamSnapshot cur;
        if (rnd::RegLookup(key, &cur) && cur.valid) {
            // values cached by the hooks may be stale (an edit at this time): only then a new generation
            if (ParamsChangedAt(in_data, L, key)) rnd::RegTouch();
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
            snap.world = WorldFromArray(params);
        } else if (in_data->sequence_data) {
            const bool v2 = PF_GET_HANDLE_SIZE(in_data->sequence_data) >= sizeof(SeqDataV2);
            auto* sd = (SeqDataV2*)PF_LOCK_HANDLE(in_data->sequence_data);
            if (sd->base.hasParams) snap.p = sd->base.p;
            snap.p.fast = false;  // may be an old project's padding byte; render threads read the real value
            snap.p.merge = false;  // the same padding
            if (v2 && sd->hasWorld) snap.world = sd->world;
            PF_UNLOCK_HANDLE(in_data->sequence_data);
        }
        read = rnd::ReadLayerAnchor(L, t, &snap.anchorX, &snap.anchorY);
        snap.valid = true;
    }
    AEGP_LayerFlags lflags = 0;
    ls->AEGP_GetLayerFlags(L, &lflags);
    const bool is3D = (lflags & AEGP_LayerFlag_LAYER_IS_3D) != 0;
    SelfState st;
    if (snap.p.fast) {
        // Fast mode is an ordinary plugin on a 2D layer: no hooks needed, and the hooks leave the layer alone.
        st = is3D ? SelfState::Need2D : SelfState::Ready;
        snap.valid = false;
    } else {
        st = rnd::GetHookStatus() != rnd::HookStatus::Installed ? SelfState::NoHooks
             : !is3D                                             ? SelfState::Not3D
             : (!read || !snap.valid)                            ? SelfState::Off
                                                                 : SelfState::Ready;
        if (st != SelfState::Ready) snap.valid = false;
    }
    rnd::RegRegister(key, snap);

    if (writeSeq) {
        if (!out_data->sequence_data && in_data->sequence_data) out_data->sequence_data = in_data->sequence_data;
        if (!out_data->sequence_data) {
            out_data->sequence_data = PF_NEW_HANDLE(sizeof(SeqDataV2));
            if (out_data->sequence_data) {
                auto* z = (SeqDataV2*)PF_LOCK_HANDLE(out_data->sequence_data);
                std::memset(z, 0, sizeof(SeqDataV2));
                PF_UNLOCK_HANDLE(out_data->sequence_data);
            }
        } else {
            const size_t old = PF_GET_HANDLE_SIZE(out_data->sequence_data);
            if (old < sizeof(SeqDataV2) && !PF_RESIZE_HANDLE(sizeof(SeqDataV2), &out_data->sequence_data)) {
                auto* z = (char*)PF_LOCK_HANDLE(out_data->sequence_data);
                std::memset(z + old, 0, sizeof(SeqDataV2) - old);  // hasWorld = 0 until written below
                PF_UNLOCK_HANDLE(out_data->sequence_data);
            }
        }
        if (out_data->sequence_data && PF_GET_HANDLE_SIZE(out_data->sequence_data) >= sizeof(SeqDataV2)) {
            // A duplicated effect starts with the original's sequence data: just take the new key.
            // (Do not unregister the old key here: it belongs to the original instance.)
            auto* sd = (SeqDataV2*)PF_LOCK_HANDLE(out_data->sequence_data);
            sd->base.compItemId = key.compItemId;
            sd->base.layerId = key.layerId;
            if (src != ParamSource::SeqData) {
                sd->base.p = snap.p;
                sd->base.hasParams = true;
                sd->world = snap.world;
                sd->hasWorld = 1;
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

// The merge's whole-layer reasons for the effect's layer at the current time (0 if none or unknown).
unsigned MergeBlockNow(PF_InData* in_data)
{
    SPBasicSuite* sp = in_data->pica_basicP;
    Suite<AEGP_PFInterfaceSuite1> pfi(sp, kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1);
    Suite<AEGP_LayerSuite9> ls(sp, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    if (!pfi || !ls) return 0;
    AEGP_LayerH L = nullptr;
    A_Time lt = {in_data->current_time, (A_u_long)in_data->time_scale}, ct = {0, 1};
    unsigned block = 0;
    if (pfi->AEGP_GetEffectLayer(in_data->effect_ref, &L) || !L || ls->AEGP_ConvertLayerToCompTime(L, &lt, &ct) ||
        !rnd::ReadMergeBlock(L, ct, &block))
        return 0;
    return block;
}

PF_Err UpdateParamsUi(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[])
{
    const SelfState st = RegisterSelf(in_data, out_data, ParamSource::Array, params, false);
    const bool fast = IsFastMode(params[RP_RENDER_MODE]->u.pd.value);
    // 3D Layers with Optimize on: the whole-layer reasons it cannot merge this layer, logged (read by
    // tests/ae/check_merge.py, scene_light)
    if (st == SelfState::Ready && !fast && params[RP_MERGE]->u.bd.value) {
        const unsigned mergeBlock = MergeBlockNow(in_data);
        static LogLimit logged;
        if (logged.Take(50))
            RLog("status: comp %d layer %d merge block %x", g_lastKey.compItemId, g_lastKey.layerId, mergeBlock);
    }
    // Optimize does nothing in Fast (2D): greyed out there
    PF_ParamUtilsSuite3* pu = nullptr;
    if (in_data->pica_basicP->AcquireSuite(kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3, (const void**)&pu) ==
            kSPNoError &&
        pu) {
        PF_ParamDef m = *params[RP_MERGE];
        const bool was = (m.ui_flags & PF_PUI_DISABLED) != 0;
        if (fast != was) {
            if (fast) m.ui_flags |= PF_PUI_DISABLED;
            else m.ui_flags &= ~PF_PUI_DISABLED;
            pu->PF_UpdateParamUI(in_data->effect_ref, RP_MERGE, &m);
        }
        // Canvas: the other way round (Fast (2D) only)
        PF_ParamDef cv = *params[RP_CANVAS];
        const bool cvOff = (cv.ui_flags & PF_PUI_DISABLED) != 0;
        if (fast == cvOff) {
            if (fast) cv.ui_flags &= ~PF_PUI_DISABLED;
            else cv.ui_flags |= PF_PUI_DISABLED;
            pu->PF_UpdateParamUI(in_data->effect_ref, RP_CANVAS, &cv);
        }
        in_data->pica_basicP->ReleaseSuite(kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3);
    }
    return PF_Err_NONE;
}

// The user switched fast mode (spec 12.13). On: the layer's transform at this frame goes into the World
// Transform and the layer to its initial 2D state. Off: the layer becomes 3D again; nothing is moved back.
void FastToggled(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[])
{
    const bool on = IsFastMode(params[RP_RENDER_MODE]->u.pd.value);
    if (on) {
        rnd::WorldXform w;
        bool worldChanged = false;
        TransferResult r = TransferLayerToWorld(in_data, g_pluginId, WorldFromArray(params), &w, &worldChanged);
        if (worldChanged) {
            for (int k = 0; k < 3; k++) {
                params[RP_WORLD_POS_X + k]->u.fs_d.value = w.pos[k];
                params[RP_WORLD_ROT_X + k]->u.ad.value = FLOAT2FIX(w.rotDeg[k]);
                params[RP_WORLD_SCALE_X + k]->u.fs_d.value = w.scale[k];
                params[RP_WORLD_POS_X + k]->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
                params[RP_WORLD_ROT_X + k]->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
                params[RP_WORLD_SCALE_X + k]->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
            }
        }
        RLog("fast mode: the layer transform's transfer %s",
             r == TransferResult::Failed || r == TransferResult::Partial ? "failed"
             : r == TransferResult::RefusedSeparated                     ? "refused (separated dimensions)"
             : r == TransferResult::RefusedWorldAnimated                 ? "refused (World Transform animated)"
             : r == TransferResult::DoneAnimationDropped                 ? "done (the layer's animation dropped)"
                                                                         : "done");
    } else {
        SetLayer3D(in_data, true);
    }
    RegisterSelf(in_data, out_data, ParamSource::Array, params, false);
    out_data->out_flags |= PF_OutFlag_REFRESH_UI;
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
    out_data->my_version = PF_VERSION(kDupliXVersion[0], kDupliXVersion[1], kDupliXVersion[2], PF_Stage_DEVELOP, 1);
    out_data->out_flags = PF_OutFlag_PIX_INDEPENDENT | PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_SEND_UPDATE_PARAMS_UI;
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE |
                           PF_OutFlag2_I_MIX_GUID_DEPENDENCIES | PF_OutFlag2_SUPPORTS_THREADED_RENDERING |
                           PF_OutFlag2_SUPPORTS_GPU_RENDER_F32 | PF_OutFlag2_SUPPORTS_DIRECTX_RENDERING;
    RLog("GlobalSetup: %s, host %d.%d", BuildId().c_str(), in_data->version.major, in_data->version.minor);
    rnd::GpuRasterSetLog(&RLog);
    // test only (tests/ae/run_fast_mode.ps1 -GpuFail): every fast-mode GPU launch fails, the CPU fallback draws
    if (GetEnvironmentVariableA("DUPLIX_GPU_FAIL", nullptr, 0) > 0) rnd::GpuRasterTestFail(true);
    Suite<AEGP_UtilitySuite6> util(in_data->pica_basicP, kAEGPUtilitySuite, kAEGPUtilitySuiteVersion6);
    if (util && !util->AEGP_RegisterWithAEGP(nullptr, "DupliX", &g_pluginId))
        rnd::InstallHooks(in_data->pica_basicP, g_pluginId);
    else
        RLog("GlobalSetup: AEGP_RegisterWithAEGP failed; hooks not installed");
    return PF_Err_NONE;
}

PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data)
{
    PF_ParamDef def;
    // (the SDK's macros return from this function on an error)
#define DX_RANGE(name, max, sliderMax, dflt, id) \
    AEFX_CLR_STRUCT(def);                        \
    PF_ADD_FLOAT_SLIDERX(name, 0, max, 0, sliderMax, dflt, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, id)
#define DX_PERCENT(name, min, max, sliderMin, sliderMax, id) \
    AEFX_CLR_STRUCT(def);                                    \
    PF_ADD_FLOAT_SLIDERX(name, min, max, sliderMin, sliderMax, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, id)
#define DX_TOPIC(name, id) \
    AEFX_CLR_STRUCT(def);  \
    PF_ADD_TOPIC(name, id)
#define DX_END_TOPIC(id)  \
    AEFX_CLR_STRUCT(def); \
    PF_END_TOPIC(id)

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Copies", 0, 100000, 1, 1000, 10, RD_COUNT);
    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Random Seed", 0, 1000000, 0, 1000, 0, RD_SEED);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Keep Original in Place", FALSE, 0, RD_KEEP_FIRST);

    DX_TOPIC("Position", RD_POS_TOPIC);
    DX_RANGE("Position Range X", 100000, 2000, 200, RD_POS_X);
    DX_RANGE("Position Range Y", 100000, 2000, 200, RD_POS_Y);
    DX_RANGE("Position Range Z", 100000, 2000, 200, RD_POS_Z);
    DX_END_TOPIC(RD_POS_TOPIC_END);

    DX_TOPIC("Rotation", RD_ROT_TOPIC);
    DX_RANGE("Rotation Range X", 180, 180, 0, RD_ROT_X);
    DX_RANGE("Rotation Range Y", 180, 180, 0, RD_ROT_Y);
    DX_RANGE("Rotation Range Z", 180, 180, 0, RD_ROT_Z);
    DX_END_TOPIC(RD_ROT_TOPIC_END);

    DX_TOPIC("Scale", RD_SCALE_TOPIC);
    DX_PERCENT("Scale Min", 0, 10000, 0, 400, RD_SCALE_MIN);
    DX_PERCENT("Scale Max", 0, 10000, 0, 400, RD_SCALE_MAX);
    DX_END_TOPIC(RD_SCALE_TOPIC_END);

    DX_TOPIC("Opacity", RD_OPACITY_TOPIC);
    DX_PERCENT("Opacity Min", 0, 100, 0, 100, RD_OPACITY_MIN);
    DX_PERCENT("Opacity Max", 0, 100, 0, 100, RD_OPACITY_MAX);
    DX_END_TOPIC(RD_OPACITY_TOPIC_END);

    // the world the copies live in (both render modes); names unique in the effect (expressions find them by name)
    DX_TOPIC("World Transform", RD_WORLD_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("World Position X", -100000, 100000, -2000, 2000, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, RD_WORLD_POS_X);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("World Position Y", -100000, 100000, -2000, 2000, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, RD_WORLD_POS_Y);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("World Position Z", -100000, 100000, -2000, 2000, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, RD_WORLD_POS_Z);
    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("World Rotation X", 0, RD_WORLD_ROT_X);
    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("World Rotation Y", 0, RD_WORLD_ROT_Y);
    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("World Rotation Z", 0, RD_WORLD_ROT_Z);
    DX_PERCENT("World Scale X", -10000, 10000, 0, 400, RD_WORLD_SCALE_X);
    DX_PERCENT("World Scale Y", -10000, 10000, 0, 400, RD_WORLD_SCALE_Y);
    DX_PERCENT("World Scale Z", -10000, 10000, 0, 400, RD_WORLD_SCALE_Z);
    DX_END_TOPIC(RD_WORLD_TOPIC_END);

    DX_TOPIC("Rendering", RD_RENDER_TOPIC);
    // USER_CHANGED_PARAM on a change (the layer transform's transfer); a keyframed mode would make no sense with it
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Render Mode", 2, RM_3D_LAYERS, "3D Layers|Fast (2D)", PF_ParamFlag_SUPERVISE | PF_ParamFlag_CANNOT_TIME_VARY,
                  RD_RENDER_MODE);
    // 3D Layers only (greyed out in Fast (2D))
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Optimize", "", FALSE, PF_ParamFlag_CANNOT_TIME_VARY, RD_MERGE);
    // Fast (2D) only (greyed out in 3D Layers): Comp Size draws within the comp-sized frame, Fit Copies grows the
    // drawing to hold every copy (seen after the layer's own 2D transform, a precomp or a later effect)
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Canvas", 2, CV_COMP, "Comp Size|Fit Copies", PF_ParamFlag_CANNOT_TIME_VARY, RD_CANVAS);
    DX_END_TOPIC(RD_RENDER_TOPIC_END);

#undef DX_RANGE
#undef DX_PERCENT
#undef DX_TOPIC
#undef DX_END_TOPIC
    out_data->num_params = RP_NUM_PARAMS;
    return PF_Err_NONE;
}

// ---- fast mode (spec 12.13): an ordinary plugin on a 2D layer. All copies are drawn here into one comp-sized
// image in layer px: screen px s is drawn at layer px q = s - (compW, compH) / 2 + anchor, so a layer in its
// initial 2D state (position = comp centre, 100%, no rotation) shows it 1:1 and the layer's 2D transform
// moves the whole image. The copies sit on a virtual 3D layer in its initial state: copies * G * V.

// The build ID (a new build never reuses AE's disk cache of an old one) and, for timing tests only, the
// value of DUPLIX_CACHE_SALT (a new salt makes AE render again instead of reading its disk cache).
void MixBuildId(PF_InData* in_data, PF_PreRenderExtra* extra)
{
    // read on every call: a test script running in AE ($.setenv) changes it between renders
    char v[64] = {0};
    DWORD vn = GetEnvironmentVariableA("DUPLIX_CACHE_SALT", v, sizeof(v));
    const std::string salt(v, vn < sizeof(v) ? vn : 0);
    const std::string& id = BuildId();
    extra->cb->GuidMixInPtr(in_data->effect_ref, (A_u_long)id.size(), id.data());
    if (!salt.empty()) extra->cb->GuidMixInPtr(in_data->effect_ref, (A_u_long)salt.size(), salt.data());
}

// What SmartRender needs, worked out in PreRender (pre_render_data).
struct FastPre {
    rnd::CameraView cam;
    double W[16];             // copy layer px -> world: G(world) * V (virtual 3D layer in its initial state)
    rnd::ScatterParams p;
    rnd::WorldXform world;
    double anchorX, anchorY;  // layer px (the effect's px: for shape / text layers, shifted by half the comp)
    bool vectorOrigin = false;  // a shape / text layer (anchor shifted)
    int canvas = CV_COMP;       // Canvas
    double dsX, dsY;
    PF_LRect inRect;          // the input's result rect (downsampled layer px)
};
void DeleteFastPre(void* p) { delete (FastPre*)p; }

// The parameter values at the effect's current time (smart render: params[] is not passed).
bool CheckoutScatterParams(PF_InData* in_data, rnd::ScatterParams* out, rnd::WorldXform* world)
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
    if (ok) {
        *out = ParamsFromArray(ptrs);
        *world = WorldFromArray(ptrs);
    }
    for (int i = RP_COUNT; i <= last; i++) PF_CHECKIN_PARAM(in_data, &defs[i]);
    return ok;
}

bool FastModeOn(PF_InData* in_data)
{
    PF_ParamDef d;
    AEFX_CLR_STRUCT(d);
    if (PF_CHECKOUT_PARAM(in_data, RP_RENDER_MODE, in_data->current_time, in_data->time_step, in_data->time_scale, &d))
        return false;
    const bool on = IsFastMode(d.u.pd.value);
    PF_CHECKIN_PARAM(in_data, &d);
    return on;
}

// Camera, comp size and anchor at the effect's current time (public AEGP only; works on a 2D layer).
bool FastSetup(PF_InData* in_data, FastPre* f, int* compW, int* compH)
{
    SPBasicSuite* sp = in_data->pica_basicP;
    Suite<AEGP_PFInterfaceSuite1> pfi(sp, kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1);
    Suite<AEGP_LayerSuite9> ls(sp, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    Suite<AEGP_CompSuite12> cs(sp, kAEGPCompSuite, kAEGPCompSuiteVersion12);
    Suite<AEGP_ItemSuite9> is(sp, kAEGPItemSuite, kAEGPItemSuiteVersion9);
    if (!pfi || !ls || !cs || !is) return false;
    A_Time ct = {0, 1};  // comp time: what the camera read takes
    AEGP_LayerH L = nullptr;
    AEGP_CompH comp = nullptr;
    AEGP_ItemH item = nullptr;
    if (pfi->AEGP_ConvertEffectToCompTime(in_data->effect_ref, in_data->current_time, in_data->time_scale, &ct) ||
        pfi->AEGP_GetEffectLayer(in_data->effect_ref, &L) || ls->AEGP_GetLayerParentComp(L, &comp) ||
        cs->AEGP_GetItemFromComp(comp, &item))
        return false;
    A_long w = 0, h = 0;
    A_Matrix4 cam;
    A_FpLong dist = 0;
    A_short ipw = 0, iph = 0;
    if (is->AEGP_GetItemDimensions(item, &w, &h) || w <= 0 || h <= 0 ||
        pfi->AEGP_GetEffectCameraMatrix(in_data->effect_ref, &ct, &cam, &dist, &ipw, &iph) || !(dist > 0))
        return false;
    A_Time lt = {in_data->current_time, (A_u_long)(in_data->time_scale ? in_data->time_scale : 1)};
    if (!rnd::ReadLayerAnchor(L, lt, &f->anchorX, &f->anchorY)) return false;
    // Shape and text layers: the effect's px are the layer's coordinates - anchor + half the comp (a comp-sized
    // frame centred on the anchor; vector_probe, 11/27: moving the anchor moves the input rect the other way).
    // So the anchor is always at half the comp in the effect's px. Plain AV layers start at their corner.
    AEGP_ObjectType ot = AEGP_ObjectType_NONE;
    if (!ls->AEGP_GetLayerObjectType(L, &ot) && (ot == AEGP_ObjectType_VECTOR || ot == AEGP_ObjectType_TEXT)) {
        f->anchorX = w / 2.0;
        f->anchorY = h / 2.0;
        f->vectorOrigin = true;
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) f->cam.camToWorld[r * 4 + c] = cam.mat[r][c];
    f->cam.dist = dist;
    f->cam.compW = (double)w;
    f->cam.compH = (double)h;
    double G[16], V[16];
    rnd::WorldM4(f->world, f->anchorX, f->anchorY, G);
    rnd::InitialLayerToWorld(f->anchorX, f->anchorY, (double)w, (double)h, V);
    rnd::Mul4(G, V, f->W);
    *compW = (int)w;
    *compH = (int)h;
    return true;
}

// Canvas = Fit Copies: the comp-sized frame grown to hold every copy (their screen bounds, a margin of 2 px for
// the edges' filtering), in (downsampled) layer px. The area is capped (kFitMaxPixels at full resolution): past it
// the extent is cut around the comp-sized frame's centre.
constexpr double kFitMaxPixels = 32.0 * 1024 * 1024;
PF_LRect FitCopiesRect(const FastPre& f, const PF_LRect& full)
{
    const double srcRect[4] = {f.inRect.left / f.dsX, f.inRect.top / f.dsY, f.inRect.right / f.dsX,
                               f.inRect.bottom / f.dsY};
    if (!(srcRect[2] > srcRect[0]) || !(srcRect[3] > srcRect[1])) return full;
    // screen px of the copies' bounds (full resolution)
    double l = 1e300, t = 1e300, r = -1e300, b = -1e300;
    for (int i = 0; i < f.p.count; i++) {
        rnd::CopyXform x;
        rnd::ComputeCopy(f.p, i, f.anchorX, f.anchorY, &x);
        double bb[4];
        if (x.opacity <= 0 || !rnd::CopyScreenBounds(f.cam, f.W, x.m4, srcRect, bb)) continue;
        l = std::min(l, bb[0]), t = std::min(t, bb[1]), r = std::max(r, bb[2]), b = std::max(b, bb[3]);
    }
    if (!(r > l) || !(b > t)) return full;
    // screen px -> layer px (screen = layer - anchor + comp centre), with the margin
    const double sx = f.cam.compW / 2 - f.anchorX, sy = f.cam.compH / 2 - f.anchorY, m = 2;
    double L = l - sx - m, T = t - sy - m, R = r - sx + m, B = b - sy + m;
    // never smaller than the comp-sized frame
    L = std::min(L, full.left / f.dsX), T = std::min(T, full.top / f.dsY);
    R = std::max(R, full.right / f.dsX), B = std::max(B, full.bottom / f.dsY);
    // the cap: shrink each side towards the frame's centre in the same ratio
    const double area = (R - L) * (B - T);
    if (area > kFitMaxPixels) {
        const double cx = (full.left + full.right) / 2.0 / f.dsX, cy = (full.top + full.bottom) / 2.0 / f.dsY;
        const double k = std::sqrt(kFitMaxPixels / area);
        L = cx + (L - cx) * k, R = cx + (R - cx) * k, T = cy + (T - cy) * k, B = cy + (B - cy) * k;
    }
    auto clampL = [](double v) { return (A_long)std::max(-30000.0, std::min(30000.0, v)); };
    return {clampL(std::floor(L * f.dsX)), clampL(std::floor(T * f.dsY)), clampL(std::ceil(R * f.dsX)),
            clampL(std::ceil(B * f.dsY))};
}

PF_Err FastPreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* extra)
{
    RegisterSelf(in_data, out_data, ParamSource::Aegp, nullptr, false);  // keeps the hooks off this layer
    auto* f = new FastPre();
    f->dsX = in_data->downsample_x.den ? (double)in_data->downsample_x.num / in_data->downsample_x.den : 1.0;
    f->dsY = in_data->downsample_y.den ? (double)in_data->downsample_y.num / in_data->downsample_y.den : 1.0;
    int cw = 0, ch = 0;
    bool ok = CheckoutScatterParams(in_data, &f->p, &f->world) && FastSetup(in_data, f, &cw, &ch);
    // The whole source is needed whatever part of the screen is asked for.
    PF_RenderRequest req = extra->input->output_request;
    req.rect.left = req.rect.top = -30000;
    req.rect.right = req.rect.bottom = 30000;
    PF_CheckoutResult in_result;
    AEFX_CLR_STRUCT(in_result);
    PF_Err err = extra->cb->checkout_layer(in_data->effect_ref, RP_INPUT, RP_INPUT, &req, in_data->current_time,
                                           in_data->time_step, in_data->time_scale, &in_result);
    f->inRect = in_result.result_rect;
    if (ok) {
        PF_ParamDef cd;
        AEFX_CLR_STRUCT(cd);
        if (!PF_CHECKOUT_PARAM(in_data, RP_CANVAS, in_data->current_time, in_data->time_step, in_data->time_scale, &cd)) {
            f->canvas = cd.u.pd.value;
            PF_CHECKIN_PARAM(in_data, &cd);
        }
    }
    // The output depends on the camera (AE does not know a 2D layer does), the world transform, the comp
    // size, the time and the downsample: mix them all into the cache key (required with
    // I_MIX_GUID_DEPENDENCIES in any case).
    double key[64] = {0};
    int n = 0;
    for (int i = 0; i < 16; i++) key[n++] = f->cam.camToWorld[i];
    for (int i = 0; i < 16; i++) key[n++] = f->W[i];
    const double more[] = {f->cam.dist, f->cam.compW, f->cam.compH, f->dsX, f->dsY, (double)in_data->current_time,
                           (double)in_data->time_scale, f->anchorX, f->anchorY, ok ? 1.0 : 0.0, (double)f->canvas};
    for (double v : more) key[n++] = v;
    if (extra->cb->GuidMixInPtr) {
        MixBuildId(in_data, extra);
        extra->cb->GuidMixInPtr(in_data->effect_ref, sizeof(double) * n, key);
    }
    if (err) {
        delete f;
        return err;
    }
    if (ok) {
        // the comp-sized area in (downsampled) layer px: screen (0, 0) is layer px anchor - comp centre
        PF_LRect full = {(A_long)std::floor((f->anchorX - cw / 2.0) * f->dsX),
                         (A_long)std::floor((f->anchorY - ch / 2.0) * f->dsY),
                         (A_long)std::ceil((f->anchorX + cw / 2.0) * f->dsX),
                         (A_long)std::ceil((f->anchorY + ch / 2.0) * f->dsY)};  // l, t, r, b
        if (f->canvas == CV_FIT) full = FitCopiesRect(*f, full);
        const PF_LRect& q = extra->input->output_request.rect;
        PF_LRect x = {std::max(q.left, full.left), std::max(q.top, full.top), std::min(q.right, full.right),
                      std::min(q.bottom, full.bottom)};
        if (x.right < x.left) x.right = x.left;
        if (x.bottom < x.top) x.bottom = x.top;
        extra->output->result_rect = x;
        extra->output->max_result_rect = full;
        extra->output->pre_render_data = f;
        extra->output->delete_pre_render_data_func = DeleteFastPre;
        if (extra->input->what_gpu == PF_GPU_Framework_CUDA && rnd::GpuRasterUsable())
            extra->output->flags |= PF_RenderOutputFlag_GPU_RENDER_POSSIBLE;
    } else {
        // Nothing to show (camera or comp unreadable): an empty output.
        delete f;
        PF_LRect none = {0, 0, 0, 0};
        extra->output->result_rect = none;
        extra->output->max_result_rect = none;
    }
    static LogLimit logged;
    if (logged.Take(6))
        RLog("fast: prerender ok=%d comp %dx%d in=(%d,%d,%d,%d) max=(%d,%d,%d,%d) anchor (%.2f,%.2f)%s ds %.3f "
             "out=(%d,%d,%d,%d) t=%d/%d",
             (int)ok, cw, ch, (int)in_result.result_rect.left, (int)in_result.result_rect.top,
             (int)in_result.result_rect.right, (int)in_result.result_rect.bottom,
             (int)in_result.max_result_rect.left, (int)in_result.max_result_rect.top,
             (int)in_result.max_result_rect.right, (int)in_result.max_result_rect.bottom, ok ? f->anchorX : 0.0,
             ok ? f->anchorY : 0.0, ok && f->vectorOrigin ? " vector" : "", ok ? f->dsX : 0.0,
             (int)extra->output->result_rect.left, (int)extra->output->result_rect.top,
             (int)extra->output->result_rect.right, (int)extra->output->result_rect.bottom,
             (int)in_data->current_time, (int)in_data->time_scale);
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

// Every copy's screen -> source projection, opacity and centre depth (CPU and GPU draw the same list).
// inW, inH: the input buffer's size (downsampled px, at f.inRect's origin).
std::vector<rnd::RasterCopy> BuildFastCopies(const FastPre& f, int inW, int inH)
{
    // the source in full-resolution layer px
    const double srcRect[4] = {f.inRect.left / f.dsX, f.inRect.top / f.dsY, (f.inRect.left + inW) / f.dsX,
                               (f.inRect.top + inH) / f.dsY};
    const double cx = (srcRect[0] + srcRect[2]) / 2, cy = (srcRect[1] + srcRect[3]) / 2;
    std::vector<rnd::RasterCopy> copies;
    copies.reserve((size_t)std::max(f.p.count, 0));
    for (int i = 0; i < f.p.count; i++) {
        rnd::CopyXform x;
        rnd::ComputeCopy(f.p, i, f.anchorX, f.anchorY, &x);
        rnd::RasterCopy rc;
        if (x.opacity <= 0 || !rnd::CopyHomography(f.cam, f.W, x.m4, srcRect, rc.screenToSrc)) continue;
        rc.opacity = x.opacity;
        rc.depth = rnd::CopyDepth(f.cam, f.W, x.m4, cx, cy);
        copies.push_back(rc);
    }
    return copies;
}

PF_Err FastSmartRender(PF_InData* in_data, const FastPre* f, PF_EffectWorld* inW, PF_EffectWorld* outW)
{
    int bpc = WorldBpc(in_data, outW);
    if (!bpc) return PF_Err_BAD_CALLBACK_PARAM;
    rnd::RasterImage dst = {outW->data, (int)outW->width, (int)outW->height, (long)outW->rowbytes, bpc,
                            (int)outW->origin_x, (int)outW->origin_y};
    if (f) {  // the output is in layer px: screen px = layer px - anchor + comp centre
        dst.screenShiftX = f->cam.compW / 2 - f->anchorX;
        dst.screenShiftY = f->cam.compH / 2 - f->anchorY;
    }
    rnd::RasterImage src = {nullptr, 0, 0, 0, bpc, 0, 0};
    std::vector<rnd::RasterCopy> copies;
    if (f && inW && inW->data && WorldBpc(in_data, inW) == bpc) {
        src = {inW->data, (int)inW->width, (int)inW->height, (long)inW->rowbytes, bpc, (int)f->inRect.left,
               (int)f->inRect.top};
        copies = BuildFastCopies(*f, (int)inW->width, (int)inW->height);
        static std::atomic<int> logged{0};
        if (++logged <= 3)
            RLog("fast: render bpc %d in %dx%d origin(%d,%d) inRect(%d,%d) out %dx%d origin(%d,%d) copies %zu/%d", bpc,
                 (int)inW->width, (int)inW->height, (int)inW->origin_x, (int)inW->origin_y, (int)f->inRect.left,
                 (int)f->inRect.top, (int)outW->width, (int)outW->height, (int)outW->origin_x, (int)outW->origin_y,
                 copies.size(), f->p.count);
    }
    unsigned hw = std::thread::hardware_concurrency();
    // Many large copies take long: stop when AE cancels (scrubbing, render queue stop).
    auto aborted = [in_data]() { return in_data->inter.abort && PF_ABORT(in_data) != PF_Err_NONE; };
    if (!rnd::RasterizeCopies(src, copies.data(), (int)copies.size(), f ? f->dsX : 1.0, f ? f->dsY : 1.0, dst,
                              hw ? (int)hw : 4, aborted))
        return PF_Interrupt_CANCEL;
    return PF_Err_NONE;
}

PF_Err PreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* extra)
{
    {
        // DUPLIX_PRE_STACK=1 (diagnostics): who asks for the pre-render (module+offset of the callers)
        static const bool s_stack = GetEnvironmentVariableA("DUPLIX_PRE_STACK", nullptr, 0) > 0;
        static std::atomic<int> n{0};
        if (s_stack && ++n % 50 == 0 && n <= 5000) {
            void* fr[24] = {};
            const USHORT k = RtlCaptureStackBackTrace(1, 24, fr, nullptr);
            std::string s;
            for (USHORT i = 0; i < k; i++) {
                HMODULE m = nullptr;
                char name[MAX_PATH] = "?";
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)fr[i], &m) && m)
                    GetModuleFileNameA(m, name, MAX_PATH);
                const char* bn = strrchr(name, '\\');
                char one[160];
                snprintf(one, sizeof(one), " %s+%llx", bn ? bn + 1 : name, (unsigned long long)((uintptr_t)fr[i] - (uintptr_t)m));
                s += one;
            }
            RLog("pre stack:%s", s.c_str());
        }
    }
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
    if (extra->cb->GuidMixInPtr) MixBuildId(in_data, extra);
    {
        // merge (Task 2 gate, log only): AE's own camera for this effect, to compare with the one the hooks read
        static LogLimit logged;
        PF_ParamDef d;
        AEFX_CLR_STRUCT(d);
        if (logged.Count() < 3 && !PF_CHECKOUT_PARAM(in_data, RP_MERGE, in_data->current_time, in_data->time_step,
                                                    in_data->time_scale, &d)) {
            const bool merge = d.u.bd.value != 0;
            PF_CHECKIN_PARAM(in_data, &d);
            FastPre f;
            int cw = 0, ch = 0;
            if (merge && logged.Take(3) && FastSetup(in_data, &f, &cw, &ch))
                RLog("merge: effect cam dist=%.2f camPos=(%.2f,%.2f,%.2f)", f.cam.dist, f.cam.camToWorld[12],
                     f.cam.camToWorld[13], f.cam.camToWorld[14]);
        }
    }

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

// CUDA devices AE has set up for this effect (its GPU acceleration is CUDA) whose kernel loaded.
std::atomic<int> g_aeCudaDevices{0};

PF_Err GpuDeviceSetup(PF_InData* in_data, PF_OutData* out_data, PF_GPUDeviceSetupExtra* extra)
{
    PF_GPUDeviceInfo info;
    AEFX_CLR_STRUCT(info);
    Suite<PF_GPUDeviceSuite1> gs(in_data->pica_basicP, kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1);
    if (!gs || gs->GetDeviceInfo(in_data->effect_ref, extra->input->device_index, &info)) return PF_Err_NONE;
    void* data = nullptr;
    bool ok = rnd::GpuDeviceSetup(extra->input->what_gpu, info, &data);
    bool fastGpu = false;
    if (extra->input->what_gpu == PF_GPU_Framework_CUDA) {
        // The fast mode's kernel is loaded now: a driver that cannot run it is known before the first frame
        // (the fast mode then stays on the CPU; the normal mode's copy needs no kernel). gpu_data = the context,
        // so setdown can drop its cached kernel.
        fastGpu = rnd::GpuRasterPrepare(info.contextPV);
        data = info.contextPV;
        if (fastGpu) g_aeCudaDevices++;  // the merge may draw its group textures on the GPU too
    }
    extra->output->gpu_data = data;
    if (ok) out_data->out_flags2 = PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;
    RLog("gpu: device %u framework %d -> %s, fast-mode kernel %s", (unsigned)extra->input->device_index,
         (int)extra->input->what_gpu, ok ? "supported" : "not supported (CPU)", fastGpu ? "loaded" : "not loaded");
    return PF_Err_NONE;
}

PF_Err GpuDeviceSetdown(PF_GPUDeviceSetdownExtra* extra)
{
    if (extra->input->what_gpu == PF_GPU_Framework_CUDA) {
        if (g_aeCudaDevices > 0) g_aeCudaDevices--;
        rnd::GpuRasterForget(extra->input->gpu_data);
        return PF_Err_NONE;
    }
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

// Fast mode on the GPU (CUDA only: PreRender allows the GPU render for CUDA only). AE's GPU worlds are BGRA
// float, straight alpha, like gpu_raster expects.
PF_Err FastSmartRenderGpu(PF_InData* in_data, PF_SmartRenderExtra* extra, PF_EffectWorld* inW, PF_EffectWorld* outW)
{
    const FastPre* f = (const FastPre*)extra->input->pre_render_data;
    Suite<PF_GPUDeviceSuite1> gs(in_data->pica_basicP, kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1);
    PF_GPUDeviceInfo info;
    AEFX_CLR_STRUCT(info);
    void* src = nullptr;
    void* dst = nullptr;
    // PreRender offers the GPU for CUDA only
    if (extra->input->what_gpu != PF_GPU_Framework_CUDA || !gs) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    PF_Err e = gs->GetDeviceInfo(in_data->effect_ref, extra->input->device_index, &info);
    if (!e) e = gs->GetGPUWorldData(in_data->effect_ref, outW, &dst);
    if (!e && inW) e = gs->GetGPUWorldData(in_data->effect_ref, inW, &src);
    if (e) {
        RLog("fast: gpu suite error %d", (int)e);
        return e;
    }
    // the copy lists in AE's GPU memory (AE can purge its caches for them when memory is short)
    rnd::GpuAllocator al;
    const PF_ProgPtr ref = in_data->effect_ref;
    const A_u_long dev = extra->input->device_index;
    PF_GPUDeviceSuite1* gsp = gs.s;
    al.alloc = [gsp, ref, dev](size_t bytes) -> void* {
        void* p = nullptr;
        return gsp->AllocateDeviceMemory(ref, dev, bytes, &p) ? nullptr : p;
    };
    al.free = [gsp, ref, dev](void* p) { gsp->FreeDeviceMemory(ref, dev, p); };
    rnd::GpuImage gin = {src, inW ? (int)inW->width : 0, inW ? (int)inW->height : 0, inW ? (long)inW->rowbytes : 0,
                         f ? (int)f->inRect.left : 0, f ? (int)f->inRect.top : 0};
    rnd::GpuImage gout = {dst, (int)outW->width, (int)outW->height, (long)outW->rowbytes, (int)outW->origin_x,
                          (int)outW->origin_y};
    std::vector<rnd::RasterCopy> copies;
    if (f && src) {
        gout.screenShiftX = f->cam.compW / 2 - f->anchorX;  // the output is in layer px (see FastSmartRender)
        gout.screenShiftY = f->cam.compH / 2 - f->anchorY;
        copies = BuildFastCopies(*f, gin.width, gin.height);
    }
    auto aborted = [in_data]() { return in_data->inter.abort && PF_ABORT(in_data) != PF_Err_NONE; };
    const double dsX = f ? f->dsX : 1.0, dsY = f ? f->dsY : 1.0;
    rnd::GpuRasterStatus st = rnd::GpuRasterize(info.contextPV, info.command_queuePV, gin, gout, copies.data(),
                                                (int)copies.size(), dsX, dsY, aborted, &al);
    bool viaCpu = false;
    if (st == rnd::kGpuFailed) {
        // the GPU could not draw this frame (logged by gpu_raster): the CPU rasterizer draws it into AE's buffers
        viaCpu = true;
        st = rnd::GpuRasterizeOnCpu(info.contextPV, info.command_queuePV, gin, gout, copies.data(), (int)copies.size(),
                                    dsX, dsY, aborted);
    }
    static std::atomic<int> logged{0};
    if (++logged <= 3 || st != rnd::kGpuOk || viaCpu)
        RLog("fast: gpu render ok=%d status %d via cpu %d in %dx%d rowbytes %d origin(%d,%d) out %dx%d rowbytes %d "
             "origin(%d,%d) copies %zu",
             (int)(st == rnd::kGpuOk), (int)st, (int)viaCpu, gin.width, gin.height, (int)gin.rowBytes, gin.originX,
             gin.originY, gout.width, gout.height, (int)gout.rowBytes, gout.originX, gout.originY, copies.size());
    if (st == rnd::kGpuAborted) return PF_Interrupt_CANCEL;
    return st == rnd::kGpuOk ? PF_Err_NONE : PF_Err_INTERNAL_STRUCT_DAMAGED;
}

PF_Err SmartRenderGpu(PF_InData* in_data, PF_OutData*, PF_SmartRenderExtra* extra)
{
    RenderMark mark{in_data};
    PF_Err err = PF_Err_NONE;
    PF_EffectWorld* inW = nullptr;
    PF_EffectWorld* outW = nullptr;
    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, RP_INPUT, &inW));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &outW));
    if (!err && outW && FastModeOn(in_data)) {
        err = FastSmartRenderGpu(in_data, extra, inW, outW);
    } else if (!err && inW && outW) {
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
    {
        // DUPLIX_CMDLOG=1 (diagnostics): every command with its thread and time
        static const bool s_cmdLog = GetEnvironmentVariableA("DUPLIX_CMDLOG", nullptr, 0) > 0;
        if (s_cmdLog && in_data)
            RLog("cmd: %d main %d time %d/%u", (int)cmd, (int)rnd::IsMainThread(), (int)in_data->current_time,
                 (unsigned)in_data->time_scale);
    }
    try {
        switch (cmd) {
        case PF_Cmd_GLOBAL_SETUP:
            err = GlobalSetup(in_data, out_data);
            break;
        case PF_Cmd_ABOUT:
            // the Effect Controls' About: the name, the version and where to find more (the SDK's practice)
            PF_SPRINTF(out_data->return_msg, "DupliX %d.%d.%d\rRandom 3D copies of a layer.\rhttps://github.com/yak9909/DupliX",
                       kDupliXVersion[0], kDupliXVersion[1], kDupliXVersion[2]);
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
            if (rnd::IsMainThread()) {
                if (((PF_UserChangedParamExtra*)extra)->param_index == RP_RENDER_MODE)
                    FastToggled(in_data, out_data, params);
                RegisterSelf(in_data, out_data, ParamSource::Array, params, true);
            }
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
    } catch (const std::bad_alloc&) {
        err = PF_Err_OUT_OF_MEMORY;  // e.g. the fast mode's tile lists past kMaxTileEntries
    } catch (...) {
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    if (d <= 200) RLog("dbg: cmd %d leave err=%d", (int)cmd, (int)err);
    return err;
}

// The merge draws its group textures on the GPU when AE's GPU acceleration is CUDA (a CUDA device set up for this
// effect, its kernel loaded) and no GPU launch has failed. DUPLIX_MERGE_GPU_DRAW (read per call; tests): 0 = never,
// 1 = whenever the driver can (also without AE's CUDA setting).
bool MergeGpuAllowed()
{
    char v[8] = {0};
    if (GetEnvironmentVariableA("DUPLIX_MERGE_GPU_DRAW", v, sizeof(v)) > 0) {
        if (v[0] == '0') return false;
        if (v[0] == '1') return rnd::GpuRasterUsable();
    }
    return g_aeCudaDevices.load() > 0 && rnd::GpuRasterUsable();
}
