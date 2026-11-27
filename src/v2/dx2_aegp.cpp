// DupliX 2 (prototype): the parts that go through AEGP (UI commands only, never render):
// the layers chosen as Sources, and which controls are shown.
#include "v2/dx2.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

namespace dx2 {

AEGP_PluginID g_pluginId = 0;

void Log(const char* fmt, ...)
{
    char path[MAX_PATH] = {0};
    // DUPLIX2_LOG, else the tests' DUPLIX_LOG (the same file as DupliX's, lines start with "dx2:")
    if (GetEnvironmentVariableA("DUPLIX2_LOG", path, sizeof(path)) == 0 &&
        GetEnvironmentVariableA("DUPLIX_LOG", path, sizeof(path)) == 0)
        return;
    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "a") || !fp) return;
    fputs("dx2: ", fp);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

namespace {

// per instance, keyed by the id in its sequence data (AE passes another effect_ref on every command).
// UI thread only, but guarded anyway.
struct Instance {
    int minSourceRows = 0;       // Source rows shown at least ("+ Add Source"; not saved: empty rows close on reopen)
    std::vector<signed char> hidden;  // what this instance last set per stream (-1 = never set)
};
std::mutex g_mu;
std::map<A_u_longlong, Instance> g_inst;
std::atomic<A_u_longlong> g_nextId{1};

struct SeqData {
    A_u_longlong magic;
    A_u_longlong id;  // this session's id of the instance (a new one after loading or duplicating)
};
constexpr A_u_longlong kSeqMagic = 0x44583253455131ULL;  // "DX2SEQ1"

A_u_longlong InstanceId(PF_InData* in_data)
{
    if (!in_data->sequence_data || PF_GET_HANDLE_SIZE(in_data->sequence_data) < sizeof(SeqData)) return 0;
    const SeqData* sd = (const SeqData*)PF_LOCK_HANDLE(in_data->sequence_data);
    const A_u_longlong id = sd->magic == kSeqMagic ? sd->id : 0;
    PF_UNLOCK_HANDLE(in_data->sequence_data);
    return id;
}

// id 0 (no sequence data): nothing is remembered, every row is set each time
Instance& Inst(PF_InData* in_data)
{
    const A_u_longlong id = InstanceId(in_data);
    Instance& s = g_inst[id];
    if (s.hidden.size() != (size_t)P_NUM || id == 0) s.hidden.assign(P_NUM, -1);
    return s;
}

// The effect's streams (one AEGP effect handle for the call).
struct Streams {
    SPBasicSuite* sp;
    Suite<AEGP_PFInterfaceSuite1> pfi;
    Suite<AEGP_EffectSuite4> es;
    Suite<AEGP_StreamSuite6> ss;
    AEGP_EffectRefH eff = nullptr;
    explicit Streams(PF_InData* in_data)
        : sp(in_data->pica_basicP), pfi(sp, kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1),
          es(sp, kAEGPEffectSuite, kAEGPEffectSuiteVersion4), ss(sp, kAEGPStreamSuite, kAEGPStreamSuiteVersion6)
    {
        if (pfi && es && ss && g_pluginId) pfi->AEGP_GetNewEffectForEffect(g_pluginId, in_data->effect_ref, &eff);
    }
    ~Streams()
    {
        if (eff) es->AEGP_DisposeEffect(eff);
    }
    explicit operator bool() const { return eff != nullptr; }
    AEGP_StreamRefH Get(int index) const
    {
        AEGP_StreamRefH s = nullptr;
        if (eff && ss->AEGP_GetNewEffectStreamByIndex(g_pluginId, eff, index, &s)) s = nullptr;
        return s;
    }
    void Dispose(AEGP_StreamRefH s) const
    {
        if (s) ss->AEGP_DisposeStream(s);
    }
};

std::string Utf8(const A_UTF16Char* w)
{
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t*)w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, (const wchar_t*)w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

A_long ReadLayerId(PF_InData* in_data, const Streams& st, int index)
{
    AEGP_StreamRefH s = st.Get(index);
    if (!s) return 0;
    A_Time t = {in_data->current_time, (A_u_long)(in_data->time_scale ? in_data->time_scale : 1)};
    AEGP_StreamValue2 v;
    AEFX_CLR_STRUCT(v);
    A_long id = 0;
    if (!st.ss->AEGP_GetNewStreamValue(g_pluginId, s, AEGP_LTimeMode_LayerTime, &t, TRUE, &v)) {
        id = v.val.layer_id;
        st.ss->AEGP_DisposeStreamValue(&v);
    }
    st.Dispose(s);
    return id;
}

}  // namespace

A_long SourceLayerId(PF_InData* in_data, int slot)
{
    Streams st(in_data);
    return st ? ReadLayerId(in_data, st, P_SRC_LAYER + slot) : 0;
}

bool ReadSources(PF_InData* in_data, SourceInfo out[kSourceSlots])
{
    for (int k = 0; k < kSourceSlots; k++) out[k] = SourceInfo();
    Streams st(in_data);
    if (!st) return false;
    Suite<AEGP_LayerSuite9> ls(st.sp, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    Suite<AEGP_ItemSuite9> is(st.sp, kAEGPItemSuite, kAEGPItemSuiteVersion9);
    Suite<AEGP_MemorySuite1> ms(st.sp, kAEGPMemorySuite, kAEGPMemorySuiteVersion1);
    AEGP_LayerH self = nullptr;
    AEGP_CompH comp = nullptr;
    if (!ls || !is || !ms || st.pfi->AEGP_GetEffectLayer(in_data->effect_ref, &self) ||
        ls->AEGP_GetLayerParentComp(self, &comp))
        return false;
    for (int k = 0; k < kSourceSlots; k++) {
        const A_long id = ReadLayerId(in_data, st, P_SRC_LAYER + k);
        if (!id) continue;
        AEGP_LayerH L = nullptr;
        if (ls->AEGP_GetLayerFromLayerID(comp, id, &L) || !L) continue;
        out[k].used = true;
        AEGP_MemHandle nameH = nullptr, srcH = nullptr;
        if (!ls->AEGP_GetLayerName(g_pluginId, L, &nameH, &srcH)) {
            for (AEGP_MemHandle h : {nameH, srcH}) {
                if (!h) continue;
                A_UTF16Char* p = nullptr;
                if (out[k].name.empty() && !ms->AEGP_LockMemHandle(h, (void**)&p)) {
                    out[k].name = Utf8(p);
                    ms->AEGP_UnlockMemHandle(h);
                }
            }
            if (nameH) ms->AEGP_FreeMemHandle(nameH);
            if (srcH) ms->AEGP_FreeMemHandle(srcH);
        }
        AEGP_ItemH item = nullptr;
        A_long w = 0, h = 0;
        if (!ls->AEGP_GetLayerSourceItem(L, &item) && item && !is->AEGP_GetItemDimensions(item, &w, &h)) {
            out[k].width = (int)w;
            out[k].height = (int)h;
        }
    }
    return true;
}

bool SetSourceLayer(PF_InData* in_data, int slot, A_long layerId)
{
    Streams st(in_data);
    if (!st) return false;
    AEGP_StreamRefH s = st.Get(P_SRC_LAYER + slot);
    if (!s) return false;
    AEGP_StreamValue2 v;
    AEFX_CLR_STRUCT(v);
    v.streamH = s;
    v.val.layer_id = layerId;
    const bool ok = st.ss->AEGP_SetStreamValue(g_pluginId, s, &v) == A_Err_NONE;
    st.Dispose(s);
    return ok;
}

int SourceRowsShown(PF_InData* in_data, const SourceInfo src[kSourceSlots])
{
    int last = -1;
    for (int k = 0; k < kSourceSlots; k++)
        if (src[k].used) last = k;
    std::lock_guard<std::mutex> lk(g_mu);
    Instance& s = Inst(in_data);
    int rows = last + 1 > s.minSourceRows ? last + 1 : s.minSourceRows;
    if (rows < 1) rows = 1;
    if (rows > kSourceSlots) rows = kSourceSlots;
    return rows;
}

void SetMinSourceRows(PF_InData* in_data, int rows)
{
    std::lock_guard<std::mutex> lk(g_mu);
    Inst(in_data).minSourceRows = rows;
}

PF_Err SequenceSetup(PF_InData* in_data, PF_OutData* out_data)
{
    PF_Handle h = in_data->sequence_data;
    if (!h || PF_GET_HANDLE_SIZE(h) < sizeof(SeqData)) {
        if (h) PF_DISPOSE_HANDLE(h);
        h = PF_NEW_HANDLE(sizeof(SeqData));
        if (!h) return PF_Err_OUT_OF_MEMORY;
    }
    SeqData* sd = (SeqData*)PF_LOCK_HANDLE(h);
    sd->magic = kSeqMagic;
    sd->id = g_nextId.fetch_add(1);  // a loaded or duplicated instance starts with nothing remembered
    PF_UNLOCK_HANDLE(h);
    out_data->sequence_data = h;
    return PF_Err_NONE;
}

void SequenceSetdown(PF_InData* in_data, PF_OutData* out_data)
{
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const A_u_longlong id = InstanceId(in_data);
        if (id) g_inst.erase(id);
    }
    if (in_data->sequence_data) {
        PF_DISPOSE_HANDLE(in_data->sequence_data);
        out_data->sequence_data = nullptr;
    }
}

// ---- visibility ----

namespace {

void ApplyHidden(PF_InData* in_data, const std::vector<signed char>& want)
{
    std::vector<int> change;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        Instance& s = Inst(in_data);
        for (int i = 1; i < P_NUM; i++)
            if (want[i] >= 0 && s.hidden[i] != want[i]) change.push_back(i);
    }
    if (change.empty()) return;
    Streams st(in_data);
    Suite<AEGP_DynamicStreamSuite4> ds(in_data->pica_basicP, kAEGPDynamicStreamSuite, kAEGPDynamicStreamSuiteVersion4);
    if (!st || !ds) return;
    std::vector<int> done;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i : change) {
        AEGP_StreamRefH s = st.Get(i);
        if (!s) continue;  // a group's end has no stream
        if (!ds->AEGP_SetDynamicStreamFlag(s, AEGP_DynStreamFlag_HIDDEN, FALSE, want[i] ? TRUE : FALSE))
            done.push_back(i);
        st.Dispose(s);
    }
    Log("visibility: %d of %d rows changed in %.1f ms (instance %llu)", (int)done.size(), (int)change.size(),
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
        (unsigned long long)InstanceId(in_data));
    std::lock_guard<std::mutex> lk(g_mu);
    Instance& s = Inst(in_data);
    for (int i : done) s.hidden[i] = want[i];
}

}  // namespace

void SetHiddenRange(PF_InData* in_data, int first, int last, bool hidden)
{
    std::vector<signed char> want(P_NUM, -1);
    for (int i = first; i <= last && i < P_NUM; i++) want[i] = hidden ? 1 : 0;
    ApplyHidden(in_data, want);
}

void UpdateVisibility(PF_InData* in_data, PF_ParamDef* params[])
{
    std::vector<signed char> want(P_NUM, 0);  // everything shown unless hidden below
    // tests only: DUPLIX2_SHOW_ALL=1 shows every row (a script cannot set a hidden parameter)
    if (GetEnvironmentVariableA("DUPLIX2_SHOW_ALL", nullptr, 0) > 0) {
        ApplyHidden(in_data, want);
        return;
    }
    auto hide = [&](int a, int b) {
        for (int i = a; i <= b; i++) want[i] = 1;
    };
    // Shape Settings: the chosen Shape's rows only
    const int shape = params[P_SHAPE]->u.pd.value;
    if (shape != SHAPE_GRID) hide(P_GRID_SIZE_X, P_GRID_BRICK);
    if (shape != SHAPE_POLAR) hide(P_POLAR_INNER, P_POLAR_HEIGHT);
    if (shape != SHAPE_SPHERE) hide(P_SPHERE_RADIUS, P_SPHERE_LATITUDE);
    if (shape != SHAPE_PATH) hide(P_PATH_MASK, P_PATH_EVEN);
    // Emission: its settings only when it is on
    if (!params[P_EMIT_ON]->u.bd.value) hide(P_EMIT_START, P_EMIT_TURB_RAMP);
    // Modifiers: unused slots hidden; in a slot, the rows of its Type and Mask only
    for (int k = 0; k < kModifierSlots; k++) {
        auto m = [&](int p) { return ModIndex(k, p); };
        const int type = params[m(M_TYPE)]->u.pd.value;
        if (type == MT_NONE) {
            hide(m(M_TOPIC), m(M_TOPIC_END));
            continue;
        }
        if (type != MT_WAVE) hide(m(M_ALONG), m(M_PHASE));
        if (type != MT_WAVE && type != MT_NOISE) hide(m(M_SPEED), m(M_SPEED));
        if (type != MT_NOISE) hide(m(M_NOISE_SCALE), m(M_EVOLUTION));
        if (type != MT_RANDOM) hide(m(M_SEED), m(M_SEED));
        if (type != MT_STAGGER) hide(m(M_PROGRESS), m(M_SOFTNESS));
        if (type != MT_MAP) hide(m(M_MAP_LAYER), m(M_MAP_CHANNEL));
        const int mask = params[m(M_MASK)]->u.pd.value;
        if (mask != MM_INDEX_RANGE) hide(m(M_INDEX_FROM), m(M_INDEX_TO));
        if (mask != MM_EVERY_N) hide(m(M_EVERY_N), m(M_EVERY_N));
        if (mask != MM_FIELD) hide(m(M_FIELD_CENTER), m(M_FIELD_SOFTNESS));
        if (mask != MM_AGE_RANGE) hide(m(M_AGE_FROM), m(M_AGE_TO));
    }
    // Sources: the rows in use (+ the ones added with "+ Add Source")
    SourceInfo src[kSourceSlots];
    ReadSources(in_data, src);
    const int rows = SourceRowsShown(in_data, src);
    for (int k = rows; k < kSourceSlots; k++) want[P_SRC_LAYER + k] = 1;
    Log("visibility: shape %d, emission %d, source rows %d", shape, (int)params[P_EMIT_ON]->u.bd.value, rows);
    ApplyHidden(in_data, want);
}

}  // namespace dx2
