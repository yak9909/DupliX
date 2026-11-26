// BEE.dll / AEGPDriver.aex replacements. A layer is "ours" when its (comp item ID, layer ID) is in
// the registry with a valid snapshot; every other layer goes straight to the original function.
#include "hook/bee_hooks.h"

#include <intrin.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>

#include "core/ae_pair.h"
#include "core/ae_span.h"
#include "gpu/gpu_raster.h"
#include "core/fast_plane.h"
#include "core/scatter.h"
#include "core/world.h"
#include "effect/randamu.h"
#include "hook/bee_abi.h"
#include "hook/flatten_site.h"
#include "core/merge_geom.h"
#include "hook/merge_cache.h"
#include "hook/merge_scene.h"
#include "hook/merge_texture.h"
#include "hook/registry.h"
#include "hook/timing.h"

namespace rnd {

namespace {

typedef uintptr_t (*Fwd8)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef double (*ArtOpacityFn)(void*, void*, const void*, const void*);
typedef int (*LayerGetIdFn)(const void*);
typedef const void* (*LayerGetCompFn)(const void*);
typedef const int& (*ItemGetIdFn)(const void*);
typedef void (*M3IdentityFn)(void*);
typedef bool (*HasEffectAppliedFn)(const void*, short, void*);
typedef void* (*VirtGetPtrFn)(const void*);
typedef unsigned long long (*VirtGetU64Fn)(const void*);
typedef int (*ParadeNumSeqsFn)(void*, int*);
typedef void* (*ParadeGetEffectFn)(void*, unsigned);
typedef bool (*EffectIsActiveFn)(const void*);
typedef bool (*CompBoolFn)(const void*);

std::atomic<HookStatus> g_status{HookStatus::NotInstalled};
DWORD g_mainThread = 0;
SPBasicSuite* g_pica = nullptr;
AEGP_PluginID g_pluginId = 0;

Fwd8 g_origIsLayerType = nullptr, g_origSeparately = nullptr, g_origCount = nullptr, g_origRects = nullptr;
ArtOpacityFn g_origArtOpacity = nullptr;
LayerGetIdFn g_layerGetId = nullptr;
LayerGetCompFn g_layerGetComp = nullptr;
ItemGetIdFn g_itemGetId = nullptr;
M3IdentityFn g_m3Identity = nullptr;
HasEffectAppliedFn g_hasEffectApplied = nullptr;
int g_slotParade = -1;           // vtable slot of GetFilterParade (const)
int g_slotSwitches = -1;         // vtable slot of the switch getter (from SubLayersRenderSeparately's code)
ParadeNumSeqsFn g_paradeNum = nullptr;
ParadeGetEffectFn g_paradeGet = nullptr;
EffectIsActiveFn g_effectActive = nullptr;
CompBoolFn g_compIsCalder = nullptr, g_compIsErnst = nullptr, g_compIsEscher = nullptr, g_compIsPicasso = nullptr;
std::atomic<int> g_effectKey{-1};  // this effect's installed key (-1 = not known yet)
uintptr_t g_betaFn = 0;

bool KeyOf(const void* layer, LayerKey* k)
{
    if (!layer) return false;
    const void* comp = g_layerGetComp(layer);
    if (!comp) return false;
    k->compItemId = g_itemGetId(comp);
    k->layerId = g_layerGetId(layer);
    return true;
}

// Registered with a valid snapshot (the effect is on, the layer is 3D, the hooks may answer) and the
// layer still has this effect. AE sends no command when the effect is deleted (it keeps the instance
// for undo), so the registry alone would keep answering for a layer whose effect is gone.
bool IsMine(const void* layer)
{
    LayerKey k;
    ParamSnapshot s;
    return KeyOf(layer, &k) && RegLookup(k, &s) && s.valid && LayerIsLive(layer);
}

// ---- parameters
template <class T>
struct Suite {
    T* s = nullptr;
    const char* name;
    int32 ver;
    Suite(const char* n, int32 v) : name(n), ver(v)
    {
        if (g_pica && g_pica->AcquireSuite(n, v, (const void**)&s) != kSPNoError) s = nullptr;
    }
    ~Suite()
    {
        if (s) g_pica->ReleaseSuite(name, ver);
    }
    T* operator->() const { return s; }
    explicit operator bool() const { return s != nullptr; }
};

}  // namespace

bool ReadLayerAnchor(AEGP_LayerH L, const A_Time& t, double* x, double* y)
{
    Suite<AEGP_StreamSuite6> ss(kAEGPStreamSuite, kAEGPStreamSuiteVersion6);
    if (!ss) return false;
    AEGP_StreamRefH as = nullptr;
    if (ss->AEGP_GetNewLayerStream(g_pluginId, L, AEGP_LayerStream_ANCHORPOINT, &as) || !as) return false;
    AEGP_StreamValue2 val;
    std::memset(&val, 0, sizeof(val));
    bool ok = !ss->AEGP_GetNewStreamValue(g_pluginId, as, AEGP_LTimeMode_LayerTime, &t, FALSE, &val);
    if (ok) {
        *x = val.val.three_d.x;
        *y = val.val.three_d.y;
        ss->AEGP_DisposeStreamValue(&val);
    }
    ss->AEGP_DisposeStream(as);
    return ok;
}

bool ReadParamsFromEffect(AEGP_EffectRefH eff, AEGP_LayerH L, const A_Time& t, ParamSnapshot* out)
{
    Suite<AEGP_EffectSuite4> es(kAEGPEffectSuite, kAEGPEffectSuiteVersion4);
    Suite<AEGP_StreamSuite6> ss(kAEGPStreamSuite, kAEGPStreamSuiteVersion6);
    Suite<AEGP_LayerSuite9> ls(kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    if (!es || !ss || !ls) return false;
    bool ok = true;
    AEGP_EffectFlags flags = 0;
    AEGP_LayerFlags lflags = 0;
    if ((!es->AEGP_GetEffectFlags(eff, &flags) && !(flags & AEGP_EffectFlags_ACTIVE)) ||
        (!ls->AEGP_GetLayerFlags(L, &lflags) && !(lflags & AEGP_LayerFlag_LAYER_IS_3D))) {
        out->valid = false;
        return true;
    }
    auto one = [&](int index, double* v) {
        AEGP_StreamRefH s = nullptr;
        if (ss->AEGP_GetNewEffectStreamByIndex(g_pluginId, eff, index, &s) || !s) { ok = false; return; }
        AEGP_StreamValue2 val;
        std::memset(&val, 0, sizeof(val));
        if (!ss->AEGP_GetNewStreamValue(g_pluginId, s, AEGP_LTimeMode_LayerTime, &t, FALSE, &val)) {
            *v = val.val.one_d;
            ss->AEGP_DisposeStreamValue(&val);
        } else {
            ok = false;
        }
        ss->AEGP_DisposeStream(s);
    };
    double count = 0, seed = 0, keep = 0, fast = 0;
    ParamSnapshot r = *out;
    one(RP_COUNT, &count);
    one(RP_SEED, &seed);
    one(RP_POS_X, &r.p.pos[0]);
    one(RP_POS_Y, &r.p.pos[1]);
    one(RP_POS_Z, &r.p.pos[2]);
    one(RP_ROT_X, &r.p.rotDeg[0]);
    one(RP_ROT_Y, &r.p.rotDeg[1]);
    one(RP_ROT_Z, &r.p.rotDeg[2]);
    one(RP_SCALE_MIN, &r.p.scaleMin);
    one(RP_SCALE_MAX, &r.p.scaleMax);
    one(RP_OPACITY_MIN, &r.p.opacityMin);
    one(RP_OPACITY_MAX, &r.p.opacityMax);
    one(RP_KEEP_FIRST, &keep);
    one(RP_FAST, &fast);
    for (int k = 0; k < 3; k++) {  // AEGP gives angles in degrees, percent sliders in percent
        one(RP_WORLD_POS_X + k, &r.world.pos[k]);
        one(RP_WORLD_ROT_X + k, &r.world.rotDeg[k]);
        one(RP_WORLD_SCALE_X + k, &r.world.scale[k]);
    }
    if (!ReadLayerAnchor(L, t, &r.anchorX, &r.anchorY)) ok = false;
    if (!ok) return false;
    {
        double merge = 0;  // a failed read (an instance without it) leaves merging off
        one(RP_MERGE, &merge);
        ok = true;
        r.p.merge = merge != 0;
    }
    r.p.count = count < 0 ? 0 : (int)(count + 0.5);
    r.p.seed = (int)(seed + 0.5);
    r.p.keepFirst = keep != 0;
    r.p.fast = fast != 0;
    r.valid = !r.p.fast;  // fast mode is drawn by the effect itself: the hooks leave the layer alone
    *out = r;
    return true;
}

SPBasicSuite* HookPica() { return g_pica; }
AEGP_PluginID HookPluginId() { return g_pluginId; }

bool ReadLayerParams(AEGP_LayerH L, const A_Time& t, ParamSnapshot* out, int* effectIndex, int* otherActive)
{
    Suite<AEGP_EffectSuite4> es(kAEGPEffectSuite, kAEGPEffectSuiteVersion4);
    if (!es) return false;
    A_long n = 0;
    if (es->AEGP_GetLayerNumEffects(L, &n)) return false;
    AEGP_EffectRefH eff = nullptr;
    int index = -1, others = 0;
    for (A_long i = 0; i < n; i++) {
        AEGP_EffectRefH e = nullptr;
        if (es->AEGP_GetLayerEffectByIndex(g_pluginId, L, i, &e) || !e) continue;
        AEGP_InstalledEffectKey key = 0;
        A_char mn[AEGP_MAX_EFFECT_MATCH_NAME_SIZE + 1] = {0};
        if (!eff && !es->AEGP_GetInstalledKeyFromLayerEffect(e, &key) && !es->AEGP_GetEffectMatchName(key, mn) &&
            !std::strcmp(mn, RANDAMU_MATCH_NAME)) {
            eff = e;
            index = (int)i;
            continue;
        }
        if (otherActive) {
            AEGP_EffectFlags fl = 0;
            if (!es->AEGP_GetEffectFlags(e, &fl) && (fl & AEGP_EffectFlags_ACTIVE)) others++;
        }
        es->AEGP_DisposeEffect(e);
        if (eff && !otherActive) break;
    }
    if (effectIndex) *effectIndex = index;
    if (otherActive) *otherActive = others;
    if (!eff) {  // the effect was removed: the layer is no longer ours
        out->valid = false;
        return true;
    }
    bool ok = ReadParamsFromEffect(eff, L, t, out);
    es->AEGP_DisposeEffect(eff);
    return ok;
}

namespace {

// Finds this layer's randamu effect and reads it at layer time t. Render threads only.
// Returns false if the read failed; an absent or inactive effect gives true with out->valid = false.
bool ReadParamsAegp(const void* layer, const A_Time& t, ParamSnapshot* out)
{
    return ReadLayerParams((AEGP_LayerH)layer, t, out);
}

// ---- profiling (RANDAMU_LOG only): calls and total time per hook, logged every 2^k calls
enum ProfId { PF_SEP, PF_CNT, PF_RECTS, PF_ISTYPE, PF_OPAC, PF_AEGP, PF_N };
const char* const kProfNames[PF_N] = {"Separately", "Count", "Rects", "IsLayerType", "Opacity", "AEGP read"};
std::atomic<long long> g_profTicks[PF_N], g_profCalls[PF_N];
long long QpcNow()
{
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}
struct Prof {
    ProfId id;
    long long t0;
    explicit Prof(ProfId i) : id(i), t0(QpcNow()) {}
    ~Prof()
    {
        g_profTicks[id] += QpcNow() - t0;
        long long c = ++g_profCalls[id];
        if ((c & (c - 1)) == 0 && c >= 64) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            char buf[512];
            int n = 0;
            for (int k = 0; k < PF_N; k++)
                n += snprintf(buf + n, sizeof(buf) - n, " %s=%lld/%.1fms", kProfNames[k], g_profCalls[k].load(),
                              g_profTicks[k].load() * 1000.0 / f.QuadPart);
            RLog("prof:%s", buf);
        }
    }
};

// The parameters for `layer` at time t (t may be null). Snapshot from the effect's own commands; on
// render threads the AEGP values at t replace it.
bool ParamsFor(const void* layer, const void* tm, ParamSnapshot* out)
{
    LayerKey k;
    if (!KeyOf(layer, &k) || !RegLookup(k, out) || !out->valid) return false;
    // Thread statistics for the UI test (tests/ae/run_ui.ps1): main-thread queries use the snapshot.
    {
        static std::atomic<int> qMain{0}, qRender{0};
        bool onMain = GetCurrentThreadId() == g_mainThread;
        int m = onMain ? ++qMain : qMain.load();
        int r = onMain ? qRender.load() : ++qRender;
        int c = onMain ? m : r;
        if ((c & (c - 1)) == 0 || c % 500 == 0)
            RLog("stats: ParamsFor main=%d (snapshot) render=%d (aegp)", m, r);
    }
    if (tm && GetCurrentThreadId() != g_mainThread) {
        const int* ti = (const int*)tm;
        A_Time t = {ti[0], (A_u_long)(unsigned)ti[1]};
        if (t.scale == 0) t.scale = 1;
        // Per-thread reuse: the opacity hook asks once per copy for the same layer and time.
        struct Cached {
            bool used = false;
            LayerKey k{};
            A_long tv = 0;
            A_u_long ts = 0;
            unsigned gen = 0;
            ParamSnapshot s;
        };
        thread_local Cached cache;
        unsigned gen = RegGeneration();
        if (cache.used && cache.gen == gen && cache.k.compItemId == k.compItemId && cache.k.layerId == k.layerId &&
            cache.tv == t.value && cache.ts == t.scale) {
            *out = cache.s;
            return out->valid;
        }
        static std::atomic<int> dbg{0};
        int d = ++dbg;
        static LogLimit missLog;
        if (missLog.Take(60))
            RLog("dbg: ParamsFor miss tid %lu used %d gen %u/%u key %d/%d vs %d/%d time %d/%u vs %d/%u", GetCurrentThreadId(),
                 (int)cache.used, cache.gen, gen, cache.k.compItemId, cache.k.layerId, k.compItemId, k.layerId, (int)cache.tv,
                 (unsigned)cache.ts, (int)t.value, (unsigned)t.scale);
        if (d <= 40) RLog("dbg: ParamsFor AEGP enter comp %d layer %d", k.compItemId, k.layerId);
        bool r;
        {
            Prof pf(PF_AEGP);
            r = ReadParamsAegp(layer, t, out);
        }
        if (d <= 40) RLog("dbg: ParamsFor AEGP leave ok=%d valid=%d", (int)r, (int)out->valid);
        if (!r) {
            static std::atomic<int> warned{0};
            if (++warned <= 5) RLog("hook: AEGP param read failed (comp %d layer %d), using snapshot", k.compItemId, k.layerId);
            RegLookup(k, out);
        } else {
            cache.used = true;
            cache.k = k;
            cache.tv = t.value;
            cache.ts = t.scale;
            cache.gen = gen;
            cache.s = *out;
        }
    }
    return out->valid;
}

// ---- merge: the plan of `layer` at hook time `tm` (render threads only; s: its parameters). *lp: the layer's part
// when it has a group (otherwise null: the layer is drawn as before). The returned pointer keeps *lp alive.
std::atomic<bool> g_texHooked{false};  // both texture entry points are replaced: groups can be drawn
thread_local AEGP_DownsampleFactor t_dsf = {0, 0};  // the render's downsample as Advanced3D last asked on this thread

// The merge checkbox of a layer as the opacity hook last read it (with a frame's time, from AEGP, on any render
// thread): classic 3D asks every copy's opacity before it draws the copies. A stale value only changes who draws a
// copy (DupliX or AE), never its pixels: both are AE's arithmetic.
struct MergeSeen {
    const void* layer;
    bool merge;
};
std::mutex g_mergeSeenMu;
MergeSeen g_mergeSeen[64] = {};
int g_mergeSeenNext = 0;
void NoteMergeSeen(const void* layer, bool merge)
{
    std::lock_guard<std::mutex> lk(g_mergeSeenMu);
    for (MergeSeen& m : g_mergeSeen)
        if (m.layer == layer) {
            m.merge = merge;
            return;
        }
    g_mergeSeen[g_mergeSeenNext] = {layer, merge};
    g_mergeSeenNext = (g_mergeSeenNext + 1) % 64;
}
// 1 merge on, 0 off, -1 not seen yet
int MergeSeenOf(const void* layer)
{
    std::lock_guard<std::mutex> lk(g_mergeSeenMu);
    for (const MergeSeen& m : g_mergeSeen)
        if (m.layer == layer) return m.merge ? 1 : 0;
    return -1;
}

// The merge checkbox now means: the copies are drawn by DupliX on the GPU with AE's own arithmetic, one by one in
// AE's order (HookA3dDraw, core/ae_span). The earlier group planes (merge plans) only with RANDAMU_MERGE_OLD=1.
bool OldMerge()
{
    char v[4] = {0};
    return GetEnvironmentVariableA("RANDAMU_MERGE_OLD", v, sizeof(v)) > 0 && v[0] == '1';
}

// The group drawing (BuildIsolatedGroupPlan): RANDAMU_GROUP=0 turns it off (every copy one by one)
bool GroupDrawOn()
{
    char v[4] = {0};
    return !(GetEnvironmentVariableA("RANDAMU_GROUP", v, sizeof(v)) > 0 && v[0] == '0');
}

// The front and back groups (BuildFrontBackGroupPlan): on unless RANDAMU_GROUP_FB=0, and only while the hook that
// moves their representatives in AE's order is in (HookGraph)
std::atomic<bool> g_graphHooked{false};
}  // namespace
bool FrontBackGroupsOn()
{
    char v[4] = {0};
    return g_graphHooked.load() && !(GetEnvironmentVariableA("RANDAMU_GROUP_FB", v, sizeof(v)) > 0 && v[0] == '0');
}
namespace {

std::shared_ptr<const MergePlan> MergePlanOf(const void* layer, const void* tm, const ParamSnapshot& s, bool reread,
                                             const MergeLayerPlan** lp)
{
    *lp = nullptr;
    if (!OldMerge() && !GroupDrawOn()) return nullptr;
    if (!s.p.merge || !tm || (OldMerge() && !g_texHooked.load()) || GetCurrentThreadId() == g_mainThread) return nullptr;
    const int* ti = (const int*)tm;
    A_Time t = {ti[0], (A_u_long)(unsigned)ti[1]};
    if (t.scale == 0) t.scale = 1;
    auto plan = MergePlanFor(layer, t, reread);
    LayerKey k;
    if (!plan || !KeyOf(layer, &k)) return nullptr;
    *lp = MergeLayerOf(*plan, k.compItemId, k.layerId);
    if (*lp && (*lp)->subs.empty()) *lp = nullptr;
    return plan;
}

// The time the opacity hook last saw for each layer on this thread: the texture hook (no time argument) is called
// by the same Artisan render after it (AE may ask several layers' opacities first).
struct LastArtTime {
    LayerKey k{-1, -1};
    int t[2] = {0, 1};
};
thread_local LastArtTime t_lastArt[8];
thread_local int t_lastArtNext = 0;
void RememberArtTime(const LayerKey& k, const void* t)
{
    for (auto& e : t_lastArt)
        if (e.k.compItemId == k.compItemId && e.k.layerId == k.layerId) {
            e.t[0] = ((const int*)t)[0], e.t[1] = ((const int*)t)[1];
            return;
        }
    LastArtTime& e = t_lastArt[t_lastArtNext];
    t_lastArtNext = (t_lastArtNext + 1) % 8;
    e.k = k;
    e.t[0] = ((const int*)t)[0], e.t[1] = ((const int*)t)[1];
}
const int* ArtTimeOf(const LayerKey& k)
{
    for (auto& e : t_lastArt)
        if (e.k.compItemId == k.compItemId && e.k.layerId == k.layerId) return e.t;
    return nullptr;
}
// The time the Count hook last saw for each layer on this thread (the start of the layer's frame).
thread_local LastArtTime t_lastCount[8];
thread_local int t_lastCountNext = 0;
void RememberCountTime(const LayerKey& k, const void* t)
{
    for (auto& e : t_lastCount)
        if (e.k.compItemId == k.compItemId && e.k.layerId == k.layerId) {
            e.t[0] = ((const int*)t)[0], e.t[1] = ((const int*)t)[1];
            return;
        }
    LastArtTime& e = t_lastCount[t_lastCountNext];
    t_lastCountNext = (t_lastCountNext + 1) % 8;
    e.k = k;
    e.t[0] = ((const int*)t)[0], e.t[1] = ((const int*)t)[1];
}
const int* CountTimeOf(const LayerKey& k)
{
    for (auto& e : t_lastCount)
        if (e.k.compItemId == k.compItemId && e.k.layerId == k.layerId) return e.t;
    return nullptr;
}

// ---- per-frame timing (RANDAMU_TIMING): an event for `layer` at hook time `tm` that started at t0
void MarkTiming(const void* layer, const void* tm, TimingKind kind, long long t0)
{
    if (!TimingEnabled() || !tm) return;
    LayerKey k;
    if (!KeyOf(layer, &k)) return;
    const int* ti = (const int*)tm;
    unsigned scale = (unsigned)ti[1];
    TimingMark(k.compItemId, scale ? (double)ti[0] / scale : 0.0, kind, t0, TimingNow());
}

// ---- replacements
struct DbgScope {
    const char* n;
    bool on;
    DbgScope(const char* name, std::atomic<int>& c) : n(name), on(++c <= 30)
    {
        if (on) RLog("dbg: %s enter main=%d", n, (int)(GetCurrentThreadId() == g_mainThread));
    }
    ~DbgScope()
    {
        if (on) RLog("dbg: %s leave", n);
    }
};
std::atomic<int> g_dbgSep{0}, g_dbgCnt{0}, g_dbgRc{0}, g_dbgIs{0}, g_dbgOp{0};

uintptr_t HookSeparately(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g,
                         uintptr_t h)
{
    DbgScope ds("Separately", g_dbgSep);
    Prof pf(PF_SEP);
    if (IsMine((const void*)a)) {
        static std::atomic<int> dbgN{0};
        if (++dbgN <= 200) {
            LayerKey k{};
            KeyOf((const void*)a, &k);
            RLog("dbg: Separately mine comp %d layer %d main=%d", k.compItemId, k.layerId,
                 (int)(GetCurrentThreadId() == g_mainThread));
        }
        return 1;
    }
    return g_origSeparately(a, b, c, d, e, f, g, h);
}

// (this, const T_Time&, const shared_ptr<BEE_XformTimeCache>&, const TDB_ParamBag*)
uintptr_t HookCount(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g,
                    uintptr_t h)
{
    // Rasterize2DGraph asks with an empty cache and switches to the layer-set raster node (empty for
    // raster layers) when the answer is >= 2: give it the original answer so the whole layer is rasterised.
    DbgScope ds("Count", g_dbgCnt);
    Prof pf(PF_CNT);
    bool emptyCache = c && *(void**)c == nullptr;
    long long t0 = TimingNow();
    if (!emptyCache && IsMine((const void*)a)) {
        ParamSnapshot s;
        if (ParamsFor((const void*)a, (const void*)b, &s)) {
            MarkTiming((const void*)a, (const void*)b, TK_HOOK, t0);
            // merge: the plan is read (and rebuilt if the comp changed) here, once per frame and layer
            const MergeLayerPlan* lp = nullptr;
            auto plan = MergePlanOf((const void*)a, (const void*)b, s, true, &lp);
            LayerKey k;
            if (s.p.merge && b && KeyOf((const void*)a, &k)) RememberCountTime(k, (const void*)b);
            if (lp) return (uintptr_t)lp->subs.size();
            return (uintptr_t)(unsigned)s.p.count;
        }
        uintptr_t o = g_origCount(a, b, c, d, e, f, g, h);
        static std::atomic<int> dbgN{0};
        if (++dbgN <= 50) RLog("dbg: Count mine but params invalid -> orig %d", (int)o);
        return o;
    }
    return g_origCount(a, b, c, d, e, f, g, h);
}

// (this, M_FloatRect* ret, const T_Time&, const M_FloatRect& rect, vector<M_FloatRect>&, vector<M_Matrix3>&,
//  vector<M_Matrix4>&, const TDB_ParamBag*) -> ret
uintptr_t HookRects(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g,
                    uintptr_t h)
{
    DbgScope ds("Rects", g_dbgRc);
    Prof pf(PF_RECTS);
    long long t0 = TimingNow();
    struct MarkAtExit {
        const void* l;
        const void* tm;
        long long t0;
        bool on = false;
        ~MarkAtExit()
        {
            if (on) MarkTiming(l, tm, TK_HOOK, t0);
        }
    } mark{(const void*)a, (const void*)c, t0};
    ParamSnapshot s;
    if (!IsMine((const void*)a) || !ParamsFor((const void*)a, (const void*)c, &s))
        return g_origRects(a, b, c, d, e, f, g, h);
    mark.on = true;
    double* ret = (double*)b;
    const double* rect = (const double*)d;
    auto* vR = (bee::RawVec*)e;
    auto* v3 = (bee::RawVec*)f;
    auto* v4 = (bee::RawVec*)g;
    for (int k = 0; k < 4; k++) ret[k] = rect[k];
    // merge: individual copies and group planes (the same plan the Count hook answered with)
    const MergeLayerPlan* lp = nullptr;
    auto plan = MergePlanOf((const void*)a, (const void*)c, s, false, &lp);
    size_t n = lp ? lp->subs.size() : (size_t)s.p.count;
    {
        static LogLimit probe;  // dbg: is the render's downsample known on this thread when the planes are placed?
        if (lp && probe.Take(6)) RLog("dbg: rects with downsample (%d,%d) on this thread", (int)t_dsf.xS, (int)t_dsf.yS);
    }
    if (!bee::VecReserve(vR, bee::kRectBytes, n) || !bee::VecReserve(v3, bee::kM3Bytes, n) ||
        !bee::VecReserve(v4, bee::kM4Bytes, n))
        return b;
    double G[16];  // World Transform: copies * G * layer-to-world
    WorldM4(s.world, s.anchorX, s.anchorY, G);
    for (size_t i = 0; i < n; i++) {
        std::memcpy(vR->last, rect, bee::kRectBytes);
        vR->last += bee::kRectBytes;
        g_m3Identity(v3->last);
        v3->last += bee::kM3Bytes;
        double m[16];
        const int copy = lp ? lp->subs[i].copy : (int)i;
        if (copy >= 0) {
            CopyXform x;
            ComputeCopy(s.p, copy, s.anchorX, s.anchorY, &x);
            Mul4(x.m4, G, m);
        } else {
            const MergeCluster& cl = lp->clusters[lp->subs[i].cluster];
            // on whole pixels of this render (Advanced3D asked its downsample on this thread before the planes)
            double gr[4];
            AlignRectToDownsample(cl.rect, t_dsf.xS, t_dsf.yS, gr);
            if (!GroupPlaneM4(plan->cam, lp->in.layerToWorld, cl.depth, rect, gr, m)) {
                static LogLimit warned;
                if (warned.Take(5)) RLog("merge: group plane failed (depth %.2f)", cl.depth);
                std::memset(m, 0, sizeof(m));  // a degenerate plane draws nothing
                m[15] = 1;
            }
        }
        std::memcpy(v4->last, m, bee::kM4Bytes);
        v4->last += bee::kM4Bytes;
    }
    static std::atomic<int> logged{0};
    if (++logged <= 3) {
        LayerKey k;
        KeyOf((const void*)a, &k);
        RLog("hook: rects comp %d layer %d n=%zu anchor=(%.1f,%.1f) main=%d", k.compItemId, k.layerId, n, s.anchorX,
             s.anchorY, (int)(GetCurrentThreadId() == g_mainThread));
    }
    return b;
}

// Return addresses of Artisan's flatten check, found once at install by scanning BEE's code.
// IsLayerType is one of AE's hottest virtuals (hundreds of thousands of calls while the UI is used),
// so the hook must cost no more than an address compare for every other caller.
constexpr int kMaxSites = 4;
uintptr_t g_sites[kMaxSites] = {};
int g_numSites = 0;

int ScanFlattenSites(HMODULE mod)
{
    auto base = (const unsigned char*)mod;
    auto nt = (const IMAGE_NT_HEADERS64*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const unsigned char* p = base + sec->VirtualAddress;
        size_t n = sec->Misc.VirtualSize;
        for (size_t i = 0; i + 64 < n; i++) {
            if (p[i] != 0x84 || p[i + 1] != 0xC0 || p[i + 2] != 0x75) continue;
            if (MatchFlattenCheckSite(p + i, g_betaFn)) {
                if (g_numSites >= kMaxSites) return -1;  // ambiguous: refuse
                g_sites[g_numSites++] = (uintptr_t)(p + i);
            }
        }
    }
    return g_numSites;
}

inline bool IsFlattenSite(uintptr_t ra)
{
    for (int i = 0; i < g_numSites; i++)
        if (g_sites[i] == ra) return true;
    return false;
}

uintptr_t HookIsLayerType(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g,
                          uintptr_t h)
{
    if ((int)b == 0 && IsFlattenSite((uintptr_t)_ReturnAddress())) {
        Prof pf(PF_ISTYPE);
        if (IsMine((const void*)a)) return 0;
    }
    return g_origIsLayerType(a, b, c, d, e, f, g, h);
}

thread_local double t_lastBaseOpacity = 1.0;
double HookArtOpacity(void* ctx, void* trace, const void* t, const void* bag)
{
    DbgScope ds("ArtOpacity", g_dbgOp);
    Prof pf(PF_OPAC);
    long long t0 = TimingNow();
    double op = g_origArtOpacity(ctx, trace, t, bag);
    t_lastBaseOpacity = op;  // the group drawing builds the copies' opacities from it (AE's value times the copy's)
    if (!trace) return op;
    char* v = *(char**)((char*)trace + bee::kTraceArtLayer);
    if (!v) return op;
    const void* layer = *(void**)(v + bee::kArtLayerToLayer);
    int index = *(int*)(v + bee::kArtLayerToSubIndex);
    ParamSnapshot s;
    if (index < 0 || !IsMine(layer) || !ParamsFor(layer, t, &s)) return op;
    NoteMergeSeen(layer, s.p.merge);
    int copy = index;
    if (s.p.merge) {
        LayerKey k;
        if (t && KeyOf(layer, &k)) RememberArtTime(k, t);  // for the texture hook of this sub-layer (it has no time)
        const MergeLayerPlan* lp = nullptr;
        auto plan = MergePlanOf(layer, t, s, false, &lp);
        if (lp && index < (int)lp->subs.size()) {
            copy = lp->subs[index].copy;
            if (copy < 0) return op;  // a group plane: the copies' opacities are in its texture
        }
    }
    CopyXform x;
    ComputeCopy(s.p, copy, s.anchorX, s.anchorY, &x);
    MarkTiming(layer, t, TK_COPY, t0);
    return op * x.opacity;
}

// ---- merge: the texture AE gets for each sub-layer (AEGPDriver's AEGP_RenderTexture -> BEE). A group plane gets
// DupliX's drawing of its copies instead of the layer's texture.
typedef int (*TexFn)(void*, void*, unsigned, double*, double*, void**, double*);
typedef int (*TexRcFn)(void*, void*, unsigned, short, double*, double*, void*, void*, void**, double*);
TexFn g_origTex = nullptr;
TexRcFn g_origTexRc = nullptr;
LogLimit g_texSwaps;

void SwapGroupTexture(void* trace, void** world, double* m3, const char* which)
{
    // only the earlier group planes swap textures; reading the parameters here cost ~0.9 ms per copy (AEGP)
    if (!OldMerge()) return;
    if (!trace || !world || !*world || !m3) return;
    char* v = *(char**)((char*)trace + bee::kTraceArtLayer);
    if (!v) return;
    const void* layer = *(void**)(v + bee::kArtLayerToLayer);
    const int index = *(int*)(v + bee::kArtLayerToSubIndex);
    if (index < 0 || !IsMine(layer)) return;
    LayerKey k;
    ParamSnapshot s;
    if (!KeyOf(layer, &k)) return;
    const int* tm = ArtTimeOf(k);
    {
        // the quality hook uses the Count time: log when it is not the frame the texture is drawn for
        const int* tc = CountTimeOf(k);
        static LogLimit mism;
        if (tm && (!tc || (long long)tc[0] * tm[1] != (long long)tm[0] * tc[1]) && mism.Take(20))
            RLog("merge: count time %d/%d differs from the art time %d/%d (texture)", tc ? tc[0] : -1, tc ? tc[1] : -1, tm[0], tm[1]);
    }
    // the parameters at that time (render thread: AEGP); the registry's snapshot may hold merge off
    if (!tm || !ParamsFor(layer, tm, &s) || !s.p.merge) {
        if (!tm && ParamsFor(layer, nullptr, &s) && s.p.merge)
            RLog("merge: texture failed (%s): no time for comp %d layer %d sub %d", which, k.compItemId, k.layerId, index);
        return;
    }
    const MergeLayerPlan* lp = nullptr;
    auto plan = MergePlanOf(layer, tm, s, false, &lp);
    if (!lp || index >= (int)lp->subs.size() || lp->subs[index].cluster < 0) return;
    double out3[9];
    AEGP_WorldH w1 = RenderGroupTexture((AEGP_WorldH)*world, m3, *plan, *lp, lp->clusters[lp->subs[index].cluster], t_dsf.xS,
                                        t_dsf.yS, out3);
    if (!w1) {
        RLog("merge: texture failed (%s): drawing the group (comp %d layer %d sub %d)", which, k.compItemId, k.layerId, index);
        return;
    }
    (**(void (***)(void*, int))*world)(*world, 1);  // the layer's texture: what AEGP_DisposeTexture does
    *world = w1;
    std::memcpy(m3, out3, sizeof(out3));
    int n = 0;
    if (g_texSwaps.Take(5, true, &n)) RLog("merge: texture swapped %d (%s, comp %d layer %d sub %d)", n, which, k.compItemId, k.layerId, index);
}

// experiment (RANDAMU_A3D_DRAW set): the time AE spends getting each sub-layer's texture
LogLimit g_texTimeLog;
void LogTexTime(void* trace, const char* which, std::chrono::steady_clock::time_point t0)
{
    char ev[8] = {0};
    static const bool s_time = GetEnvironmentVariableA("RANDAMU_A3D_TIME", ev, sizeof(ev)) > 0 && ev[0] == '1';
    if ((!s_time && GetEnvironmentVariableA("RANDAMU_A3D_DRAW", ev, sizeof(ev)) <= 0) || !g_texTimeLog.Take(4000)) return;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    int sub = -9;
    if (trace) {
        char* v = *(char**)((char*)trace + bee::kTraceArtLayer);
        if (v) sub = *(int*)(v + bee::kArtLayerToSubIndex);
    }
    RLog("a3d tex time: %s sub %d %.3f ms", which, sub, ms);
}
// a new texture may have been made: the GPU copy of AE's textures (core/ae_span) is uploaded again
std::atomic<unsigned long long> g_texGen{1};
// RANDAMU_A3D_TIME=1: what each texture request asked and got (are the copies' textures the same?)
LogLimit g_texArgLog;
void LogTexArgs(void* trace, unsigned u, const double* v2, const double* rect, void** world, const double* m3)
{
    static const bool on = GetEnvironmentVariableA("RANDAMU_A3D_TIME", nullptr, 0) > 0;
    if (!on || !world || !*world || *(int*)((uintptr_t)*world + 44) < 64 || !g_texArgLog.Take(3000)) return;
    int sub = -9;
    const void* layer = nullptr;
    if (trace) {
        char* v = *(char**)((char*)trace + bee::kTraceArtLayer);
        if (v) sub = *(int*)(v + bee::kArtLayerToSubIndex), layer = *(void**)(v + bee::kArtLayerToLayer);
    }
    const uintptr_t w = world ? (uintptr_t)*world : 0;
    const unsigned char* data = w ? *(const unsigned char**)(w + 32) : nullptr;
    const int rb = w ? *(int*)(w + 40) : 0, ww = w ? *(int*)(w + 44) : 0, hh = w ? *(int*)(w + 48) : 0;
    uint64_t h = 1469598103934665603ull;
    if (data)
        for (int y = 0; y < hh; y++)
            for (int x = 0; x < 4 * ww; x += 4) h = (h ^ *(const uint32_t*)(data + (size_t)y * rb + x)) * 1099511628211ull;
    RLog("a3d tex args: layer %p sub %d u %u v2 %g %g rect %g %g %g %g -> %dx%d rb %d hash %016llx m3 %g %g %g %g %g %g %g %g %g",
         layer, sub, u, v2 ? v2[0] : -1, v2 ? v2[1] : -1, rect ? rect[0] : -1, rect ? rect[1] : -1, rect ? rect[2] : -1,
         rect ? rect[3] : -1, ww, hh, rb, (unsigned long long)h, m3 ? m3[0] : 0, m3 ? m3[1] : 0, m3 ? m3[2] : 0, m3 ? m3[3] : 0,
         m3 ? m3[4] : 0, m3 ? m3[5] : 0, m3 ? m3[6] : 0, m3 ? m3[7] : 0, m3 ? m3[8] : 0);
}
// ---- the merge checkbox: one texture per layer and frame. Classic 3D asks AEGP for each sub-layer's texture while it
// draws it (SY_TTextureFetcher, keyed by the sub-layer), and each request runs the layer's render (the effects'
// pre-render and the caches, ~1 ms in the viewer). Every copy is the same layer at the same time, so within one
// frame's drawing (HookA3dDraw, one thread) a request equal to an earlier one (same layer, hints, scale, rect) gets a
// copy of the first texture (AE disposes it like its own: AEGP_New, as the earlier group planes did).
struct SharedTex {
    const void* rc;  // the render context
    const void* layer;
    unsigned u;
    double v2[2], rect[4];
    bool hasV2, hasRect;
    AEGP_WorldH master;  // ours (a copy of AE's first texture)
    double m3[9];
};
thread_local std::vector<SharedTex> t_shared;
thread_local int t_inGroup = 0;  // inside a group's drawing (its surfaces' draws are ordinary copies)
thread_local bool t_inGroupIter = false;  // RANDAMU_GROUP_TRACE: the draw of a group's surface
thread_local int t_inA3dDraw = 0;           // inside HookA3dDraw on this thread
thread_local bool t_shareOn = false;        // the drawn surface's layer has the merge on
thread_local uintptr_t t_shareCtx = 0;      // the drawing context the shared textures belong to
std::atomic<long long> g_texShared{0}, g_texFetched{0};
LogLimit g_texShareLog;

AEGP_WorldSuite3* WorldSuite()
{
    static AEGP_WorldSuite3* ws = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        SPBasicSuite* sp = HookPica();
        if (sp && sp->AcquireSuite(kAEGPWorldSuite, kAEGPWorldSuiteVersion3, (const void**)&ws)) ws = nullptr;
    });
    return ws;
}
// a new world with src's pixels (null on failure)
AEGP_WorldH CopyOfWorld(AEGP_WorldH src)
{
    AEGP_WorldSuite3* ws = WorldSuite();
    if (!ws || !src) return nullptr;
    AEGP_WorldType type = AEGP_WorldType_NONE;
    A_long w = 0, h = 0;
    A_u_long rb = 0, orb = 0;
    if (ws->AEGP_GetType(src, &type) || ws->AEGP_GetSize(src, &w, &h) || ws->AEGP_GetRowBytes(src, &rb) || w <= 0 || h <= 0)
        return nullptr;
    const int bpp = type == AEGP_WorldType_8 ? 4 : type == AEGP_WorldType_16 ? 8 : type == AEGP_WorldType_32 ? 16 : 0;
    if (!bpp) return nullptr;
    AEGP_WorldH out = nullptr;
    if (ws->AEGP_New(HookPluginId(), type, w, h, &out) || !out) return nullptr;
    auto base = [&](AEGP_WorldH wh) -> unsigned char* {
        if (type == AEGP_WorldType_8) {
            PF_Pixel8* p = nullptr;
            return ws->AEGP_GetBaseAddr8(wh, &p) ? nullptr : (unsigned char*)p;
        }
        if (type == AEGP_WorldType_16) {
            PF_Pixel16* p = nullptr;
            return ws->AEGP_GetBaseAddr16(wh, &p) ? nullptr : (unsigned char*)p;
        }
        PF_PixelFloat* p = nullptr;
        return ws->AEGP_GetBaseAddr32(wh, &p) ? nullptr : (unsigned char*)p;
    };
    const unsigned char* s = base(src);
    unsigned char* d = base(out);
    if (!s || !d || ws->AEGP_GetRowBytes(out, &orb)) {
        ws->AEGP_Dispose(out);
        return nullptr;
    }
    for (A_long y = 0; y < h; y++) std::memcpy(d + (size_t)y * orb, s + (size_t)y * rb, (size_t)w * bpp);
    return out;
}
void DropSharedTextures()
{
    AEGP_WorldSuite3* ws = WorldSuite();
    for (SharedTex& s : t_shared)
        if (ws && s.master) ws->AEGP_Dispose(s.master);
    t_shared.clear();
}
bool SameRequest(const SharedTex& s, const void* rc, const void* layer, unsigned u, const double* v2, const double* rect)
{
    if (s.rc != rc || s.layer != layer || s.u != u || s.hasV2 != (v2 != nullptr) || s.hasRect != (rect != nullptr)) return false;
    if (v2 && (s.v2[0] != v2[0] || s.v2[1] != v2[1])) return false;
    if (rect)
        for (int k = 0; k < 4; k++)
            if (s.rect[k] != rect[k]) return false;
    return true;
}

int HookTex(void* ctx, void* trace, unsigned u, double* v2, double* rect, void** world, double* m3)
{
    static const bool s_gtr = GetEnvironmentVariableA("RANDAMU_GROUP_TRACE", nullptr, 0) > 0;
    if (s_gtr && t_inGroupIter) RLog("group trace: texture request (shared %zu)", t_shared.size());
    const auto t0 = std::chrono::steady_clock::now();
    const void* shareLayer = nullptr;
    if (t_inA3dDraw && t_shareOn && trace && world && m3) {
        char* v = *(char**)((char*)trace + bee::kTraceArtLayer);
        const void* layer = v ? *(void**)(v + bee::kArtLayerToLayer) : nullptr;
        if (layer && *(int*)(v + bee::kArtLayerToSubIndex) >= 0 && IsMine(layer)) {
            shareLayer = layer;
            for (const SharedTex& s : t_shared)
                if (SameRequest(s, ctx, layer, u, v2, rect)) {
                    AEGP_WorldH c = CopyOfWorld(s.master);
                    if (!c) break;
                    *world = (void*)c;
                    std::memcpy(m3, s.m3, sizeof(s.m3));
                    g_texGen++;  // a new world (maybe at a freed one's address): the GPU copy is uploaded again
                    int n = 0;
                    if (g_texShareLog.Take(3, true, &n))
                        RLog("merge: texture shared %lld (fetched %lld)", ++g_texShared, g_texFetched.load());
                    else
                        ++g_texShared;
                    return 0;
                }
        }
    }
    int r = g_origTex(ctx, trace, u, v2, rect, world, m3);
    if (shareLayer && !r && *world && t_shared.size() < 16) {
        SharedTex s = {};
        s.rc = ctx, s.layer = shareLayer, s.u = u, s.hasV2 = v2 != nullptr, s.hasRect = rect != nullptr;
        if (v2) s.v2[0] = v2[0], s.v2[1] = v2[1];
        if (rect) std::memcpy(s.rect, rect, sizeof(s.rect));
        std::memcpy(s.m3, m3, sizeof(s.m3));
        s.master = CopyOfWorld((AEGP_WorldH)*world);
        if (s.master) t_shared.push_back(s), ++g_texFetched;
    }
    g_texGen++;
    LogTexTime(trace, "tex", t0);
    if (!r) LogTexArgs(trace, u, v2, rect, world, m3);
    if (!r) SwapGroupTexture(trace, world, m3, "tex");
    return r;
}
int HookTexRc(void* ctx, void* trace, unsigned u, short sh, double* v2, double* rect, void* opts, void* mrect, void** world,
              double* m3)
{
    const auto t0 = std::chrono::steady_clock::now();
    int r = g_origTexRc(ctx, trace, u, sh, v2, rect, opts, mrect, world, m3);
    g_texGen++;
    LogTexTime(trace, "texrc", t0);
    if (!r) SwapGroupTexture(trace, world, m3, "texrc");
    return r;
}

// ---- merge: classic 3D (Advanced3D.aex) asks the layer's quality through
// AEGP_LayerSuite::AEGP_GetLayerQuality. A group plane lies 1:1 on the screen pixels, so drawing it in draft
// (nearest sampling) gives the same pixels without the per-pixel interpolation. Answer draft to Advanced3D only,
// and only for a merge-on layer whose sub-layers at the last art time are all group planes.
bool WritePtr(void** where, void* value);
typedef A_Err (*GetLayerQualityFn)(AEGP_LayerH, AEGP_LayerQuality*);
GetLayerQualityFn g_origGetQuality = nullptr;
LogLimit g_qCalls, g_qDraft, g_qNoTime;
uintptr_t g_a3dLo = 0, g_a3dHi = 0;

bool AllGroups(const MergeLayerPlan& lp)
{
    for (const auto& s : lp.subs)
        if (s.cluster < 0) return false;
    return !lp.subs.empty();
}

A_Err HookGetLayerQuality(AEGP_LayerH L, AEGP_LayerQuality* q)
{
    A_Err err = g_origGetQuality(L, q);
    uintptr_t ra = (uintptr_t)_ReturnAddress();
    if (err || !q || ra < g_a3dLo || ra >= g_a3dHi || GetCurrentThreadId() == g_mainThread) return err;
    const void* layer = (const void*)L;
    if (!IsMine(layer) || !OldMerge()) return err;  // draft only for the earlier group planes (their own textures)
    int n = 0;
    const bool firstCalls = g_qCalls.Take(8, false, &n);
    LayerKey k;
    ParamSnapshot s;
    if (!KeyOf(layer, &k)) return err;
    // the frame's time: the Count hook of the same layer on this thread (Count is asked when the layer's frame
    // starts, the opacities only after the quality)
    const int* tm = CountTimeOf(k);
    if (!tm) {
        int m = 0;
        if (g_qNoTime.Take(5, true, &m)) RLog("merge: quality asked without a count time (%d of %d calls)", m, n);
        return err;
    }
    if (firstCalls) {
        const int* ta = ArtTimeOf(k);
        RLog("merge: quality call %d: count time %d/%d, last art time %d/%d", n, tm[0], tm[1], ta ? ta[0] : -1, ta ? ta[1] : -1);
    }
    if (!ParamsFor(layer, tm, &s) || !s.p.merge) return err;
    const MergeLayerPlan* lp = nullptr;
    auto plan = MergePlanOf(layer, tm, s, false, &lp);
    if (!lp || !AllGroups(*lp)) return err;
    // diagnosis / tests: RANDAMU_MERGE_DRAFT=0 keeps the layer's own quality (read per call: test scripts in AE
    // switch it between renders)
    char v[8] = {0};
    if (GetEnvironmentVariableA("RANDAMU_MERGE_DRAFT", v, sizeof(v)) > 0 && v[0] == '0') return err;
    *q = AEGP_LayerQual_DRAFT;
    int d = 0;
    if (g_qDraft.Take(5, true, &d)) RLog("merge: draft quality answered %d (calls %d, comp %d layer %d)", d, n, k.compItemId, k.layerId);
    return err;
}

// ---- merge: the render's downsample factor. Advanced3D asks AEGP_CanvasSuite::AEGP_GetRenderDownsampleFactor;
// the answer is remembered per thread for the hooks of the same render (Rects, texture).
typedef A_Err (*GetDsfFn)(PR_RenderContextH, AEGP_DownsampleFactor*);
GetDsfFn g_origGetDsf = nullptr;
LogLimit g_dsfLog;

A_Err HookGetDsf(PR_RenderContextH ctx, AEGP_DownsampleFactor* f)
{
    A_Err err = g_origGetDsf(ctx, f);
    if (!err && f) {
        t_dsf = *f;
        if (g_dsfLog.Take(6)) RLog("dbg: render downsample (%d,%d) on this thread", (int)f->xS, (int)f->yS);
    }
    return err;
}

// ---- the merge checkbox: one layer-bounds answer per layer and pass. Before drawing, Advanced3D (sub_180008710) asks
// AEGP_GetRenderLayerBounds for every sub-layer, and each ask runs the layer's pre-render (the effects' bounds, ~0.2
// ms each in the viewer). The copies are the same layer at the same time: within one pass (from its
// AEGP_GetNumLayersToRender, on one thread) a later sub-layer of a layer with the merge on gets the first answer.
std::atomic<DWORD> g_profTid{0};  // see ProfLoop
// RANDAMU_A3D_PROF: 1 a copy's setup, 2 the drawing thread all the time, 3 a pass (its AEGP_GetNumLayersToRender
// to its last surface drawn)
int g_profMode = 0;
typedef A_Err (*GetNumLayersFn)(PR_RenderContextH, A_long*);
typedef A_Err (*GetBoundsFn)(PR_RenderContextH, AEGP_RenderLayerContextH, const A_Time*, A_LegacyRect*);
GetNumLayersFn g_origGetNumLayers = nullptr;
GetBoundsFn g_origGetBounds = nullptr;
AEGP_CanvasSuite8* g_canvas8 = nullptr;
struct SharedBounds {
    PR_RenderContextH rc;
    AEGP_LayerH layer;
    A_Time t;
    A_LegacyRect r;
};
thread_local std::vector<SharedBounds> t_bounds;
struct SharedStream {  // see HookLayerStreamValue
    AEGP_LayerH layer;
    AEGP_LayerStream which;
    AEGP_LTimeMode mode;
    A_Time t;
    A_Boolean pre;
    AEGP_StreamVal2 v;
    AEGP_StreamType type;
};
thread_local std::vector<SharedStream> t_streams;
thread_local bool t_passOpen = false;  // between a pass's AEGP_GetNumLayersToRender and its last surface drawn
// RANDAMU_A3D_TIME=1: a pass's preparation (its start to its first surface drawn) and drawing (to its last)
thread_local std::chrono::steady_clock::time_point t_passStart, t_passFirstDraw;
thread_local int t_passDraws = 0;
// the group drawing's depth-bias indices in a pass (HookGetLayerIndex)
thread_local int t_biasShift = 0, t_biasLayerSubs = 0, t_biasBase = 0;
thread_local const void* t_biasLayer = nullptr;
void ClosePass()
{
    t_biasShift = 0, t_biasLayerSubs = 0, t_biasBase = 0, t_biasLayer = nullptr;
    static const bool s_time = GetEnvironmentVariableA("RANDAMU_A3D_TIME", nullptr, 0) > 0;
    if (s_time && t_passOpen && t_passDraws > 0) {
        const auto now = std::chrono::steady_clock::now();
        static LogLimit lg;
        if (lg.Take(400))
            RLog("pass time: prep %.2f ms draw %.2f ms surfaces %d",
                 std::chrono::duration<double, std::milli>(t_passFirstDraw - t_passStart).count(),
                 std::chrono::duration<double, std::milli>(now - t_passFirstDraw).count(), t_passDraws);
    }
    t_passDraws = 0;
    t_bounds.clear();
    t_streams.clear();
    t_passOpen = false;
}
std::atomic<long long> g_boundsShared{0}, g_boundsAsked{0};
LogLimit g_boundsLog, g_boundsCheckLog;

A_Err HookGetNumLayers(PR_RenderContextH rc, A_long* n)
{
    ClosePass();  // a new pass
    t_passOpen = true;
    t_passStart = std::chrono::steady_clock::now();
    if (g_profMode == 3) g_profTid = GetCurrentThreadId();  // RANDAMU_A3D_PROF=3: from the pass's start to its last draw
    return g_origGetNumLayers(rc, n);
}

bool MergeOnFor(const void* L)
{
    if (!L || !IsMine(L) || OldMerge()) return false;
    int seen = MergeSeenOf(L);
    if (seen < 0) {
        ParamSnapshot s;
        seen = ParamsFor(L, nullptr, &s) && s.p.merge ? 1 : 0;
    }
    return seen > 0;
}

A_Err HookGetBounds(PR_RenderContextH rc, AEGP_RenderLayerContextH lc, const A_Time* t, A_LegacyRect* r)
{
    static const bool s_time = GetEnvironmentVariableA("RANDAMU_A3D_TIME", nullptr, 0) > 0;
    // RANDAMU_BOUNDS_SHARE=0 (diagnostics): never share; with RANDAMU_A3D_TIME every answer of a merged layer is
    // compared with the first one of its pass
    static const bool s_noShare = [] {
        char v[4] = {0};
        return GetEnvironmentVariableA("RANDAMU_BOUNDS_SHARE", v, sizeof(v)) > 0 && v[0] == '0';
    }();
    AEGP_LayerH L = nullptr;
    AEGP_SubLayerIndex sub = -1;
    if (!t || !r || !g_canvas8 || g_canvas8->AEGP_GetLayerAndSubLayerFromLayerContext(rc, lc, &L, &sub) || !L || !MergeOnFor(L))
        return g_origGetBounds(rc, lc, t, r);
    for (const SharedBounds& s : t_bounds)
        if (s.rc == rc && s.layer == L && s.t.value == t->value && s.t.scale == t->scale) {
            if (!s_noShare) {
                *r = s.r;
                int n = 0;
                if (g_boundsLog.Take(3, true, &n)) RLog("merge: layer bounds shared %lld (asked %lld)", ++g_boundsShared, g_boundsAsked.load());
                else ++g_boundsShared;
                return A_Err_NONE;
            }
            const auto t0 = std::chrono::steady_clock::now();
            const A_Err e = g_origGetBounds(rc, lc, t, r);
            if (s_time && g_boundsCheckLog.Take(2000))
                RLog("merge: bounds sub %d %s (%d %d %d %d vs first %d %d %d %d) %.3f ms", (int)sub,
                     !e && !std::memcmp(r, &s.r, sizeof(*r)) ? "same" : "DIFFERENT", r->top, r->left, r->bottom, r->right,
                     s.r.top, s.r.left, s.r.bottom, s.r.right,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            return e;
        }
    const A_Err e = g_origGetBounds(rc, lc, t, r);
    ++g_boundsAsked;
    if (!e && t_bounds.size() < 64) t_bounds.push_back({rc, L, *t, *r});
    return e;
}

// ---- the merge checkbox: one stream value per layer, stream and time in a pass. Advanced3D reads the layer's scale
// and material options (AEGP_GetLayerStreamValue, its Stream Suite version 7) for every sub-layer; each read
// evaluates the layer's property (keyframes, expressions). Within a pass (see HookGetNumLayers) a later read of
// the same layer, stream, time mode, time and pre-expression flag of a layer with the merge on gets the first value.
typedef A_Err (*GetLayerStreamValueFn)(AEGP_LayerH, AEGP_LayerStream, AEGP_LTimeMode, const A_Time*, A_Boolean,
                                       AEGP_StreamVal2*, AEGP_StreamType*);
GetLayerStreamValueFn g_origLayerStreamValue = nullptr;
static_assert(sizeof(AEGP_StreamVal2) == 32, "Advanced3D reads 32 bytes of the value");
std::atomic<long long> g_streamShared{0};
LogLimit g_streamLog, g_streamCheckLog;

A_Err HookLayerStreamValue(AEGP_LayerH L, AEGP_LayerStream which, AEGP_LTimeMode mode, const A_Time* t, A_Boolean pre,
                           AEGP_StreamVal2* v, AEGP_StreamType* type)
{
    {
        // RANDAMU_A3D_TIME=1: why reads are not shared (0 no pass, 1 not merged, 2 first of its kind, 3 shared)
        static const bool s_time = GetEnvironmentVariableA("RANDAMU_A3D_TIME", nullptr, 0) > 0;
        static std::atomic<long long> why[4];
        if (s_time) {
            const int k = !t_passOpen ? 0 : !MergeOnFor(L) ? 1 : 2;
            const long long c = ++why[k];
            static LogLimit nm;
            if (k == 1 && nm.Take(12)) {
                LayerKey lk = {};
                const bool key = KeyOf(L, &lk);
                RLog("merge: stream read not merged: layer %p key %d (%d/%d) mine %d seen %d stream %d", (void*)L, (int)key,
                     lk.compItemId, lk.layerId, (int)IsMine(L), MergeSeenOf(L), (int)which);
            }
            if ((c & (c - 1)) == 0 && c >= 64)
                RLog("merge: stream reads no-pass %lld not-merged %lld kept %lld (stream %d, time %d/%u)", why[0].load(),
                     why[1].load(), why[2].load(), (int)which, t ? (int)t->value : -1, t ? (unsigned)t->scale : 0u);
        }
    }
    if (!t_passOpen || !t || !v || !MergeOnFor(L)) return g_origLayerStreamValue(L, which, mode, t, pre, v, type);
    static const bool s_check = [] {
        char e[4] = {0};
        return GetEnvironmentVariableA("RANDAMU_BOUNDS_SHARE", e, sizeof(e)) > 0 && e[0] == '0';
    }();
    for (const SharedStream& s : t_streams)
        if (s.layer == L && s.which == which && s.mode == mode && s.t.value == t->value && s.t.scale == t->scale && s.pre == pre) {
            if (s_check) {  // RANDAMU_BOUNDS_SHARE=0: compare instead of sharing
                const A_Err e = g_origLayerStreamValue(L, which, mode, t, pre, v, type);
                if (g_streamCheckLog.Take(2000))
                    RLog("merge: stream %d %s", (int)which, !e && !std::memcmp(v, &s.v, sizeof(*v)) ? "same" : "DIFFERENT");
                return e;
            }
            *v = s.v;
            if (type) *type = s.type;
            int n = 0;
            if (g_streamLog.Take(3, true, &n)) RLog("merge: stream value shared %lld", ++g_streamShared);
            else ++g_streamShared;
            return A_Err_NONE;
        }
    AEGP_StreamType tp = AEGP_StreamType_NO_DATA;
    const A_Err e = g_origLayerStreamValue(L, which, mode, t, pre, v, &tp);
    if (type) *type = tp;
    if (!e && t_streams.size() < 256) t_streams.push_back({L, which, mode, *t, pre, *v, tp});
    return e;
}

void InstallStreamHook()
{
    void** t7 = nullptr;
    AEGP_StreamSuite6* s6 = nullptr;
    if (!g_pica || g_pica->AcquireSuite(kAEGPStreamSuite, 7, (const void**)&t7) || !t7 ||
        g_pica->AcquireSuite(kAEGPStreamSuite, kAEGPStreamSuiteVersion6, (const void**)&s6) || !s6) {
        RLog("merge: stream hook not installed (suites)");
        return;
    }
    // slot 16 (+128) is AEGP_GetLayerStreamValue: the SDK's table has it there, and Advanced3D's sub_18000ABB0 calls
    // version 7's +128 with its arguments (layer, stream, time mode, time, pre-expression, value, type). Version 7's
    // entry is its own implementation (an older value type of the same 32 bytes), so the values are copied as is.
    (void)s6;
    g_origLayerStreamValue = (GetLayerStreamValueFn)t7[16];
    const bool ok = WritePtr(&t7[16], (void*)&HookLayerStreamValue);
    RLog("merge: stream hook %s", ok ? "installed" : "failed");
}

// ---- RANDAMU_XF_CHECK=1 (diagnostics, the group drawing's inputs): the world matrix AE gets for a copy
// (AEGP_GetRenderLayerToWorldXform) compared bit by bit with the copy's matrix (as the Rects hook gave it) times
// the layer's world matrix (AEGP_GetLayerToWorldXform), both orders, multiplied with AE's M_Matrix4::operator*= (U.dll)
typedef A_Err (*GetRenderXformFn)(PR_RenderContextH, AEGP_RenderLayerContextH, const A_Time*, A_Matrix4*);
typedef void* (*M4MulFn)(void*, const void*);
GetRenderXformFn g_origRenderXform = nullptr;
M4MulFn g_m4Mul = nullptr;
typedef void (*M4ScaleFn)(void*, double, double, double);
M4ScaleFn g_m4Scale = nullptr;
LogLimit g_xfLog;
std::atomic<long long> g_xfN{0}, g_xfEq1{0}, g_xfEq2{0};

A_Err HookRenderXform(PR_RenderContextH rc, AEGP_RenderLayerContextH lc, const A_Time* t, A_Matrix4* out)
{
    const A_Err e = g_origRenderXform(rc, lc, t, out);
    AEGP_LayerH L = nullptr;
    AEGP_SubLayerIndex sub = -1;
    if (e || !t || !out || !g_m4Mul || !g_canvas8 || g_canvas8->AEGP_GetLayerAndSubLayerFromLayerContext(rc, lc, &L, &sub) ||
        !L || sub < 0 || !IsMine(L))
        return e;
    ParamSnapshot s;
    const int tm[2] = {(int)t->value, (int)t->scale};
    if (!ParamsFor(L, tm, &s)) return e;
    Suite<AEGP_LayerSuite9> ls(kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    A_Matrix4 B;
    if (!ls || ls->AEGP_GetLayerToWorldXform(L, t, &B)) return e;
    CopyXform x;
    ComputeCopy(s.p, (int)sub, s.anchorX, s.anchorY, &x);
    double G[16], M[16];
    WorldM4(s.world, s.anchorX, s.anchorY, G);
    Mul4(x.m4, G, M);
    double c1[16], c2[16];
    std::memcpy(c1, M, sizeof(c1));
    g_m4Mul(c1, &B);
    std::memcpy(c2, &B, sizeof(c2));
    g_m4Mul(c2, M);
    const bool eq1 = !std::memcmp(c1, out, sizeof(c1)), eq2 = !std::memcmp(c2, out, sizeof(c2));
    double d1 = 0, d2 = 0;
    for (int k = 0; k < 16; k++)
        d1 = std::max(d1, std::fabs(c1[k] - (&out->mat[0][0])[k])), d2 = std::max(d2, std::fabs(c2[k] - (&out->mat[0][0])[k]));
    const long long n = ++g_xfN, a = eq1 ? ++g_xfEq1 : g_xfEq1.load(), b = eq2 ? ++g_xfEq2 : g_xfEq2.load();
    if (g_xfLog.Take(20, true))
        RLog("xf check: sub %d M*B %s (max %.3g) B*M %s (max %.3g); so far %lld: %lld / %lld equal", (int)sub, eq1 ? "EQUAL" : "differs",
             d1, eq2 ? "EQUAL" : "differs", d2, n, a, b);
    return e;
}

void InstallBoundsHook(void** t10)
{
    // the v10 table's slots of the two functions, found by comparing with the SDK version's table
    if (!g_canvas8) return;
    int sn = -1, sb = -1;
    for (int i = 0; i < 60; i++) {
        if (t10[i] == (void*)g_canvas8->AEGP_GetNumLayersToRender && sn < 0) sn = i;
        if (t10[i] == (void*)g_canvas8->AEGP_GetRenderLayerBounds && sb < 0) sb = i;
    }
    if (sn < 0 || sb < 0) {
        RLog("merge: bounds hook not installed (slots %d %d)", sn, sb);
        return;
    }
    g_origGetNumLayers = (GetNumLayersFn)t10[sn];
    g_origGetBounds = (GetBoundsFn)t10[sb];
    const bool ok = WritePtr(&t10[sn], (void*)&HookGetNumLayers) && WritePtr(&t10[sb], (void*)&HookGetBounds);
    RLog("merge: bounds hook %s (v10 slots %d %d)", ok ? "installed" : "failed", sn, sb);
    char ev[4] = {0};
    {
        HMODULE u = GetModuleHandleA("U.dll");
        g_m4Mul = u ? (M4MulFn)GetProcAddress(u, "??XM_Matrix4@@QEAAAEAV0@AEBV0@@Z") : nullptr;
        g_m4Scale = u ? (M4ScaleFn)GetProcAddress(u, "?Scale@M_Matrix4@@QEAAXNNN@Z") : nullptr;
        RLog("merge: U.dll matrix functions %p %p", (void*)g_m4Mul, (void*)g_m4Scale);
    }
    if (GetEnvironmentVariableA("RANDAMU_XF_CHECK", ev, sizeof(ev)) > 0 && ev[0] == '1') {
        int sx = -1;
        for (int i = 0; i < 60; i++)
            if (t10[i] == (void*)g_canvas8->AEGP_GetRenderLayerToWorldXform) { sx = i; break; }
        if (sx >= 0) g_origRenderXform = (GetRenderXformFn)t10[sx], WritePtr(&t10[sx], (void*)&HookRenderXform);
        RLog("xf check: hook slot %d, M_Matrix4::operator*= %p", sx, (void*)g_m4Mul);
    }
}

void InstallDownsampleHook(uintptr_t a3dLo, uintptr_t a3dHi)
{
    (void)a3dLo, (void)a3dHi;
    // Advanced3D acquires the canvas suite with version 10: find its table's slot of the function by comparing
    // with the SDK version's table (the same implementation)
    void** t10 = nullptr;
    AEGP_CanvasSuite8* t14 = nullptr;
    if (!g_pica || g_pica->AcquireSuite(kAEGPCanvasSuite, 10, (const void**)&t10) || !t10 ||
        g_pica->AcquireSuite(kAEGPCanvasSuite, kAEGPCanvasSuiteVersion8, (const void**)&t14) || !t14) {
        RLog("merge: downsample hook not installed (suites)");
        return;
    }
    g_canvas8 = t14;
    InstallBoundsHook(t10);
    InstallStreamHook();
    void* fn = (void*)t14->AEGP_GetRenderDownsampleFactor;
    int slot = -1;
    for (int i = 0; i < 40; i++)
        if (t10[i] == fn) { slot = i; break; }
    if (slot < 0) {
        RLog("merge: downsample hook not installed (slot not found)");
        return;
    }
    g_origGetDsf = (GetDsfFn)t10[slot];
    bool ok = WritePtr(&t10[slot], (void*)&HookGetDsf);
    // the SDK version's table too (other artisans may use it)
    if (ok && (void*)t14->AEGP_GetRenderDownsampleFactor == fn)
        ok = WritePtr((void**)&t14->AEGP_GetRenderDownsampleFactor, (void*)&HookGetDsf);
    RLog("merge: downsample hook %s (v10 slot %d)", ok ? "installed" : "failed", slot);
}

// ---- experiment (11/26): classic 3D draws each surface (SY_TRLayer, 232 bytes, an element of Advanced3D's surface
// vector) through its vtable slot 1, in the order of its own graph (sub_180014470 -> sub_180014800). The surface's
// int at +224 is the index of its layer in the render (BEE_GetArtisanNthLayerToRender -> the render trace -> the
// art layer -> the layer and the sub-layer index). RANDAMU_A3D_DRAW: 1 log every surface drawn, 2 also skip the
// copies (to see that AE's frame is left without them).
namespace a3d {
constexpr uintptr_t kTRLayerVtRva = 0x80438, kTRLayerSlot0Rva = 0x11240, kTRLayerDrawRva = 0x12B30;
constexpr ptrdiff_t kSurfLayerIndex = 224;
constexpr ptrdiff_t kCtxRenderContext = 8, kCtxProgressCount = 252, kCtxProgressTotal = 248, kCtxProgress = 208;
}  // namespace a3d
// ---- the group drawing's building blocks (11/26): a copy's surface built by Advanced3D's own functions from a
// layer entry (the 192-byte record sub_180008710 makes per sub-layer, in ctx[0] + 432) whose world matrices are the
// copy's. sub_180013160 initializes a SY_TRLayer, then sub_1800159A0 fills it from the entry and the camera.
namespace a3d {
constexpr uintptr_t kBuildSurfaceRva = 0x159A0, kSurfaceLightsRva = 0x15340, kSurfInit1Rva = 0x805E0,
                    kSurfInit2Rva = 0x80600;
constexpr ptrdiff_t kPrepEntries = 432, kPrepEntryCount = 440, kEntrySize = 192, kSurfaceSize = 232;
}  // namespace a3d
typedef bool (*BuildSurfaceFn)(uintptr_t entry, double zBias, uintptr_t entry2, uintptr_t cam, unsigned char flag,
                               uintptr_t motion, double zero, const double* viewRect, uintptr_t zero2, unsigned char one,
                               uintptr_t surface);
typedef void (*SurfaceLightsFn)(uintptr_t entry, uintptr_t lights, uintptr_t surface);

// A copy's layer entry: the template's (the same layer: scale, bounds, material, lights) with the copy's world matrix
// (the copy's matrix times the layer's, as BEE makes it: checked equal with RANDAMU_XF_CHECK), scaled as sub_180008710
// does (M_Matrix4::Scale(s, 1, 1)), the copy's opacity (AE's layer opacity times the copy's) and the index its depth
// bias is made from (+0x34: index * 1e-7 in sub_180013160). +0x30 stays the template's: the render layer the
// surface's texture and draw parameters are fetched for (the surface's +56).
// m4 must hold 128 bytes; false: not built (the caller draws as before).
bool CopyEntry(uintptr_t tmpl, const double copyM4[16], const A_Matrix4& layerW, double scaleX, double opacity, int index,
               unsigned char* out, double* m4)
{
    if (!g_m4Mul || !g_m4Scale || *(int*)(tmpl + 8) != 1) return false;  // one motion sample only
    std::memcpy(out, (const void*)tmpl, a3d::kEntrySize);
    std::memcpy(m4, copyM4, 128);
    g_m4Mul(m4, &layerW);
    g_m4Scale(m4, scaleX, 1.0, 1.0);
    *(double**)out = m4;
    *(double*)(out + 0x40) = opacity;
    *(int*)(out + 0x34) = index;
    return true;
}

// Initializes surface (232 bytes) as sub_180013160 does and fills it from entry (index: the entry's render index).
// False: AE culls it (nothing to draw); the surface holds nothing to free then.
bool BuildSurface(uintptr_t ctx, uintptr_t entry, int index, unsigned char* s)
{
    std::memset(s, 0, a3d::kSurfaceSize);
    *(uintptr_t*)s = g_a3dLo + a3d::kTRLayerVtRva;
    std::memcpy(s + 0x08, (const void*)(g_a3dLo + a3d::kSurfInit1Rva), 16);
    std::memcpy(s + 0x18, (const void*)(g_a3dLo + a3d::kSurfInit2Rva), 16);
    std::memcpy(s + 0x28, (const void*)(g_a3dLo + a3d::kSurfInit1Rva), 8);
    std::memcpy(s + 0x30, (const void*)(g_a3dLo + a3d::kSurfInit2Rva), 8);
    std::memcpy(s + 0x38, (const void*)(entry + 0x30), 0x60);
    *(uint64_t*)(s + 0x98) = *(const uint64_t*)(entry + 0x90);
    *(uint64_t*)(s + 0xC0) = *(const uint64_t*)(entry + 0xB8);
    *(int*)(s + 0xE0) = index;
    const uintptr_t cam = ctx + (*(const unsigned char*)(entry + 0x7F) ? 0x38 : 0x48);
    const double view[4] = {0.0, 0.0, (double)*(const int*)(ctx + 0xC0), (double)*(const int*)(ctx + 0xC4)};
    const double zBias = (double)*(const int*)(entry + 0x34) * 1e-7;  // qword_180080558
    const auto build = (BuildSurfaceFn)(g_a3dLo + a3d::kBuildSurfaceRva);
    if (!build(entry, zBias, entry, cam, *(const unsigned char*)(ctx + 0x68), *(uintptr_t*)ctx + 0x128, 0.0, view, 0, 1,
               (uintptr_t)s)) {
        (**(void (***)(void*, int))s)(s, 0);  // what sub_180013160 does with a culled surface
        return false;
    }
    ((SurfaceLightsFn)(g_a3dLo + a3d::kSurfaceLightsRva))(entry, ctx + 0x28, (uintptr_t)s);
    return true;
}
void FreeSurface(unsigned char* s) { (**(void (***)(void*, int))s)(s, 0); }

// RANDAMU_GROUP_CLONE=1 (diagnostics): every copy drawn by DupliX is drawn from a surface rebuilt as above; the rebuilt
// surface is compared with AE's (its 232 bytes and its sample records) and the differences are logged
LogLimit g_cloneLog;
std::atomic<long long> g_cloneN{0}, g_cloneSame{0};
bool CloneForCheck(uintptr_t self, uintptr_t ctx, const void* L, int sub, unsigned char* clone, unsigned char* entryBuf,
                   double* m4)
{
    const uintptr_t prep = *(uintptr_t*)ctx;
    const int idx = *(int*)(self + a3d::kSurfLayerIndex);
    if (!prep || idx < 0 || idx >= *(int*)(prep + a3d::kPrepEntryCount)) return false;
    const uintptr_t entry = *(uintptr_t*)(prep + a3d::kPrepEntries) + (uintptr_t)idx * a3d::kEntrySize;
    A_Time t = {0, 1}, step = {0, 1};
    void* rc = *(void**)(ctx + a3d::kCtxRenderContext);
    if (!g_canvas8 || g_canvas8->AEGP_GetCompRenderTime((PR_RenderContextH)rc, &t, &step)) return false;
    ParamSnapshot s;
    const int tm[2] = {(int)t.value, (int)t.scale};
    Suite<AEGP_LayerSuite9> ls(kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    A_Matrix4 B;
    if (!ParamsFor(L, tm, &s) || !ls || ls->AEGP_GetLayerToWorldXform((AEGP_LayerH)L, &t, &B)) return false;
    CopyXform x;
    ComputeCopy(s.p, sub, s.anchorX, s.anchorY, &x);
    double G[16], M[16];
    WorldM4(s.world, s.anchorX, s.anchorY, G);
    Mul4(x.m4, G, M);
    const double opacity = t_lastBaseOpacity * x.opacity;
    if (!CopyEntry(entry, M, B, 1.0, opacity, *(int*)(entry + 0x34), entryBuf, m4)) return false;
    // the entry compared with AE's (its world matrix and the rest)
    const bool m4Same = !std::memcmp(m4, *(const double**)entry, 128);
    int entryDiff = -1;
    for (int k = 8; k < a3d::kEntrySize; k += 8)
        if (std::memcmp(entryBuf + k, (const void*)(entry + k), 8)) { entryDiff = k; break; }
    if (!BuildSurface(ctx, (uintptr_t)entryBuf, idx, clone)) {
        if (g_cloneLog.Take(20)) RLog("clone: sub %d culled by AE's builder (AE drew it)", sub);
        return false;
    }
    char diffs[256] = {0};
    int nd = 0;
    for (int k = 8; k < a3d::kSurfaceSize; k += 8) {
        if (k == 0xA0 || k == 0xC8) continue;  // vectors: compared below
        if (std::memcmp(clone + k, (const void*)(self + k), 8) && nd < 12) {
            const size_t l = strlen(diffs);
            snprintf(diffs + l, sizeof(diffs) - l, " +%X", k);
            nd++;
        }
    }
    const int ns = *(int*)(self + 0xD0), nc = *(int*)(clone + 0xD0);
    // a sample record: +0, +8, +0x10 point to 128-byte matrices (compared by their contents), the rest is values
    bool samples = ns == nc && ns > 0 && *(uintptr_t*)(self + 0xC8) && *(uintptr_t*)(clone + 0xC8);
    for (int i = 0; samples && i < ns; i++) {
        const unsigned char* a = *(const unsigned char**)(self + 0xC8) + 192 * i;
        const unsigned char* b = *(const unsigned char**)(clone + 0xC8) + 192 * i;
        for (int k = 0; samples && k < 0x18; k += 8) {
            const void* pa = *(const void* const*)(a + k);
            const void* pb = *(const void* const*)(b + k);
            samples = (!pa && !pb) || (pa && pb && !std::memcmp(pa, pb, 128));
        }
        samples = samples && !std::memcmp(a + 0x18, b + 0x18, 192 - 0x18);
    }
    if (!samples && ns == nc && ns > 0 && *(uintptr_t*)(self + 0xC8) && *(uintptr_t*)(clone + 0xC8)) {
        static LogLimit sl;
        const unsigned char* a = *(const unsigned char**)(self + 0xC8);
        const unsigned char* b = *(const unsigned char**)(clone + 0xC8);
        char line[512] = {0};
        for (int k = 0; k < 192; k += 8)
            if (k < 0x18 ? (*(const void* const*)(a + k) && *(const void* const*)(b + k) &&
                            std::memcmp(*(const void* const*)(a + k), *(const void* const*)(b + k), 128))
                         : std::memcmp(a + k, b + k, 8) != 0) {
                const size_t l = strlen(line);
                snprintf(line + l, sizeof(line) - l, " +%X(%016llx/%016llx %g/%g)", k, *(const unsigned long long*)(a + k),
                         *(const unsigned long long*)(b + k), *(const double*)(a + k), *(const double*)(b + k));
            }
        if (sl.Take(6)) {
            RLog("clone: sample differs at%s", line);
            for (int k = 8; k < 0x18; k += 8) {
                const double* ma = *(const double* const*)(a + k);
                const double* mb = *(const double* const*)(b + k);
                if (!ma || !mb) continue;
                char t[1024] = {0};
                for (int q = 0; q < 16; q++) {
                    const size_t l = strlen(t);
                    snprintf(t + l, sizeof(t) - l, " %.17g/%.17g", ma[q], mb[q]);
                }
                RLog("clone:   +%X ae/ours%s", k, t);
            }
        }
    }
    const int nb = *(int*)(self + 0xA8);
    const bool lights = nb == *(int*)(clone + 0xA8);
    const long long n = ++g_cloneN, same = (m4Same && entryDiff < 0 && !nd && samples && lights) ? ++g_cloneSame : g_cloneSame.load();
    if (g_cloneLog.Take(30, true))
        RLog("clone: sub %d world %s entry diff %d surface diffs%s samples %s (%d) lights %d; same %lld of %lld", sub,
             m4Same ? "same" : "DIFFERS", entryDiff, nd ? diffs : " none", samples ? "same" : "DIFFER", ns, nb, same, n);
    return true;
}

// ---- the group drawing: at the representative's draw, every copy of the group is drawn as AE would draw it if it
// were a sub-layer (sub_180014470's steps on a list of our surfaces: AE's graph, AE's draw of each component)
namespace a3d {
constexpr uintptr_t kGraphRva = 0x2AA00, kPrepareRva = 0x127B0, kDrawOneRva = 0x14800, kDrawClusterRva = 0x13470,
                    kAfterDrawRva = 0x151F0, kNewRva = 0x5FD58, kFreeRva = 0x5FBDC;
}  // namespace a3d
typedef void* (*AeNewFn)(size_t);
typedef void (*AeFreeFn)(void*, size_t);
typedef void (*GraphFn)(void* list, void* comps);
typedef void (*SurfFn)(uintptr_t);
typedef void (*DrawOneFn)(uintptr_t ctx, uintptr_t surface, void* target, int a4, uintptr_t a5);
typedef void (*DrawClusterFn)(uintptr_t ctx, void* comp, uintptr_t targetBuffer);
typedef void (*AfterDrawFn)(uintptr_t surface, uintptr_t targetBuffer);
uintptr_t HookA3dDraw(uintptr_t self, uintptr_t ctx, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7);
void FlushGroupJobs();
void DropGroupJobs();
void LogPairTests();
LogLimit g_groupLog;
typedef void (*DrawParamsFn)(void* out, uintptr_t ctx, unsigned short a3, uintptr_t a4, uintptr_t a5, int a6);
typedef uintptr_t (*SurfDrawFn)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
namespace a3d {
constexpr uintptr_t kDrawParamsRva = 0x50FD0;
}
// What sub_180014800 does for a surface without motion blur, without its BEE_ArtisanLayerCapsuleParamsHelper (the
// representative's is alive for the same layer; a second one for it waits forever): the draw parameters
// (sub_180050FD0) and the surface's draw (vtable slot 1). False: not drawn this way (motion blur path).
// RANDAMU_A3D_TIME (diagnostics): a group's draws, summed: AE's draw parameters, AE's draw up to its row callback,
// the row callback (ours), AE's draw after it
thread_local std::chrono::steady_clock::time_point t_gIterIn, t_gIterOut;
thread_local double t_gAcc[4];
thread_local int t_gIters;
bool DrawSurfaceDirect(uintptr_t ctx, uintptr_t sf, void* kt)
{
    if (!*(const unsigned char*)(sf + 128)) return true;  // nothing to draw
    const uintptr_t prep = *(uintptr_t*)ctx;
    const bool still = *(const int*)(sf + 184) == *(const int*)(sf + 176) || *(const int*)(sf + 188) == *(const int*)(sf + 180);
    if (!still || *(const unsigned char*)(prep + 296)) return false;
    alignas(16) unsigned char params[192];
    const auto tg0 = std::chrono::steady_clock::now();
    ((DrawParamsFn)(g_a3dLo + a3d::kDrawParamsRva))(params, ctx, *(const unsigned short*)(sf + 56), sf + 56,
                                                    *(const uintptr_t*)(sf + 160), *(const int*)(sf + 168));
    const auto tg1 = std::chrono::steady_clock::now();
    t_gIterIn = t_gIterOut = tg1;
    struct Acc {
        std::chrono::steady_clock::time_point a, b;
        ~Acc()
        {
            auto ms = [](std::chrono::steady_clock::time_point x, std::chrono::steady_clock::time_point y) {
                return std::chrono::duration<double, std::milli>(y - x).count();
            };
            const auto c = std::chrono::steady_clock::now();
            t_gAcc[0] += ms(a, b);
            if (t_gIterIn > b) {
                t_gAcc[1] += ms(b, t_gIterIn), t_gAcc[2] += ms(t_gIterIn, t_gIterOut), t_gAcc[3] += ms(t_gIterOut, c);
                t_gIters++;
            } else {
                t_gAcc[1] += ms(b, c);
            }
        }
    } acc{tg0, tg1};
    const uintptr_t cam = ctx + 8 * (*(const unsigned char*)(sf + 135) ? 7 : 9);
    const auto draw = *(SurfDrawFn*)(*(uintptr_t*)sf + 8);
    static const bool s_gtr = GetEnvironmentVariableA("RANDAMU_GROUP_TRACE", nullptr, 0) > 0;
    if (s_gtr) RLog("group trace: direct draw %p via %p (hook %p)", (void*)sf, (void*)draw, (void*)&HookA3dDraw);
    t_inGroupIter = true;
    if (g_profMode == 4) g_profTid = GetCurrentThreadId();  // RANDAMU_A3D_PROF=4: where a group's draw spends its time
    try {
        draw(sf, ctx, cam, *(const unsigned char*)(ctx + 104), (uintptr_t)params, (uintptr_t)*(const int*)(prep + 428),
             (uintptr_t)kt);
    } catch (int e) {  // AE throws its error codes
        t_inGroupIter = false;
        RLog("group: AE's draw of a group surface threw %d", e);
        throw;
    } catch (...) {
        t_inGroupIter = false;
        RLog("group: AE's draw of a group surface threw (not an int)");
        throw;
    }
    t_inGroupIter = false;
    if (g_profMode == 4) g_profTid = 0;
    if (s_gtr) RLog("group trace: direct draw %p done", (void*)sf);
    return true;
}

// false: not drawn by the group drawing (the caller draws the representative as an ordinary copy)
bool DrawGroup(uintptr_t self, uintptr_t ctx, uintptr_t a7, const void* L, const ParamSnapshot& s,
               const MergeCluster& g, int rep)
{
    // RANDAMU_GROUP_TRACE=1 (diagnostics): every step logged (finding where AE stops)
    static const bool tr = GetEnvironmentVariableA("RANDAMU_GROUP_TRACE", nullptr, 0) > 0;
#define GTRACE(...) \
    if (tr) RLog(__VA_ARGS__)
    GTRACE("group trace: start rep %d copies %zu", rep, g.copies.size());
    static const bool s_time = GetEnvironmentVariableA("RANDAMU_A3D_TIME", nullptr, 0) > 0;
    const auto tg0 = std::chrono::steady_clock::now();
    auto gms = [](std::chrono::steady_clock::time_point a) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
    };
    double tBuild = 0, tGraph = 0;
    const uintptr_t prep = *(uintptr_t*)ctx;
    const int idx = *(int*)(self + a3d::kSurfLayerIndex);
    if (*(int*)a7 != 1 || !prep || *(const unsigned char*)(prep + 0x128) || idx < 0 ||
        idx >= *(int*)(prep + a3d::kPrepEntryCount)) {
        if (g_groupLog.Take(10)) RLog("group: not drawn as a group (target kind %d, motion %d)", *(int*)a7, prep ? (int)*(const unsigned char*)(prep + 0x128) : -1);
        return false;
    }
    const uintptr_t tmpl = *(uintptr_t*)(prep + a3d::kPrepEntries) + (uintptr_t)idx * a3d::kEntrySize;
    A_Time t = {0, 1}, step = {0, 1};
    Suite<AEGP_LayerSuite9> ls(kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    A_Matrix4 B;
    if (!g_canvas8 || g_canvas8->AEGP_GetCompRenderTime((PR_RenderContextH)*(void**)(ctx + a3d::kCtxRenderContext), &t, &step) ||
        !ls || ls->AEGP_GetLayerToWorldXform((AEGP_LayerH)L, &t, &B))
        return false;
    double G[16];
    WorldM4(s.world, s.anchorX, s.anchorY, G);
    const int n = (int)g.copies.size();
    std::vector<unsigned char> entries((size_t)n * a3d::kEntrySize), surfs((size_t)n * a3d::kSurfaceSize);
    std::vector<double> m4((size_t)n * 16);
    std::vector<uintptr_t> built;
    const int tmplIndex = *(const int*)(tmpl + 0x34);
    const double base = t_lastBaseOpacity;
    int mism = 0;
    for (int k = 0; k < n; k++) {
        const int c = g.copies[k];
        CopyXform x;
        ComputeCopy(s.p, c, s.anchorX, s.anchorY, &x);
        double M[16];
        Mul4(x.m4, G, M);
        unsigned char* e = &entries[(size_t)k * a3d::kEntrySize];
        if (!CopyEntry(tmpl, M, B, 1.0, base * x.opacity, tmplIndex + (c - rep), e, &m4[(size_t)k * 16])) return false;
        if (c == rep && (std::memcmp(&m4[(size_t)k * 16], *(const double**)tmpl, 128) ||
                         *(const double*)(e + 0x40) != *(const double*)(tmpl + 0x40)))
            mism++;
        // sub_180013160's conditions for a surface: opacity, visible, a texture
        if (*(const double*)(e + 0x40) == 0 || !(e[0x78] || e[0x79]) || !*(const int*)(e + 8)) continue;
        unsigned char* sf = &surfs[(size_t)k * a3d::kSurfaceSize];
        GTRACE("group trace: build copy %d", c);
        if (BuildSurface(ctx, (uintptr_t)e, idx, sf)) built.push_back((uintptr_t)sf);
    }
    GTRACE("group trace: built %zu", built.size());
    tBuild = gms(tg0);
    if (mism && g_groupLog.Take(20)) RLog("group: the representative's rebuilt entry differs from AE's (world or opacity)");
    // AE's list of surfaces and its components (std::list nodes: next, prev, value), allocated by AE's new
    const auto aeNew = (AeNewFn)(g_a3dLo + a3d::kNewRva);
    const auto aeFree = (AeFreeFn)(g_a3dLo + a3d::kFreeRva);
    struct List {
        uintptr_t* head;
        size_t size;
    } list{(uintptr_t*)aeNew(0x18), 0}, comps{(uintptr_t*)aeNew(0x20), 0};
    list.head[0] = list.head[1] = (uintptr_t)list.head;
    comps.head[0] = comps.head[1] = (uintptr_t)comps.head;
    for (uintptr_t sf : built) {
        uintptr_t* node = (uintptr_t*)aeNew(0x18);
        uintptr_t* last = (uintptr_t*)list.head[1];
        node[0] = (uintptr_t)list.head, node[1] = (uintptr_t)last, node[2] = sf;
        last[0] = (uintptr_t)node, list.head[1] = (uintptr_t)node;
        list.size++;
    }
    GTRACE("group trace: graph");
    ((GraphFn)(g_a3dLo + a3d::kGraphRva))(&list, &comps);
    LogPairTests();
    GTRACE("group trace: graph done (%zu components)", comps.size);
    const double tSort = gms(tg0) - tBuild;
    for (uintptr_t sf : built) ((SurfFn)(g_a3dLo + a3d::kPrepareRva))(sf);
    GTRACE("group trace: prepared");
    tGraph = gms(tg0) - tBuild;
    if (s_time && built.size() > 500) RLog("group time: graph %.2f of it AE's sort, %.2f the surfaces' preparation", tSort, tGraph - tSort);
    const auto tDraw0 = std::chrono::steady_clock::now();
    for (double& v : t_gAcc) v = 0;
    t_gIters = 0;
    const int savedCount = *(int*)(ctx + a3d::kCtxProgressCount);
    const uintptr_t target = *(uintptr_t*)(a7 + 8);
    struct InGroup {  // also when AE throws (its error codes) out of a draw: the kept jobs are dropped then
        bool done = false;
        InGroup() { ++t_inGroup; }
        ~InGroup()
        {
            if (!done) DropGroupJobs();
            --t_inGroup;
        }
    };
    auto inGroup = std::make_unique<InGroup>();
    int drawn = 0, clusters = 0;
    for (uintptr_t* j = (uintptr_t*)comps.head[0]; j != comps.head; j = (uintptr_t*)j[0]) {
        if (j[3] == 1) {
            struct {
                int kind;
                uintptr_t buf;
            } kt{1, target};
            const uintptr_t sf = ((uintptr_t*)((uintptr_t*)j[2])[0])[2];
            GTRACE("group trace: draw one %p", (void*)sf);
            if (!DrawSurfaceDirect(ctx, sf, &kt) && g_groupLog.Take(10)) RLog("group: a copy on the motion path was not drawn");
            if (*(const unsigned char*)(sf + 0x81)) ((AfterDrawFn)(g_a3dLo + a3d::kAfterDrawRva))(sf, target);
            drawn++;
        } else {
            // AE's cluster drawing (sub_180013470) makes a capsule helper per surface too (it would wait forever):
            // its surfaces drawn one by one in the component's order (not AE's pixels where they cross; the plan
            // keeps copies that meet in 3D out of groups)
            if (g_groupLog.Take(10)) RLog("group: a cluster of %llu copies drawn one by one", (unsigned long long)j[3]);
            for (uintptr_t* q = (uintptr_t*)((uintptr_t*)j[2])[0]; q != (uintptr_t*)j[2]; q = (uintptr_t*)q[0]) {
                struct {
                    int kind;
                    uintptr_t buf;
                } kt{1, target};
                DrawSurfaceDirect(ctx, q[2], &kt);
            }
            drawn += (int)j[3], clusters++;
        }
    }
    FlushGroupJobs();
    inGroup->done = true;
    inGroup.reset();
    GTRACE("group trace: drawn");
    const double tDraw = gms(tDraw0);
    *(int*)(ctx + a3d::kCtxProgressCount) = savedCount;
    // the lists freed as sub_180014470 frees them
    for (uintptr_t* j = (uintptr_t*)comps.head[0]; j != comps.head;) {
        uintptr_t* next = (uintptr_t*)j[0];
        uintptr_t* inner = (uintptr_t*)j[2];
        for (uintptr_t* q = (uintptr_t*)inner[0]; q != inner;) {
            uintptr_t* qn = (uintptr_t*)q[0];
            aeFree(q, 0x18);
            q = qn;
        }
        aeFree(inner, 0x18);
        aeFree(j, 0x20);
        j = next;
    }
    aeFree(comps.head, 0x20);
    for (uintptr_t* q = (uintptr_t*)list.head[0]; q != list.head;) {
        uintptr_t* qn = (uintptr_t*)q[0];
        aeFree(q, 0x18);
        q = qn;
    }
    aeFree(list.head, 0x18);
    GTRACE("group trace: lists freed");
    for (uintptr_t sf : built) FreeSurface((unsigned char*)sf);
    GTRACE("group trace: surfaces freed");
#undef GTRACE
    static LogLimit tl;
    if (s_time && tl.Take(200))
        RLog("group time: %d copies: build %.2f graph %.2f draw %.2f all %.2f ms (draws: params %.2f, AE before the rows %.2f, "
             "rows %.2f, AE after %.2f; %d row loops)",
             n, tBuild, tGraph, tDraw, gms(tg0), t_gAcc[0], t_gAcc[1], t_gAcc[2], t_gAcc[3], t_gIters);
    int nn = 0;
    if (g_groupLog.Take(8, true, &nn))
        RLog("group: drew %d of %d copies (%zu surfaces, %d clusters) at copy %d", drawn, n, built.size(), clusters, rep);
    return true;
}

typedef uintptr_t (*A3dDrawFn)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef int (*ExtractRefconFn)(void*, void**);
typedef int (*NthLayerFn)(void*, int, void**);
A3dDrawFn g_origA3dDraw = nullptr;
ExtractRefconFn g_extractRefcon = nullptr;
NthLayerFn g_nthLayer = nullptr;
LogLimit g_a3dDrawLog;
std::atomic<long> g_a3dDrawSeq{0};

// The layer and sub-layer index of a surface drawn through ctx (the surface's +224: its render layer index).
bool SurfaceLayer(uintptr_t ctx, uintptr_t sf, const void** L, int* sub)
{
    void* rf = nullptr;
    void* tr = nullptr;
    void** rcx = *(void***)(ctx + a3d::kCtxRenderContext);
    if (!g_extractRefcon || !g_nthLayer || g_extractRefcon(rcx, &rf) ||
        g_nthLayer(rf, *(int*)(sf + a3d::kSurfLayerIndex), &tr) || !tr)
        return false;
    char* v = *(char**)((char*)tr + bee::kTraceArtLayer);
    if (!v) return false;
    *L = *(void**)(v + bee::kArtLayerToLayer), *sub = *(int*)(v + bee::kArtLayerToSubIndex);
    return *L && *sub >= 0;
}

// ---- the front and back groups: AE's order moved. sub_180014470 builds its list of surfaces, has its graph
// (sub_18002AA00, called from +0x14630 only) make the list of components in drawing order (std::list nodes: next,
// prev, the component's own list of surfaces, its size), then draws them in that order. The call goes through
// HookGraph: after AE's graph, the component of a front group's representative is moved to the end of the list
// (drawn after every surface), a back group's to the start. The surfaces moved are remembered per pass (ctx) so that
// the draw knows the group is at its place.
namespace a3d {
constexpr uintptr_t kGraphCallRva = 0x14630;
constexpr ptrdiff_t kCompsToCtx = 0x28;  // sub_180014470: comps at rsp+0x68, its draw context at rbp-0x70 = rsp+0x90
}  // namespace a3d
GraphFn g_origGraph = nullptr;
struct MovedSurface {
    uintptr_t ctx, sf;
};
thread_local std::vector<MovedSurface> t_moved;
LogLimit g_moveLog;
bool WasMoved(uintptr_t ctx, uintptr_t sf)
{
    for (const MovedSurface& m : t_moved)
        if (m.ctx == ctx && m.sf == sf) return true;
    return false;
}

void HookGraph(void* list, void* comps)
{
    g_origGraph(list, comps);
    LogPairTests();
    const uintptr_t ctx = (uintptr_t)comps + a3d::kCompsToCtx;
    // a pass at this stack depth before is over (a nested pass, deeper on the stack, keeps the outer one's)
    t_moved.erase(std::remove_if(t_moved.begin(), t_moved.end(), [&](const MovedSurface& m) { return m.ctx <= ctx; }),
                  t_moved.end());
    if (t_moved.size() > 4096) t_moved.clear();
    if (!GroupDrawOn() || !FrontBackGroupsOn() || OldMerge()) return;
    try {
        uintptr_t* head = *(uintptr_t**)comps;
        // the plans of the layers met (a frame has few merge layers)
        struct Seen {
            const void* L;
            std::shared_ptr<const MergePlan> plan;
            const MergeLayerPlan* lp;
        };
        std::vector<Seen> seen;
        auto planOf = [&](const void* L) -> const MergeLayerPlan* {
            for (const Seen& s : seen)
                if (s.L == L) return s.lp;
            Seen s{L, nullptr, nullptr};
            int ms = MergeSeenOf(L);
            ParamSnapshot ps;
            if (ms != 0 && IsMine(L)) {
                LayerKey k;
                const int* tm = KeyOf(L, &k) ? ArtTimeOf(k) : nullptr;
                if (tm && ParamsFor(L, tm, &ps) && ps.p.merge) s.plan = MergePlanOf(L, tm, ps, false, &s.lp);
            }
            seen.push_back(s);
            return s.lp;
        };
        // the place a surface's draw has in a group (0: none)
        auto placeOf = [&](uintptr_t sf) {
            const void* L = nullptr;
            int sb = -1;
            if (!SurfaceLayer(ctx, sf, &L, &sb)) return 0;
            const MergeLayerPlan* lp = planOf(L);
            if (!lp || sb >= (int)lp->subs.size() || lp->subs[sb].cluster < 0 ||
                lp->subs[sb].cluster >= (int)lp->clusters.size())
                return 0;
            return lp->clusters[lp->subs[sb].cluster].place;
        };
        std::vector<uintptr_t*> front, back;
        int n = 0, inCluster = 0;
        for (uintptr_t* j = (uintptr_t*)head[0]; j != head; j = (uintptr_t*)j[0], n++) {
            uintptr_t* inner = (uintptr_t*)j[2];
            if (j[3] == 1) {
                const uintptr_t sf = ((uintptr_t*)inner[0])[2];
                const int p = placeOf(sf);
                if (p > 0) front.push_back(j);
                else if (p < 0) back.push_back(j);
                if (p) t_moved.push_back({ctx, sf});
            } else {
                // a representative inside AE's cluster: the plan's sides were wrong (it is left where it is)
                for (uintptr_t* q = (uintptr_t*)inner[0]; q != inner; q = (uintptr_t*)q[0])
                    if (placeOf(q[2])) inCluster++;
            }
        }
        if (inCluster) RLog("group: %d front/back representatives in AE's clusters (not moved; the plan is wrong)", inCluster);
        // a representative AE did not put in its list (culled): its group is not drawn at all
        {
            int groups = 0;
            for (const Seen& s : seen)
                if (s.lp)
                    for (const MergeCluster& c : s.lp->clusters) groups += c.place != 0;
            static LogLimit ml;
            if (groups > (int)(front.size() + back.size()) && ml.Take(20))
                RLog("group: %d front/back groups but %zu representatives in AE's list (a representative culled: its group "
                     "is missing)", groups, front.size() + back.size());
        }
        auto unlink = [](uintptr_t* j) {
            uintptr_t* nx = (uintptr_t*)j[0];
            uintptr_t* pv = (uintptr_t*)j[1];
            pv[0] = (uintptr_t)nx, nx[1] = (uintptr_t)pv;
        };
        auto insertAfter = [](uintptr_t* at, uintptr_t* j) {
            uintptr_t* nx = (uintptr_t*)at[0];
            j[0] = (uintptr_t)nx, j[1] = (uintptr_t)at;
            at[0] = (uintptr_t)j, nx[1] = (uintptr_t)j;
        };
        for (uintptr_t* j : front) unlink(j), insertAfter((uintptr_t*)head[1], j);  // each at the end, in order
        uintptr_t* at = head;
        for (uintptr_t* j : back) unlink(j), insertAfter(at, j), at = j;  // at the start, in order
        int ln = 0;
        if ((front.size() || back.size()) && g_moveLog.Take(10, true, &ln))
            RLog("group: moved %zu front and %zu back groups (of %d components)", front.size(), back.size(), n);
    } catch (...) {
        RLog("group: the reorder threw (the order is AE's)");
    }
}

// The call at site (E8 rel32, checked by the caller) made to go to fn through a stub within 2 GB of it
// (mov rax, fn; jmp rax). At plug-in setup, before any render.
bool RedirectCall(uintptr_t lo, const unsigned char* site, void* fn, const char** why)
{
    unsigned char* stub = nullptr;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    for (uintptr_t d = gran; d < 0x70000000 && !stub; d += gran) {
        for (const uintptr_t a : {(lo & ~(gran - 1)) - d, ((lo + 0x200000) & ~(gran - 1)) + d}) {
            stub = (unsigned char*)VirtualAlloc((void*)a, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (stub) {
                const long long off = (long long)(uintptr_t)stub - (long long)(uintptr_t)(site + 5);
                if (off > -0x78000000LL && off < 0x78000000LL) break;
                VirtualFree(stub, 0, MEM_RELEASE);
                stub = nullptr;
            }
        }
    }
    if (!stub) return *why = "no memory near Advanced3D", false;
    stub[0] = 0x48, stub[1] = 0xB8;
    std::memcpy(stub + 2, &fn, 8);
    stub[10] = 0xFF, stub[11] = 0xE0;
    DWORD old = 0;
    VirtualProtect(stub, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, 16);
    const int newRel = (int)((long long)(uintptr_t)stub - (long long)(uintptr_t)(site + 5));
    if (!VirtualProtect((void*)(site + 1), 4, PAGE_EXECUTE_READWRITE, &old)) return *why = "VirtualProtect failed", false;
    std::memcpy((void*)(site + 1), &newRel, 4);
    VirtualProtect((void*)(site + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return true;
}

// ---- RANDAMU_GRAPH_PROF=1 (diagnostics): AE's pair test in its graph (sub_18002AB40 +0x2ACEA calls sub_18002A630)
// counted and timed: all pairs, pairs whose screen boxes overlap (surface +8 .. +0x20), and the time in each
typedef void (*PairTestFn)(uintptr_t a, uintptr_t b, unsigned char* aFront, unsigned char* bFront);
PairTestFn g_origPairTest = nullptr;
std::atomic<long long> g_pairN{0}, g_pairBox{0}, g_pairTicks{0}, g_pairBoxTicks{0};
void HookPairTest(uintptr_t a, uintptr_t b, unsigned char* f1, unsigned char* f2)
{
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    g_origPairTest(a, b, f1, f2);
    QueryPerformanceCounter(&t1);
    const double* A = (const double*)(a + 8);
    const double* B = (const double*)(b + 8);
    const bool box = std::min(A[2], B[2]) > std::max(A[0], B[0]) && std::min(A[3], B[3]) > std::max(A[1], B[1]);
    g_pairN++, g_pairTicks += t1.QuadPart - t0.QuadPart;
    if (box) g_pairBox++, g_pairBoxTicks += t1.QuadPart - t0.QuadPart;
}
void LogPairTests()
{
    if (!g_origPairTest || g_pairN.load() < 1000) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    RLog("graph prof: %lld pair tests %.2f ms, of them %lld with overlapping boxes %.2f ms", g_pairN.exchange(0),
         g_pairTicks.exchange(0) * 1000.0 / f.QuadPart, g_pairBox.exchange(0), g_pairBoxTicks.exchange(0) * 1000.0 / f.QuadPart);
}
void InstallPairTestProf(uintptr_t lo)
{
    const unsigned char* site = (const unsigned char*)(lo + 0x2ACEA);
    const int rel = site[0] == 0xE8 ? *(const int*)(site + 1) : 0;
    const char* why = "the call site does not match";
    if (site[0] != 0xE8 || (uintptr_t)(site + 5 + rel) != lo + 0x2A630) {
        RLog("graph prof: not installed (%s)", why);
        return;
    }
    g_origPairTest = (PairTestFn)(lo + 0x2A630);
    RLog("graph prof: %s", RedirectCall(lo, site, (void*)&HookPairTest, &why) ? "installed" : why);
}

// ---- AE's graph edges without its O(n^2) enumeration. sub_18002AA00 calls sub_18002AB40 (+0x2AAD1), which makes a
// node (0x30 bytes: next, prev, +16 the surface, +24 its edge list's head, +32 the edge count, +40 a word 0) for
// every surface of the list in order, then for every pair (earlier i, later j) calls the pair test
// sub_18002A630(surface i, surface j, &f1, &f2): f1 appends to j's edge list a node whose value is &node_i[2], f2
// to i's list &node_j[2]. Each node's edge list is therefore in its partners' list order. The pair test answers
// nothing for surfaces whose screen boxes (+8 x0, +0x10 y0, +0x18 x1, +0x20 y1) do not overlap strictly
// (sub_18002B040's first checks): HookBuildEdges calls it only for the pairs whose boxes overlap (a sweep in x)
// and builds the same lists. A box with a NaN: AE's own enumeration.
typedef void (*BuildEdgesFn)(uintptr_t** surfaces, uintptr_t* nodes);
BuildEdgesFn g_origBuildEdges = nullptr;
namespace a3d {
constexpr uintptr_t kBuildEdgesCallRva = 0x2AAD1, kBuildEdgesRva = 0x2AB40, kClearNodesRva = 0x2A640, kPairTestRva = 0x2A630;
}
typedef void (*ClearNodesFn)(uintptr_t* list, uintptr_t first);
std::atomic<long long> g_edgeChecks{0}, g_edgeMismatch{0};
thread_local double t_edgePairMs = 0;  // diagnostics: the last build's pair tests
thread_local size_t t_edgeCand = 0;
LogLimit g_edgeLog;
// sub_18002B040 with core/ae_pair (the same answers: tests/unit ae_pair_matches_advanced3d calls AE's own on millions of
// cases). Samples it does not take (no samples, more than 64 vertices): AE's own test.
thread_local bool t_pairNoAe = false;  // a worker thread of BuildEdgesFast: AE's test is left to the caller (*f1 = 2)
void PairOurs(uintptr_t A, uintptr_t B, unsigned char* f1, unsigned char* f2)
{
    *f1 = *f2 = 0;
    if (!rnd::AePairBoxes((const double*)(A + 8), (const double*)(B + 8))) return;
    const int nA = *(const int*)(A + 0xD0), nB = *(const int*)(B + 0xD0), n = std::max(nA, nB);
    auto ae = [&] {
        if (t_pairNoAe) {
            *f1 = 2, *f2 = 0;
            return;
        }
        ((PairTestFn)(g_a3dLo + a3d::kPairTestRva))(A, B, f1, f2);
    };
    if (nA <= 0 || nB <= 0) return ae();
    auto sample = [](uintptr_t S, int idx, rnd::AePairSample* out) {
        const unsigned char* r = *(const unsigned char* const*)(S + 0xC8) + 192 * (size_t)idx;
        const double* b = *(const double* const*)r;
        const double* e = *(const double* const*)(r + 8);
        if (b == e) return false;
        out->v = b, out->n = (int)((e - b) / 4);
        std::memcpy(&out->a, r + 0x18, 32);
        return true;
    };
    for (int k = 0; k < n; k++) {
        rnd::AePairSample sa, sb;
        if (!sample(A, std::min(k, nA - 1), &sa) || !sample(B, std::min(k, nB - 1), &sb)) continue;
        if (!rnd::AePairSamples(sa, sb, f1, f2)) return (void)(*f1 = *f2 = 0), ae();
        if (*f1 && *f2) return;
    }
}
void BuildEdgesFast(uintptr_t** surfaces, uintptr_t* a2)
{
    const auto aeNew = (AeNewFn)(g_a3dLo + a3d::kNewRva);
    // RANDAMU_PAIR_OURS=0: AE's own pair test
    static const bool s_aePair = [] {
        char v[4] = {0};
        return GetEnvironmentVariableA("RANDAMU_PAIR_OURS", v, sizeof(v)) > 0 && v[0] == '0';
    }();
    const auto pair = s_aePair ? (PairTestFn)(g_a3dLo + a3d::kPairTestRva) : &PairOurs;
    ((ClearNodesFn)(g_a3dLo + a3d::kClearNodesRva))(a2, *a2);
    uintptr_t* head = (uintptr_t*)*a2;
    head[0] = head[1] = (uintptr_t)head;
    a2[1] = 0;
    std::vector<uintptr_t*> nodes;
    for (uintptr_t* i = (uintptr_t*)(*surfaces)[0]; i != *surfaces; i = (uintptr_t*)i[0]) {
        uintptr_t* adj = (uintptr_t*)aeNew(0x18);
        adj[0] = adj[1] = (uintptr_t)adj;
        uintptr_t* v = (uintptr_t*)aeNew(0x30);
        v[2] = i[2], v[3] = (uintptr_t)adj, v[4] = 0;
        *(unsigned short*)((unsigned char*)v + 40) = 0;
        *(int*)((unsigned char*)v + 44) = 0;
        uintptr_t* last = (uintptr_t*)head[1];
        v[0] = (uintptr_t)head, v[1] = (uintptr_t)last;
        head[1] = (uintptr_t)v, last[0] = (uintptr_t)v;
        a2[1]++;
        nodes.push_back(v);
    }
    const int n = (int)nodes.size();
    struct B {
        double x0, y0, x1, y1;
        int i;
    };
    std::vector<B> bs((size_t)n);
    for (int k = 0; k < n; k++) {
        const double* b = (const double*)(nodes[k][2] + 8);
        bs[k] = {b[0], b[1], b[2], b[3], k};
    }
    std::sort(bs.begin(), bs.end(), [](const B& a, const B& b) { return a.x0 < b.x0; });
    // the pairs whose boxes overlap (earlier, later in the list)
    std::vector<std::pair<int, int>> cand;
    for (int a = 0; a < n; a++) {
        const B& A = bs[a];
        for (int b = a + 1; b < n && bs[b].x0 < A.x1; b++) {
            const B& Bb = bs[b];
            if (!(std::min(A.y1, Bb.y1) > std::max(A.y0, Bb.y0))) continue;
            cand.push_back({std::min(A.i, Bb.i), std::max(A.i, Bb.i)});
        }
    }
    // the pair tests. Ours (core/ae_pair) only reads the two surfaces: many at once on several threads. AE's own was
    // no faster on several threads (11/26: its work memory allocations take turns), so it runs on this thread.
    std::vector<unsigned char> flags(cand.size() * 2, 0);
    const auto tp0 = std::chrono::steady_clock::now();
    const int nt = s_aePair || cand.size() < 4096 ? 1 : std::max(1, std::min(8, (int)std::thread::hardware_concurrency()));
    auto work = [&](size_t from, size_t to) {
        for (size_t k = from; k < to; k++)
            pair(nodes[cand[k].first][2], nodes[cand[k].second][2], &flags[2 * k], &flags[2 * k + 1]);
    };
    if (nt <= 1) {
        work(0, cand.size());
    } else {
        // ours falls back to AE's test for samples it does not take: rare; AE's errors would throw on a worker, so
        // those pairs are retried here (PairOurs on the workers never calls AE: see t_pairNoAe)
        std::vector<std::thread> ts;
        const size_t per = (cand.size() + nt - 1) / nt;
        for (int t = 1; t < nt; t++)
            ts.emplace_back([&, t] {
                t_pairNoAe = true;
                work(std::min(cand.size(), per * t), std::min(cand.size(), per * (t + 1)));
            });
        work(0, std::min(cand.size(), per));
        for (std::thread& t : ts) t.join();
        for (size_t k = 0; k < cand.size(); k++)
            if (flags[2 * k] == 2)  // a worker's pair left to AE
                flags[2 * k] = flags[2 * k + 1] = 0,
                ((PairTestFn)(g_a3dLo + a3d::kPairTestRva))(nodes[cand[k].first][2], nodes[cand[k].second][2], &flags[2 * k],
                                                             &flags[2 * k + 1]);
    }
    t_edgePairMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tp0).count();
    t_edgeCand = cand.size();
    std::vector<std::vector<int>> partners((size_t)n);
    for (size_t k = 0; k < cand.size(); k++) {
        if (flags[2 * k]) partners[cand[k].second].push_back(cand[k].first);
        if (flags[2 * k + 1]) partners[cand[k].first].push_back(cand[k].second);
    }
    for (int k = 0; k < n; k++) {
        std::vector<int>& p = partners[k];
        std::sort(p.begin(), p.end());
        uintptr_t* adj = (uintptr_t*)nodes[k][3];
        for (int q : p) {
            uintptr_t* e = (uintptr_t*)aeNew(0x18);
            uintptr_t* last = (uintptr_t*)adj[1];
            e[2] = (uintptr_t)(nodes[q] + 2);
            e[0] = (uintptr_t)adj, e[1] = (uintptr_t)last;
            adj[1] = (uintptr_t)e, last[0] = (uintptr_t)e;
            nodes[k][4]++;
        }
    }
}
// The edges of a node list as partner indices (for the check)
std::vector<std::vector<int>> EdgesOf(uintptr_t* a2)
{
    uintptr_t* head = (uintptr_t*)*a2;
    std::vector<uintptr_t*> nodes;
    for (uintptr_t* v = (uintptr_t*)head[0]; v != head; v = (uintptr_t*)v[0]) nodes.push_back(v);
    std::vector<std::vector<int>> out(nodes.size());
    for (size_t k = 0; k < nodes.size(); k++) {
        uintptr_t* adj = (uintptr_t*)nodes[k][3];
        for (uintptr_t* e = (uintptr_t*)adj[0]; e != adj; e = (uintptr_t*)e[0]) {
            const uintptr_t target = e[2] - 16;
            const auto it = std::find(nodes.begin(), nodes.end(), (uintptr_t*)target);
            out[k].push_back(it == nodes.end() ? -1 : (int)(it - nodes.begin()));
        }
    }
    return out;
}
void HookBuildEdges(uintptr_t** surfaces, uintptr_t* a2)
{
    // RANDAMU_GRAPH_FAST=0: AE's own enumeration
    static const bool s_off = [] {
        char v[4] = {0};
        return GetEnvironmentVariableA("RANDAMU_GRAPH_FAST", v, sizeof(v)) > 0 && v[0] == '0';
    }();
    bool nan = false;
    for (uintptr_t* i = (uintptr_t*)(*surfaces)[0]; i != *surfaces && !nan; i = (uintptr_t*)i[0]) {
        const double* b = (const double*)(i[2] + 8);
        nan = b[0] != b[0] || b[1] != b[1] || b[2] != b[2] || b[3] != b[3];
    }
    if (s_off || nan) return g_origBuildEdges(surfaces, a2);
    const auto te0 = std::chrono::steady_clock::now();
    BuildEdgesFast(surfaces, a2);
    {
        static const bool s_time = GetEnvironmentVariableA("RANDAMU_A3D_TIME", nullptr, 0) > 0;
        static LogLimit el;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - te0).count();
        if (s_time && a2[1] > 500 && el.Take(50))
            RLog("graph time: edges of %llu surfaces %.2f ms (pair tests %zu, %.2f ms)", (unsigned long long)a2[1], ms, t_edgeCand,
                 t_edgePairMs);
    }
    // RANDAMU_GRAPH_CHECK=1 (tests): AE's own edges too, into a list of our own, compared
    static const bool s_check = GetEnvironmentVariableA("RANDAMU_GRAPH_CHECK", nullptr, 0) > 0;
    if (!s_check) return;
    const auto aeNew = (AeNewFn)(g_a3dLo + a3d::kNewRva);
    const auto aeFree = (AeFreeFn)(g_a3dLo + a3d::kFreeRva);
    uintptr_t* h = (uintptr_t*)aeNew(0x30);
    h[0] = h[1] = (uintptr_t)h;
    uintptr_t ref[2] = {(uintptr_t)h, 0};
    g_origBuildEdges(surfaces, ref);
    const bool same = EdgesOf(a2) == EdgesOf(ref);
    ((ClearNodesFn)(g_a3dLo + a3d::kClearNodesRva))(ref, ref[0]);
    aeFree((void*)ref[0], 0x30);
    const long long c = ++g_edgeChecks, m = same ? g_edgeMismatch.load() : ++g_edgeMismatch;
    if (!same || g_edgeLog.Take(20)) RLog("graph check: edges %s (%lld of %lld differ)", same ? "same" : "DIFFER", m, c);
}
void InstallBuildEdgesHook(uintptr_t lo)
{
    const unsigned char* site = (const unsigned char*)(lo + a3d::kBuildEdgesCallRva);
    const int rel = site[0] == 0xE8 ? *(const int*)(site + 1) : 0;
    const unsigned char* pt = (const unsigned char*)(lo + 0x2ACEA);  // the pair test's call in sub_18002AB40
    const int prel = pt[0] == 0xE8 ? *(const int*)(pt + 1) : 0;
    const char* why = "the call site does not match";
    if (site[0] != 0xE8 || (uintptr_t)(site + 5 + rel) != lo + a3d::kBuildEdgesRva || pt[0] != 0xE8 ||
        (uintptr_t)(pt + 5 + prel) != lo + a3d::kPairTestRva) {
        RLog("graph edges hook: not installed (%s)", why);
        return;
    }
    g_origBuildEdges = (BuildEdgesFn)(lo + a3d::kBuildEdgesRva);
    RLog("graph edges hook: %s", RedirectCall(lo, site, (void*)&HookBuildEdges, &why) ? "installed" : why);
}

void InstallGraphHook(HMODULE a3d)
{
    const uintptr_t lo = (uintptr_t)a3d;
    const unsigned char* site = (const unsigned char*)(lo + a3d::kGraphCallRva);
    // the call and the frame it relies on: lea rdx,[rsp+68h]; lea rcx,[rsp+50h]; call; the prologue's
    // lea rbp,[rsp-0B0h]; sub rsp,1B0h; and lea rcx,[rbp-70h] (the draw context) before sub_18000FD40
    static const unsigned char kBefore[] = {0x48, 0x8D, 0x54, 0x24, 0x68, 0x48, 0x8D, 0x4C, 0x24, 0x50};
    static const unsigned char kProlog[] = {0x48, 0x8D, 0xAC, 0x24, 0x50, 0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xB0, 0x01, 0x00, 0x00};
    static const unsigned char kCtxLea[] = {0x48, 0x8D, 0x4D, 0x90};
    const int rel = site[0] == 0xE8 ? *(const int*)(site + 1) : 0;
    if (std::memcmp(site - sizeof(kBefore), kBefore, sizeof(kBefore)) ||
        std::memcmp((const void*)(lo + 0x14488), kProlog, sizeof(kProlog)) ||
        std::memcmp((const void*)(lo + 0x144FD), kCtxLea, sizeof(kCtxLea)) || site[0] != 0xE8 ||
        (uintptr_t)(site + 5 + rel) != lo + a3d::kGraphRva) {
        RLog("group: reorder hook not installed (the call site does not match)");
        return;
    }
    g_origGraph = (GraphFn)(lo + a3d::kGraphRva);
    const char* why = nullptr;
    if (!RedirectCall(lo, site, (void*)&HookGraph, &why)) {
        RLog("group: reorder hook not installed (%s)", why);
        return;
    }
    g_graphHooked = true;
    RLog("group: reorder hook installed");
    if (GetEnvironmentVariableA("RANDAMU_GRAPH_PROF", nullptr, 0) > 0) InstallPairTestProf(lo);
    InstallBuildEdgesHook(lo);
}

// ---- DupliX draws a copy's pixels with AE's own arithmetic (core/ae_span): AE's draw runs (its raster state and
// row spans), and only the row callback of its pixel loop (sub_180016900) is ours.
namespace a3d {
constexpr uintptr_t kRowPolygonRva = 0x16900;
// d (the iterate data of the polygon path): +0 ctx, +8 raster state, +16 layer params, +24 mask, +32 spans**,
// +40 row count, +44 first y, +48 field mode, +56 target kind (a7)
struct SpanJob {
    uintptr_t d = 0;
    rnd::AeSpanParams p;
    rnd::AeTex8 tex;
    unsigned char* tdata = nullptr;
    int trb = 0;
};
}  // namespace a3d
rnd::AeRcpTable g_rcp;
int g_cpuFlag = -1;
std::mutex g_jobMu;
a3d::SpanJob g_jobs[32];
LogLimit g_oursLog, g_oursSkip;
std::atomic<long long> g_oursDraws{0}, g_aeDraws{0};

uint16_t BitDepthOf(uintptr_t buf) { return (*(uint16_t(**)(uintptr_t))(*(uintptr_t*)buf + 8))(buf); }

// whether DupliX can draw this draw's rows (the path core/ae_span reproduces), and its parameters
bool SpanJobFor(uintptr_t d, a3d::SpanJob* j, const char** why)
{
    const uintptr_t ctx = *(uintptr_t*)d, st = *(uintptr_t*)(d + 8), lp = *(uintptr_t*)(d + 16), a7 = *(uintptr_t*)(d + 56);
    if (*(int*)a7 != 1) return *why = "target kind", false;
    const uintptr_t tgt = *(uintptr_t*)(a7 + 8), tex = *(uintptr_t*)(st + 16);
    if (!tgt || !tex || BitDepthOf(tgt) != 8 || BitDepthOf(tex) != 8) return *why = "bit depth", false;
    if (*(int*)(st + 40) != 1) return *why = "motion samples", false;
    const int q = *(unsigned char*)(st + 0xA8);
    if (q != 0 && q != 1) return *why = "quality", false;
    const unsigned char* p = (const unsigned char*)lp;
    if (!p || p[0x18] != 2 || p[0x8C] || p[0x8D]) return *why = "transfer / lights", false;
    const uintptr_t b0 = *(uintptr_t*)(lp + 0xB0);
    if (b0 && *(uintptr_t*)b0) return *why = "params +0xB0", false;
    if (*(uintptr_t*)(d + 24)) return *why = "mask", false;
    if (*(uintptr_t*)(*(uintptr_t*)ctx + 480)) return *why = "blending tables", false;
    if (g_cpuFlag != 1) return *why = "cpu path", false;
    const double* m = (const double*)(*(uintptr_t*)(st + 32) + 72 * (size_t) * (int*)(st + 48));
    for (int k = 0; k < 9; k++) j->p.m[k] = m[k];
    j->p.persp = *(unsigned char*)(st + 0x98) != 0;
    j->p.a0 = *(double*)(st + 0xA0);
    j->p.quality = q;
    j->p.opacity = *(double*)(lp + 0x20);
    j->p.colorMode = p[0x5B];
    j->p.rcp = &g_rcp;
    j->tex.data = *(const uint8_t**)(tex + 32), j->tex.rowbytes = *(int*)(tex + 40);
    j->tex.width = *(int*)(tex + 44), j->tex.height = *(int*)(tex + 48);
    j->tdata = *(unsigned char**)(tgt + 32), j->trb = *(int*)(tgt + 40);
    j->d = d;
    return true;
}

int OurRow(uintptr_t d, uintptr_t thread, uintptr_t rowArg)
{
    const a3d::SpanJob* j = nullptr;
    for (const a3d::SpanJob& s : g_jobs)
        if (s.d == d) {
            j = &s;
            break;
        }
    if (!j) return 2;
    const int row = (int)rowArg;
    const int y = row + *(int*)(d + 44);
    if (((y & 1) != 2 - *(int*)(d + 48))) {
        const short* sp = (const short*)(**(uintptr_t**)(d + 32)) + 2 * row;
        const int x0 = sp[0], len = sp[1] - sp[0];
        if (len > 0) rnd::AeDrawSpan8(j->p, j->tex, x0, x0 + len, y, j->tdata + (ptrdiff_t)y * j->trb);
        if (!(int)thread) {
            const unsigned count = *(unsigned*)(d + 40);
            if ((row & 3) == 0 || (unsigned)row == count) {
                const uintptr_t pr = *(uintptr_t*)(*(uintptr_t*)d + 208);
                (*(void (**)(uintptr_t, uintptr_t, uintptr_t))(*(uintptr_t*)pr + 8))(pr, (unsigned)row, count);
            }
        }
    }
    return 0;
}

typedef int (*IterFnO)(uintptr_t, uintptr_t, uintptr_t);
thread_local IterFnO t_origIter = nullptr;
thread_local bool t_aeGpu = false;  // this draw's rows on the GPU (else on AE's threads with our row callback)
// copies drawing fewer pixels are left to AE (see OurIterate). 0: with the texture kept on the GPU, the GPU and AE
// were equally fast for the user's small flowers (Test A 100 / 380 copies 42-44 / 109-112 ms either way)
constexpr long long kGpuMinPx = 0;
// RANDAMU_A3D_TIME=1 (diagnostics): the last GPU draw's times on this thread, logged per copy by HookA3dDraw
thread_local rnd::GpuAeSpanTimes t_gt;
thread_local double t_gtAll = 0;
thread_local int t_gtRows = 0;
thread_local std::chrono::steady_clock::time_point t_iterIn, t_iterOut;
thread_local double t_helperMs[4];  // see HookNewMatching etc.
thread_local int t_helperN[4];
LogLimit g_gpuLog;

// a group copy's rows and state, drawn at the end of its group
struct GroupJob {
    rnd::GpuAeSpanJob gj;
    std::vector<int> rows;  // (y, x0, x1) per row
    rnd::AeSpanParams p;    // the same on the CPU (a GPU failure)
    rnd::AeTex8 tex;
    // the texture's pixels: AE disposes a surface's texture at the end of its draw, before the group is drawn
    std::shared_ptr<std::vector<unsigned char>> texKeep;
};
thread_local std::vector<GroupJob> t_groupJobs;
// the last kept texture (the group's copies share one: kept once)
std::shared_ptr<std::vector<unsigned char>> KeepTexture(const rnd::AeTex8& t)
{
    const size_t bytes = (size_t)t.rowbytes * t.height;
    if (!t_groupJobs.empty()) {
        const GroupJob& last = t_groupJobs.back();
        const rnd::AeTex8& k = last.tex;
        bool same = k.width == t.width && k.height == t.height && k.rowbytes == t.rowbytes && last.texKeep;
        for (int y = 0; same && y < t.height; y++)
            same = !std::memcmp(k.data + (size_t)y * t.rowbytes, t.data + (size_t)y * t.rowbytes, (size_t)t.width * 4);
        if (same) return last.texKeep;
    }
    return std::make_shared<std::vector<unsigned char>>(t.data, t.data + bytes);
}
LogLimit g_flushLog;
void DropGroupJobs() { t_groupJobs.clear(); }
// draws the kept jobs in order: on the GPU together, else one by one, else on the CPU (AE's arithmetic either way)
void FlushGroupJobs()
{
    if (t_groupJobs.empty()) return;
    std::vector<rnd::GpuAeSpanJob> js;
    js.reserve(t_groupJobs.size());
    for (GroupJob& g : t_groupJobs) {
        g.gj.rows = g.rows.data();
        g.gj.nRows = (int)g.rows.size() / 3;
        js.push_back(g.gj);
    }
    double ms = 0;
    bool ok = rnd::GpuAeSpansBatch(js.data(), (int)js.size(), &ms);
    int n = 0;
    if (g_flushLog.Take(5, true, &n)) RLog("group: %zu copies on the GPU together %s in %.2f ms", js.size(), ok ? "ok" : "FAILED", ms);
    if (!ok) {
        size_t k = 0;
        for (; k < js.size() && rnd::GpuAeSpans(js[k]); k++) {
        }
        for (; k < js.size(); k++) {  // the rest on the CPU
            const GroupJob& g = t_groupJobs[k];
            for (size_t r = 0; r + 2 < g.rows.size(); r += 3)
                rnd::AeDrawSpan8(g.p, g.tex, g.rows[r + 1], g.rows[r + 2], g.rows[r],
                                 g.gj.target + (ptrdiff_t)g.rows[r] * g.gj.targetRowbytes);
        }
    }
    t_groupJobs.clear();
}

int OurIterate(uintptr_t n, uintptr_t d, uintptr_t cb)
{
    static const bool s_gtr = GetEnvironmentVariableA("RANDAMU_GROUP_TRACE", nullptr, 0) > 0;
    if (s_gtr && t_inGroupIter) RLog("group trace: nested iterate (%llu rows)", (unsigned long long)n);
    t_iterIn = t_gIterIn = std::chrono::steady_clock::now();
    if (g_profMode == 1) g_profTid = 0;
    struct Out {
        ~Out() { t_iterOut = t_gIterOut = std::chrono::steady_clock::now(); }
    } out;
    a3d::SpanJob job;
    const char* why = "row path";
    if (cb != g_a3dLo + a3d::kRowPolygonRva || !SpanJobFor(d, &job, &why)) {
        if (g_oursSkip.Take(20)) RLog("a3d ours: AE draws this copy (%s)", why);
        g_aeDraws++;
        if (t_inGroup) FlushGroupJobs();  // the group's kept copies first: AE draws this one into the target now
        return t_origIter(n, d, cb);
    }
    // RANDAMU_GROUP_NOGPU=1 (diagnostics): a group's copies are not drawn (the cost of AE's draw setup alone)
    static const bool s_noGpu = GetEnvironmentVariableA("RANDAMU_GROUP_NOGPU", nullptr, 0) > 0;
    if (s_noGpu && t_inGroup) return 0;
    if (t_aeGpu) {
        // all rows on the GPU at once (sub_180016900's rows: y = row + first y, every field row, AE's spans)
        const int count = (int)n;
        std::vector<int> tri;
        tri.reserve(3 * (size_t)count);
        const short* sp = (const short*)(**(uintptr_t**)(d + 32));
        const int yFirst = *(int*)(d + 44), field = *(int*)(d + 48);
        long long px = 0;
        for (int row = 0; row < count; row++) {
            const int y = row + yFirst;
            if ((y & 1) == 2 - field) continue;
            const int x0 = sp[2 * row], len = sp[2 * row + 1] - sp[2 * row];
            if (len > 0) tri.push_back(y), tri.push_back(x0), tri.push_back(x0 + len), px += len;
        }
        // a small copy: AE's own loop (the same arithmetic on its threads) is faster than a GPU round trip
        // (~0.45 ms however small). RANDAMU_GPU_MINPX overrides the size (0: always the GPU)
        static const long long s_minPx = [] {
            char v[16] = {0};
            return GetEnvironmentVariableA("RANDAMU_GPU_MINPX", v, sizeof(v)) > 0 ? atoll(v) : kGpuMinPx;
        }();
        if (px < s_minPx) {
            g_aeDraws++;
            if (t_inGroup) FlushGroupJobs();
            return t_origIter(n, d, cb);
        }
        rnd::GpuAeSpanJob gj;
        gj.core = rnd::AeCoreOf(job.p);
        gj.tex = job.tex;
        gj.texKey = ((unsigned long long)(uintptr_t)job.tex.data * 1000003ull) ^ (g_texGen.load() << 1) ^
                    ((unsigned long long)job.tex.width << 40) ^ (unsigned long long)job.tex.height;
        gj.rcp = g_rcp.r, gj.target = job.tdata, gj.targetRowbytes = job.trb;
        gj.rows = tri.data(), gj.nRows = (int)tri.size() / 3;
        // a group's copies: kept, drawn together at the end of the group (FlushGroupJobs) in this order
        // (RANDAMU_GROUP_BATCH=0, diagnostics: each one at once)
        static const bool s_batch = [] {
            char v[4] = {0};
            return !(GetEnvironmentVariableA("RANDAMU_GROUP_BATCH", v, sizeof(v)) > 0 && v[0] == '0');
        }();
        if (t_inGroup && gj.nRows > 0 && s_batch) {
            GroupJob pj;
            pj.gj = gj;
            pj.rows = std::move(tri);
            pj.p = job.p;
            pj.texKeep = KeepTexture(job.tex);
            pj.tex = job.tex;
            pj.tex.data = pj.texKeep->data();
            pj.gj.tex = pj.tex;
            t_groupJobs.push_back(std::move(pj));
            g_oursDraws++;
            const uintptr_t pr = *(uintptr_t*)(*(uintptr_t*)d + 208);
            for (int row = 0; row < count; row++)
                if ((row & 3) == 0 || row == count)
                    (*(void (**)(uintptr_t, uintptr_t, uintptr_t))(*(uintptr_t*)pr + 8))(pr, (unsigned)row, (unsigned)count);
            return 0;
        }
        double ms = 0;
        t_gt = rnd::GpuAeSpanTimes();
        t_gtRows = gj.nRows;
        if (gj.nRows == 0 || rnd::GpuAeSpans(gj, &ms, &t_gt)) {
            t_gtAll = ms;
            g_oursDraws++;
            // the progress AE's row callback reports (thread 0's rows)
            const uintptr_t pr = *(uintptr_t*)(*(uintptr_t*)d + 208);
            for (int row = 0; row < count; row++)
                if ((row & 3) == 0 || row == count)
                    (*(void (**)(uintptr_t, uintptr_t, uintptr_t))(*(uintptr_t*)pr + 8))(pr, (unsigned)row, (unsigned)count);
            if (g_gpuLog.Take(5, true)) RLog("a3d ours: GPU %d rows in %.2f ms", gj.nRows, ms);
            return 0;
        }
        // the GPU could not draw it: AE draws it (its own arithmetic is faster than ours on the CPU)
        if (g_gpuLog.Take(10)) RLog("a3d ours: GPU failed, AE draws this copy");
        g_aeDraws++;
        if (t_inGroup) FlushGroupJobs();
        return t_origIter(n, d, cb);
    }
    int slot = -1;
    {
        std::lock_guard<std::mutex> lk(g_jobMu);
        for (int i = 0; i < 32 && slot < 0; i++)
            if (!g_jobs[i].d) g_jobs[i] = job, slot = i;
    }
    if (t_inGroup) FlushGroupJobs();
    if (slot < 0) return t_origIter(n, d, cb);
    g_oursDraws++;
    const int e = t_origIter(n, d, (uintptr_t)&OurRow);
    std::lock_guard<std::mutex> lk(g_jobMu);
    g_jobs[slot].d = 0;
    return e;
}

uintptr_t DrawCopyOurs(uintptr_t self, uintptr_t ctx, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7)
{
    static const bool s_gtr = GetEnvironmentVariableA("RANDAMU_GROUP_TRACE", nullptr, 0) > 0;
    if (s_gtr && t_inGroupIter) RLog("group trace: DrawCopyOurs %p", (void*)self);
    IterFnO* slot = (IterFnO*)(ctx + 224);
    const IterFnO saved = t_origIter;
    t_origIter = *slot;
    *slot = &OurIterate;
    const uintptr_t r = g_origA3dDraw(self, ctx, a3, a4, a5, a6, a7);
    *slot = t_origIter;
    t_origIter = saved;
    int n = 0;
    if (g_oursLog.Take(3, true, &n)) RLog("a3d ours: %lld copies drawn by DupliX, %lld by AE", g_oursDraws.load(), g_aeDraws.load());
    return r;
}

uintptr_t HookA3dDraw(uintptr_t self, uintptr_t ctx, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7)
{
    // RANDAMU_A3D_DRAW (diagnostics): 0 AE draws everything, 1 log, 2 skip copies, 3 capture, 4 no pixel loop,
    // 5 ours on the CPU, 6 ours on the GPU. Unset: the copies of a layer with the merge on are drawn on the GPU.
    char ev[8] = {0};
    const int mode = GetEnvironmentVariableA("RANDAMU_A3D_DRAW", ev, sizeof(ev)) > 0 ? atoi(ev) : -1;
    if (!mode) return g_origA3dDraw(self, ctx, a3, a4, a5, a6, a7);
    if (mode < 0) {
        static const bool s_time = GetEnvironmentVariableA("RANDAMU_A3D_TIME", ev, sizeof(ev)) > 0 && ev[0] == '1';
        static const bool s_prof = GetEnvironmentVariableA("RANDAMU_A3D_PROF", ev, sizeof(ev)) > 0 && ev[0] >= '1';
        if (g_profMode == 2) g_profTid = GetCurrentThreadId();
        const auto tA = std::chrono::steady_clock::now();
        auto since = [](std::chrono::steady_clock::time_point t) {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
        };
        static const bool s_gtr = GetEnvironmentVariableA("RANDAMU_GROUP_TRACE", nullptr, 0) > 0;
        if (s_gtr && t_inGroup) RLog("group trace: nested draw %p enter", (void*)self);
        const void* L = nullptr;
        int sb = -9;
        void* rf = nullptr;
        void* tr = nullptr;
        void** rcx = *(void***)(ctx + a3d::kCtxRenderContext);
        if (g_extractRefcon && g_nthLayer && !g_extractRefcon(rcx, &rf) &&
            !g_nthLayer(rf, *(int*)(self + a3d::kSurfLayerIndex), &tr) && tr) {
            char* v = *(char**)((char*)tr + bee::kTraceArtLayer);
            if (v) L = *(void**)(v + bee::kArtLayerToLayer), sb = *(int*)(v + bee::kArtLayerToSubIndex);
        }
        ParamSnapshot s;
        int seen = L && sb >= 0 ? MergeSeenOf(L) : 0;
        if (seen < 0) seen = IsMine(L) && ParamsFor(L, nullptr, &s) && s.p.merge ? 1 : 0;
        const double lookMs = s_time ? since(tA) : 0;
        const bool ours = seen > 0 && !OldMerge() && IsMine(L);
        if (s_gtr && t_inGroup) RLog("group trace: nested draw looked up (L %p sub %d seen %d ours %d)", L, sb, seen, (int)ours);
        // the shared textures (HookTex) live for one frame's drawing on this thread: sub_180012B30 counts the
        // surfaces drawn so far at ctx+kCtxProgressCount (0 at a frame's first surface) out of ctx+kCtxProgressTotal
        if (ctx != t_shareCtx || *(int*)(ctx + a3d::kCtxProgressCount) == 0) DropSharedTextures(), t_shareCtx = ctx;
        if (t_passDraws++ == 0) t_passFirstDraw = std::chrono::steady_clock::now();
        struct InDraw {
            uintptr_t ctx;
            bool prev;
            InDraw(uintptr_t c, bool on) : ctx(c), prev(t_shareOn) { ++t_inA3dDraw, t_shareOn = on; }
            ~InDraw()
            {
                --t_inA3dDraw, t_shareOn = prev;
                if (!t_inGroup && *(int*)(ctx + a3d::kCtxProgressCount) >= *(int*)(ctx + a3d::kCtxProgressTotal)) {
                    DropSharedTextures();
                    ClosePass();
                    if (g_profMode == 3) g_profTid = 0;
                }
            }
        } inDraw(ctx, ours);
        // RANDAMU_A3D_AEPIX=1 (diagnostics): the shared textures, but AE draws the pixels
        static const bool s_aePix = GetEnvironmentVariableA("RANDAMU_A3D_AEPIX", ev, sizeof(ev)) > 0 && ev[0] == '1';
        // the group drawing: the representative of a group draws the whole group
        if (ours && !t_inGroup && GroupDrawOn() && IsMine(L)) {
            ParamSnapshot gs;
            LayerKey gk;
            const int* tm = KeyOf(L, &gk) ? ArtTimeOf(gk) : nullptr;
            const MergeLayerPlan* lp = nullptr;
            std::shared_ptr<const MergePlan> plan;
            if (tm && ParamsFor(L, tm, &gs)) plan = MergePlanOf(L, tm, gs, false, &lp);
            static LogLimit gl;
            if (gl.Take(10))
                RLog("group: draw sub %d tm %d plan %d subs %d", sb, tm ? tm[0] : -1, (int)(bool)plan, lp ? (int)lp->subs.size() : -1);
            if (lp && sb >= 0 && sb < (int)lp->subs.size() && lp->subs[sb].cluster >= 0 &&
                lp->subs[sb].cluster < (int)lp->clusters.size()) {
                t_aeGpu = true;
                const MergeCluster& gc = lp->clusters[lp->subs[sb].cluster];
                if (gc.place && !WasMoved(ctx, self)) {
                    static LogLimit nm;
                    if (nm.Take(20)) RLog("group: a %s group's representative was not moved (drawn at its place)", gc.place > 0 ? "front" : "back");
                }
                const long long tg0 = TimingNow();
                const bool drewGroup = DrawGroup(self, ctx, a7, L, gs, gc, lp->subs[sb].copy);
                // RANDAMU_TIMING: the frame's span reaches the end of the group's drawing (the hooks before it end
                // earlier: without this the span left the group out)
                MarkTiming(L, tm, TK_HOOK, tg0);
                if (drewGroup) {
                    // what the representative's own draw does last: the progress count
                    const int pn = ++*(int*)(ctx + a3d::kCtxProgressCount);
                    void*** pr = *(void****)(ctx + a3d::kCtxProgress);
                    ((void (*)(void*, uintptr_t, uintptr_t))(**pr))(pr, (uintptr_t)(unsigned)pn,
                                                                     *(unsigned*)(ctx + a3d::kCtxProgressTotal));
                    return 0;
                }
            }
        }
        if (ours && !s_aePix) {
            t_aeGpu = true;
            t_gt = rnd::GpuAeSpanTimes(), t_gtAll = -1, t_gtRows = 0;
            const auto tB = std::chrono::steady_clock::now();
            t_iterIn = t_iterOut = tB;
            for (int k = 0; k < 4; k++) t_helperMs[k] = 0, t_helperN[k] = 0;
            if (s_prof) g_profTid = GetCurrentThreadId();
            static const bool s_clone = GetEnvironmentVariableA("RANDAMU_GROUP_CLONE", ev, sizeof(ev)) > 0 && ev[0] == '1';
            alignas(16) unsigned char clone[a3d::kSurfaceSize], entryBuf[a3d::kEntrySize];
            alignas(16) double cm4[16];
            const bool useClone = s_clone && CloneForCheck(self, ctx, L, sb, clone, entryBuf, cm4);
            const long long tc0 = TimingNow();
            const uintptr_t r = DrawCopyOurs(useClone ? (uintptr_t)clone : self, ctx, a3, a4, a5, a6, a7);
            if (TimingEnabled() && !t_inGroup) {
                LayerKey ck;
                if (KeyOf(L, &ck)) MarkTiming(L, ArtTimeOf(ck), TK_HOOK, tc0);
            }
            if (useClone) FreeSurface(clone);
            if (g_profMode == 1) g_profTid = 0;
            if (s_time && !t_inGroup) {
                auto d = [](std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
                    return std::chrono::duration<double, std::milli>(b - a).count();
                };
                RLog("a3d time: ours sub %d kind %d look %.3f draw %.3f setup %.3f iter %.3f after %.3f gpu-call %.3f (tex %.3f "
                     "stage %.3f gpu %.3f) rows %d px %lld helpers new %.3f/%d copy %.3f/%d itergen %.3f/%d purge %.3f/%d",
                     sb, *(int*)a7, lookMs, since(tB), d(tB, t_iterIn), d(t_iterIn, t_iterOut), since(t_iterOut), t_gtAll,
                     t_gt.texMs, t_gt.stageMs, t_gt.gpuMs, t_gtRows, t_gt.regionPx, t_helperMs[0], t_helperN[0], t_helperMs[1],
                     t_helperN[1], t_helperMs[2], t_helperN[2], t_helperMs[3], t_helperN[3]);
            }
            return r;
        }
        const auto tB = std::chrono::steady_clock::now();
        const uintptr_t r = g_origA3dDraw(self, ctx, a3, a4, a5, a6, a7);
        if (s_time) RLog("a3d time: ae sub %d kind %d look %.3f draw %.3f", sb, *(int*)a7, lookMs, since(tB));
        return r;
    }
    const int index = *(int*)(self + a3d::kSurfLayerIndex);
    void* refcon = nullptr;
    void* trace = nullptr;
    const void* layer = nullptr;
    int sub = -9;
    void** rc = *(void***)(ctx + a3d::kCtxRenderContext);
    if (g_extractRefcon && g_nthLayer && !g_extractRefcon(rc, &refcon) && !g_nthLayer(refcon, index, &trace) && trace) {
        char* v = *(char**)((char*)trace + bee::kTraceArtLayer);
        if (v) layer = *(void**)(v + bee::kArtLayerToLayer), sub = *(int*)(v + bee::kArtLayerToSubIndex);
    }
    const bool mine = layer && sub >= 0 && IsMine(layer);
    LayerKey k = {};
    if (layer) KeyOf(layer, &k);
    const long seq = g_a3dDrawSeq.fetch_add(1);
    auto logIt = [&](double ms) {
        if (!g_a3dDrawLog.Take(4000)) return;
        // a7 -> {int kind, the target buffer} (sub_180014470: v39 = 1, v40 = target)
        const unsigned* t = a7 ? *(const unsigned**)(a7 + 8) : nullptr;
        static const unsigned zero[12] = {0};
        if (!t) t = zero;
        RLog("a3d draw: seq %ld tid %lu ra +%llx idx %d comp %d layer %d sub %d mine %d%s %.2f ms a3 %p [%08x %08x %08x "
             "%08x %08x %08x %08x %08x %08x %08x %08x %08x] a4 %llx a6 %llx a7 %d",
             seq, GetCurrentThreadId(), (unsigned long long)((uintptr_t)_ReturnAddress() - g_a3dLo), index, k.compItemId,
             k.layerId, sub, (int)mine, mode == 2 && mine ? " SKIP" : "", ms, (const void*)t, t[0], t[1], t[2], t[3], t[4],
             t[5], t[6], t[7], t[8], t[9], t[10], t[11], (unsigned long long)a4, (unsigned long long)a6,
             a7 ? *(int*)a7 : -1);
    };
    if (mode == 2 && mine) {
        logIt(0);
        // what the draw does last: the progress count
        const int n = ++*(int*)(ctx + a3d::kCtxProgressCount);
        void*** pr = *(void****)(ctx + a3d::kCtxProgress);
        ((void (*)(void*, uintptr_t, uintptr_t))(**pr))(pr, (uintptr_t)(unsigned)n, *(unsigned*)(ctx + a3d::kCtxProgressTotal));
        return 0;
    }
    // RANDAMU_A3D_DRAW=3: capture the first draw of copy RANDAMU_A3D_CAPSUB (default 0) into RANDAMU_A3D_CAPDIR:
    // the draw's raster state and texture, the surface, and every row's target span before and after AE drew it
    // (the reference for an exact copy of AE's span drawing)
    // RANDAMU_A3D_CAPSUB may list several copies ("0,5,9"); each goes to RANDAMU_A3D_CAPDIR\s<copy>
    static std::mutex s_capDoneMu;
    static std::vector<int> s_capDone;
    static bool s_cap = false;
    static std::string s_capDir;
    static std::mutex s_capMu;
    struct CapRow {
        int row, y, x0, x1;
        std::vector<unsigned char> before, after;
    };
    static std::vector<CapRow> s_capRows;
    s_cap = false;
    if (mode == 3 && mine) {
        char sv[256] = {0}, dir[512] = {0};
        bool listed = false;
        if (GetEnvironmentVariableA("RANDAMU_A3D_CAPSUB", sv, sizeof(sv)) > 0) {
            for (const char* q = sv; *q;) {
                if (atoi(q) == sub) listed = true;
                const char* c = strchr(q, ',');
                if (!c) break;
                q = c + 1;
            }
        } else {
            listed = sub == 0;
        }
        // RANDAMU_A3D_CAPW: only a target of this width (buffer: +32 data, +40 rowbytes, +44 width, +48 height)
        const int wantW = GetEnvironmentVariableA("RANDAMU_A3D_CAPW", sv, sizeof(sv)) > 0 ? atoi(sv) : 0;
        const unsigned char* tgt = *(int*)a7 == 1 ? *(const unsigned char**)(a7 + 8) : nullptr;
        const bool widthOk = !wantW || (tgt && *(const int*)(tgt + 44) == wantW);
        bool first = false;
        if (listed && widthOk) {
            std::lock_guard<std::mutex> lk(s_capDoneMu);
            first = std::find(s_capDone.begin(), s_capDone.end(), sub) == s_capDone.end();
            if (first) s_capDone.push_back(sub);
        }
        if (first && GetEnvironmentVariableA("RANDAMU_A3D_CAPDIR", dir, sizeof(dir)) > 0) {
            CreateDirectoryA(dir, nullptr);
            s_cap = true, s_capDir = std::string(dir) + "\\s" + std::to_string(sub);
            s_capRows.clear();
            CreateDirectoryA(s_capDir.c_str(), nullptr);
            auto dump = [&](const char* name, const void* p, size_t n) {
                FILE* f = fopen((s_capDir + "\\" + name).c_str(), "wb");
                if (f) fwrite(p, 1, n, f), fclose(f);
            };
            dump("surface.bin", (const void*)self, 232);
            dump("ctx.bin", (const void*)ctx, 512);
            const unsigned char* tb = *(const unsigned char**)(a7 + 8);
            dump("target_hdr.bin", tb, 128);
            RLog("a3d capture: sub %d into %s (kind %d)", sub, s_capDir.c_str(), *(int*)a7);
        }
    }
    // RANDAMU_A3D_DRAW=5: the copy's pixels drawn by DupliX with AE's arithmetic
    if ((mode == 5 || mode == 6) && mine) {
        t_aeGpu = mode == 6;
        return DrawCopyOurs(self, ctx, a3, a4, a5, a6, a7);
    }
    // RANDAMU_A3D_DRAW=4: AE's draw of a copy without its pixel loop (the setup's cost)
    if (mode == 4 && mine) {
        typedef int (*IterFn4)(uintptr_t, uintptr_t, uintptr_t);
        IterFn4* slot4 = (IterFn4*)(ctx + 224);
        const IterFn4 orig4 = *slot4;
        *slot4 = [](uintptr_t, uintptr_t, uintptr_t) { return 0; };
        const uintptr_t r4 = g_origA3dDraw(self, ctx, a3, a4, a5, a6, a7);
        *slot4 = orig4;
        return r4;
    }
    // the draw's pixel loop: the ctx's iterate function (+224, rows on AE's threads); its time apart from the setup
    typedef int (*IterFn)(uintptr_t, uintptr_t, uintptr_t);
    static thread_local IterFn t_iter = nullptr;
    static thread_local double t_iterMs = 0;
    IterFn* slot = (IterFn*)(ctx + 224);
    t_iter = *slot;
    t_iterMs = 0;
    // the row callback's own time summed over AE's threads (experiment: MFR off, one draw at a time)
    typedef int (*RowFn)(uintptr_t, uintptr_t, uintptr_t);
    static RowFn s_row = nullptr;
    static std::atomic<long long> s_rowNs{0};
    static std::atomic<int> s_rows{0};
    s_rowNs = 0, s_rows = 0;
    *slot = [](uintptr_t n, uintptr_t d, uintptr_t cb) {
        const auto s = std::chrono::steady_clock::now();
        s_row = (RowFn)cb;
        if (s_cap) {
            auto dump = [&](const char* name, const void* p, size_t sz) {
                FILE* f = fopen((s_capDir + "\\" + name).c_str(), "wb");
                if (f) fwrite(p, 1, sz, f), fclose(f);
            };
            dump("d.bin", (const void*)d, 64);
            const unsigned char* st = *(const unsigned char**)(d + 8);
            dump("state.bin", st, 1024);
            const unsigned char* w = *(const unsigned char**)(st + 16);  // the texture's PF_World
            if (w) {
                dump("tex_hdr.bin", w, 128);
                const unsigned char* data = *(const unsigned char**)(w + 32);
                const int rb = *(const int*)(w + 40), ww = *(const int*)(w + 44), hh = *(const int*)(w + 48);
                if (data && rb > 0 && ww > 0 && hh > 0 && hh < 20000 && rb < (1 << 20)) dump("tex.bin", data, (size_t)rb * hh);
                RLog("a3d capture: texture %dx%d rowbytes %d", ww, hh, rb);
            }
            const unsigned char* ent = *(const unsigned char**)(st + 32);
            const int ne = *(const int*)(st + 40);
            if (ent && ne > 0 && ne < 64) dump("samples.bin", ent, (size_t)72 * ne);
            RLog("a3d capture: iterate n %llu cb +%llx samples %d", (unsigned long long)n,
                 (unsigned long long)(cb - g_a3dLo), ne);
            // the span drawing's other inputs: the layer's render params (d+16: xfer mode +0x18, opacity +0x20,
            // lights +0x8C, its vfunc 0), the mask rows (d+24), the blending tables (ctx+480), and the CPU flag the
            // samplers branch on (U_G_CPU_DataP -> +8)
            const unsigned char* lp = *(const unsigned char**)(d + 16);
            if (lp) {
                dump("params.bin", lp, 256);
                const uintptr_t f0 = **(const uintptr_t**)lp;
                RLog("a3d capture: params vfunc0 +%llx xfer %d opacity %.6f lights %d quality-ish %d", (unsigned long long)(f0 - g_a3dLo),
                     lp[0x18], *(const double*)(lp + 0x20), lp[0x8C], lp[0x5B]);
            }
            const unsigned char* mk = *(const unsigned char**)(d + 24);
            if (mk) dump("mask_hdr.bin", mk, 128);
            const uintptr_t ctx0 = **(const uintptr_t**)d;
            RLog("a3d capture: mask %p tables %p", (const void*)mk, *(void**)(ctx0 + 480));
            HMODULE u = GetModuleHandleA("U.dll");
            void* cpuP = u ? (void*)GetProcAddress(u, "U_G_CPU_DataP") : nullptr;
            if (cpuP) {
                const unsigned char* cd = *(const unsigned char**)cpuP;  // the variable holds the data pointer
                RLog("a3d capture: U_G_CPU_DataP +8 = %d", cd ? *(const int*)(cd + 8) : -1);
                if (cd) dump("cpu.bin", cd, 64);
            } else {
                RLog("a3d capture: U_G_CPU_DataP not found");
            }
            // PF.dll's CPU-dispatched pointers: the area sampler (qword_180425A28) and the in-front transfer
            HMODULE pf = GetModuleHandleA("PF.dll");
            if (pf) {
                const uintptr_t pfb = (uintptr_t)pf;
                typedef void* (*GetXferFn)(int);
                auto gx = (GetXferFn)GetProcAddress(pf, "?PF_Get_Xfer1_Opacity_table8@@YAP6AXPEBVPF_Pixel8@@PEBUPF_LinearBlendingTables@@EPEAV1@@ZH@Z");
                const uintptr_t area = *(const uintptr_t*)(pfb + 0x425A28), area1 = *(const uintptr_t*)(pfb + 0x425A18);
                RLog("a3d capture: PF qword_180425A18 -> +%llx", (unsigned long long)(area1 - pfb));
                RLog("a3d capture: PF base %p area sampler +%llx xfer[%d] +%llx", (void*)pf, (unsigned long long)(area - pfb),
                     lp ? lp[0x18] & 0x7F : -1, gx && lp ? (unsigned long long)((uintptr_t)gx(lp[0x18] & 0x7F) - pfb) : 0ull);
            }
            if (lp) {
                const unsigned char* b0 = *(const unsigned char**)(lp + 0xB0);
                if (b0) dump("params_b0.bin", b0, 64);
            }
        }
        const int e = t_iter(n, d, (uintptr_t)(RowFn)[](uintptr_t a, uintptr_t b, uintptr_t c) {
            const auto r0 = std::chrono::steady_clock::now();
            if (s_cap && (uintptr_t)s_row == g_a3dLo + 0x16900) {
                // sub_180016900: spans **(d+32) (int16 x0, x1 per row), y = row + *(d+44), target a7 = *(d+56)
                const short* sp = (const short*)(**(const uintptr_t**)(a + 32)) + 2 * c;
                CapRow cr;
                cr.row = (int)c, cr.y = (int)c + *(const int*)(a + 44), cr.x0 = sp[0], cr.x1 = sp[1];
                const uintptr_t a7p = *(const uintptr_t*)(a + 56);
                unsigned char* row = nullptr;
                if (*(const int*)a7p == 1) {
                    const unsigned char* t = *(const unsigned char**)(a7p + 8);
                    row = *(unsigned char**)(t + 32) + (ptrdiff_t)cr.y * *(const int*)(t + 40);
                }
                if (row && cr.x1 > cr.x0) cr.before.assign(row + 4 * cr.x0, row + 4 * cr.x1);
                const int x = s_row(a, b, c);
                if (row && cr.x1 > cr.x0) cr.after.assign(row + 4 * cr.x0, row + 4 * cr.x1);
                s_rows++;
                std::lock_guard<std::mutex> lk(s_capMu);
                s_capRows.push_back(std::move(cr));
                return x;
            }
            const int x = s_row(a, b, c);
            s_rowNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - r0).count();
            s_rows++;
            return x;
        });
        t_iterMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s).count();
        return e;
    };
    const auto t0 = std::chrono::steady_clock::now();
    const uintptr_t r = g_origA3dDraw(self, ctx, a3, a4, a5, a6, a7);
    *slot = t_iter;
    if (s_cap) {
        // rows.bin: per row int32 row, y, x0, x1, nbytes, then the bytes before and after
        FILE* f = fopen((s_capDir + "\\rows.bin").c_str(), "wb");
        if (f) {
            for (const CapRow& cr : s_capRows) {
                const int hdr[5] = {cr.row, cr.y, cr.x0, cr.x1, (int)cr.before.size()};
                fwrite(hdr, 4, 5, f);
                if (!cr.before.empty()) fwrite(cr.before.data(), 1, cr.before.size(), f);
                if (!cr.after.empty()) fwrite(cr.after.data(), 1, cr.after.size(), f);
            }
            fclose(f);
        }
        RLog("a3d capture: %zu rows written", s_capRows.size());
        s_cap = false;
    }
    const double all = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (g_a3dDrawLog.Take(4000))
        RLog("a3d draw time: seq %ld all %.2f ms pixels %.2f ms rows %d row-work %.2f ms", seq, all, t_iterMs,
             s_rows.load(), s_rowNs.load() / 1e6);
    logIt(all);
    return r;
}

// RANDAMU_A3D_TIME=1 (diagnostics): the time of each summed area table AE builds for a surface's texture
typedef void* (*SatCtorFn)(void*, const void*, bool);
SatCtorFn g_origSatCtor = nullptr;
LogLimit g_satLog;
void* HookSatCtor(void* self, const void* world, bool b)
{
    const auto t0 = std::chrono::steady_clock::now();
    void* r = g_origSatCtor(self, world, b);
    if (g_satLog.Take(4000)) {
        // the buffer (notes: +44 width, +48 height)
        RLog("a3d sat time: %.3f ms (%dx%d)", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
             world ? *(const int*)((const char*)world + 44) : -1, world ? *(const int*)((const char*)world + 48) : -1);
    }
    return r;
}
void** FindImportSlot(HMODULE mod, const char* dll, const char* name);

// RANDAMU_A3D_PROF=1 (diagnostics): samples the instruction pointer of the thread inside a copy's draw (between
// the draw's start and its pixel loop: AE's setup) and logs the most frequent module+offset every 4000 samples
}  // namespace
extern "C" IMAGE_DOS_HEADER __ImageBase;
namespace {
std::mutex g_profMu;
std::map<std::string, int> g_profHits;
int g_profTotal = 0;
// ras: the innermost return address in Advanced3D, the innermost and the outermost (below it) in randamu
void ProfUnwind(CONTEXT u, uintptr_t* ras, uintptr_t* frames, int* nFrames)
{
    *nFrames = 0;
    __try {
        for (int f = 0; f < 48 && u.Rip; f++) {
            frames[(*nFrames)++] = u.Rip;
            if (u.Rip >= g_a3dLo && u.Rip < g_a3dHi && g_profMode == 1) {
                ras[0] = u.Rip - g_a3dLo;
                return;
            }
            if (u.Rip >= g_a3dLo && u.Rip < g_a3dHi && !ras[0]) ras[0] = u.Rip - g_a3dLo;
            if (u.Rip >= (uintptr_t)&__ImageBase && u.Rip < (uintptr_t)&__ImageBase + 0x400000) {
                if (!ras[1]) ras[1] = u.Rip - (uintptr_t)&__ImageBase;
                ras[2] = u.Rip - (uintptr_t)&__ImageBase;
            }
            DWORD64 ib = 0;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(u.Rip, &ib, nullptr);
            if (!rf) {
                u.Rip = *(DWORD64*)u.Rsp;
                u.Rsp += 8;
            } else {
                void* hd = nullptr;
                DWORD64 ef = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, u.Rip, rf, &u, &hd, &ef, nullptr);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void ProfLoop()
{
    for (;;) {
        const DWORD tid = g_profTid.load();
        if (!tid) {
            Sleep(1);
            continue;
        }
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!h) continue;
        if (SuspendThread(h) != (DWORD)-1) {
            CONTEXT c = {};
            c.ContextFlags = CONTEXT_FULL;
            const bool ok = g_profTid.load() == tid && GetThreadContext(h, &c);
            // resumed before the unwinding (taking a lock while the thread is suspended can deadlock); the stack may
            // change meanwhile, which only spoils a sample now and then
            ResumeThread(h);
            // the innermost return address inside Advanced3D and inside randamu
            uintptr_t ras[3] = {0, 0, 0}, frames[48];
            int nf = 0;
            if (ok) ProfUnwind(c, ras, frames, &nf);
            const uintptr_t a3dRa = ras[0], ourRa = ras[1], ourOuter = ras[2];
            if (ok) {
                // the chain of modules from the leaf outwards (consecutive frames of one module once), up to 7
                std::string chain;
                HMODULE last = (HMODULE)1;
                int nm = 0;
                for (int f = 0; f < nf && nm < 7; f++) {
                    HMODULE m = nullptr;
                    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)frames[f], &m);
                    if (m == last) continue;
                    last = m;
                    char name[MAX_PATH] = "?";
                    if (m) GetModuleFileNameA(m, name, MAX_PATH);
                    const char* bn = strrchr(name, '\\');
                    std::string s = bn ? bn + 1 : name;
                    if (s.size() > 4 && s[s.size() - 4] == '.') s.resize(s.size() - 4);
                    chain += (nm ? "<" : "") + s;
                    nm++;
                }
                char key[1024];
                snprintf(key, sizeof(key), "a3d+%llx our+%llx outer+%llx %s", (unsigned long long)a3dRa, (unsigned long long)ourRa,
                         (unsigned long long)ourOuter, chain.c_str());
                if (g_profMode == 4) {  // the exact frames (module+offset), innermost first
                    std::string fr;
                    for (int f = 0; f < nf && f < 14; f++) {
                        HMODULE m = nullptr;
                        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           (LPCSTR)frames[f], &m);
                        char name[MAX_PATH] = "?";
                        if (m) GetModuleFileNameA(m, name, MAX_PATH);
                        const char* bn = strrchr(name, '\\');
                        char one[96];
                        snprintf(one, sizeof(one), " %s+%llx", bn ? bn + 1 : name, (unsigned long long)(frames[f] - (uintptr_t)m));
                        fr += one;
                    }
                    snprintf(key, sizeof(key), "%s", fr.c_str());
                }
                std::lock_guard<std::mutex> lk(g_profMu);
                g_profHits[key]++;
                if (++g_profTotal % 20000 == 0) {
                    std::vector<std::pair<int, std::string>> v;
                    for (auto& kv : g_profHits) v.push_back({kv.second, kv.first});
                    std::sort(v.rbegin(), v.rend());
                    RLog("a3d prof: %d samples", g_profTotal);
                    for (size_t i = 0; i < v.size() && i < 300; i++) RLog("a3d prof:   %6d %s", v[i].first, v[i].second.c_str());
                }
            }
        }
        CloseHandle(h);
        // ~50 us between samples
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        do QueryPerformanceCounter(&b);
        while ((b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart < 50);
    }
}

// RANDAMU_A3D_TIME=1: time in AE's helpers during one copy's draw (this thread): 0 NewMatchingWorld, 1 CopyWorld,
// 2 PF_IterateGeneric, 3 U_PurgeLoop
typedef void* (*NewMatchingFn)(const void*, int);
typedef void (*CopyWorldFn)(void*, const void*);
typedef int (*IterGenFn)(int, void*, void*);
typedef int (*PurgeLoopFn)(size_t, void*, void*, int);
NewMatchingFn g_origNewMatching = nullptr;
CopyWorldFn g_origCopyWorld = nullptr;
IterGenFn g_origIterGen = nullptr;
PurgeLoopFn g_origPurgeLoop = nullptr;
struct HelperTimer {
    int k;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~HelperTimer()
    {
        t_helperMs[k] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        t_helperN[k]++;
    }
};
void* HookNewMatching(const void* w, int f)
{
    HelperTimer t{0};
    return g_origNewMatching(w, f);
}
void HookCopyWorld(void* self, const void* w)
{
    HelperTimer t{1};
    g_origCopyWorld(self, w);
}
int HookIterGen(int n, void* r, void* fn)
{
    HelperTimer t{2};
    return g_origIterGen(n, r, fn);
}
int HookPurgeLoop(size_t n, void* fn, void* r, int f)
{
    HelperTimer t{3};
    return g_origPurgeLoop(n, fn, r, f);
}

// RANDAMU_GROUP_TRACE=1: Advanced3D's failed internal checks (U_ReportFailedVerification, then it throws 3)
typedef void (*ReportFailedFn)(int, int, int, const char*);
ReportFailedFn g_origReportFailed = nullptr;
void HookReportFailed(int a, int line, int c, const char* msg)
{
    RLog("a3d check failed: line %d (%d, %d) %s", line, a, c, msg ? msg : "(null)");
    g_origReportFailed(a, line, c, msg);
}

void InstallA3dDrawHook(HMODULE a3d)
{
    if (GetEnvironmentVariableA("RANDAMU_GROUP_TRACE", nullptr, 0) > 0) {
        void** s = FindImportSlot(a3d, "U.dll", "U_ReportFailedVerification");
        if (s) g_origReportFailed = (ReportFailedFn)*s, WritePtr(s, (void*)&HookReportFailed);
        RLog("a3d check hook: %s", s ? "installed" : "not found");
    }
    {
        char ev[8] = {0};
        if (GetEnvironmentVariableA("RANDAMU_A3D_TIME", ev, sizeof(ev)) > 0 && ev[0] == '1') {
            void** s = FindImportSlot(a3d, "PF.dll", "??0PF_SummedAreaTable@@QEAA@PEBVPF_World@@_N@Z");
            if (s) g_origSatCtor = (SatCtorFn)*s, WritePtr(s, (void*)&HookSatCtor);
            RLog("a3d sat hook: %s", s ? "installed" : "not found");
            struct H {
                const char* dll;
                const char* name;
                void** orig;
                void* fn;
            } hs[] = {{"PF.dll", "?NewMatchingWorld@PF_World@@SAPEAV1@PEBV1@H@Z", (void**)&g_origNewMatching, (void*)&HookNewMatching},
                      {"PF.dll", "?CopyWorld@PF_World@@QEAAXPEBV1@@Z", (void**)&g_origCopyWorld, (void*)&HookCopyWorld},
                      {"PF.dll", "PF_IterateGeneric", (void**)&g_origIterGen, (void*)&HookIterGen},
                      {"U.dll", "U_PurgeLoop", (void**)&g_origPurgeLoop, (void*)&HookPurgeLoop}};
            for (H& h : hs) {
                void** sl = FindImportSlot(a3d, h.dll, h.name);
                if (sl) *h.orig = *sl, WritePtr(sl, h.fn);
                RLog("a3d helper hook %s: %s", h.name, sl ? "installed" : "not found");
            }
        }
        if (GetEnvironmentVariableA("RANDAMU_A3D_PROF", ev, sizeof(ev)) > 0 && ev[0] >= '1') {
            g_profMode = ev[0] - '0';
            std::thread(ProfLoop).detach();
            RLog("a3d prof: sampler started");
        }
    }
    void** vt = (void**)((uintptr_t)a3d + a3d::kTRLayerVtRva);
    if (vt[0] != (void*)((uintptr_t)a3d + a3d::kTRLayerSlot0Rva) || vt[1] != (void*)((uintptr_t)a3d + a3d::kTRLayerDrawRva)) {
        RLog("a3d draw hook: not installed (SY_TRLayer vtable %p does not match: %p %p)", (void*)vt, vt[0], vt[1]);
        return;
    }
    HMODULE pr = GetModuleHandleA("PR.dll"), bee = GetModuleHandleA("BEE.dll");
    g_extractRefcon = pr ? (ExtractRefconFn)GetProcAddress(pr, "?PR_ExtractRenderRefcon@@YAHPEAPEAUPR_RenderContext@@PEAPEAX@Z") : nullptr;
    g_nthLayer = bee ? (NthLayerFn)GetProcAddress(bee, "?BEE_GetArtisanNthLayerToRender@@YAHPEAXHPEAPEAVBEE_RenderTrace@@@Z") : nullptr;
    rnd::AeFillRcpTable(&g_rcp);
    HMODULE u = GetModuleHandleA("U.dll");
    void* cpuP = u ? (void*)GetProcAddress(u, "U_G_CPU_DataP") : nullptr;
    const unsigned char* cd = cpuP ? *(const unsigned char**)cpuP : nullptr;
    g_cpuFlag = cd ? *(const int*)(cd + 8) : -1;
    g_origA3dDraw = (A3dDrawFn)vt[1];
    const bool ok = WritePtr(&vt[1], (void*)&HookA3dDraw);
    RLog("a3d draw hook: %s (refcon %p nth %p)", ok ? "installed" : "failed", (void*)g_extractRefcon, (void*)g_nthLayer);
    if (ok && g_extractRefcon && g_nthLayer) InstallGraphHook(a3d);
}

// ---- the group drawing: every layer entry keeps the depth-bias index it has when no copy is grouped. sub_180008710
// numbers the entries in order (+0x30 and +0x34 = the render layer's index); sub_180013160 makes each surface's depth
// bias from +0x34 (index * 1e-7), which decides where surfaces meeting in 3D cross. A group makes a layer's sub-layers
// fewer, so the following indices would shift. The last call of each entry in sub_180008710 is AEGP_GetLayerIndex
// with entry + 176: there +0x34 is set to the index the entry would have without groups (a merged layer's sub-layer:
// its first entry's index + its copy; later entries: + the copies grouped before them).
typedef A_Err (*GetLayerIndexFn)(AEGP_LayerH, A_long*);
GetLayerIndexFn g_origGetLayerIndex = nullptr;
LogLimit g_biasLog;
A_Err HookGetLayerIndex(AEGP_LayerH L, A_long* idx)
{
    const A_Err e = g_origGetLayerIndex(L, idx);
    const uintptr_t ra = (uintptr_t)_ReturnAddress();
    if (e || !idx || !t_passOpen || ra < g_a3dLo || ra >= g_a3dHi || !GroupDrawOn()) return e;
    unsigned char* entry = (unsigned char*)idx - 176;
    const int v17 = *(const int*)(entry + 0x34);
    if (*(const int*)(entry + 0x30) != v17) return e;  // not an entry being built (sanity)
    int index = v17 + t_biasShift;
    if (IsMine(L) && MergeOnFor(L)) {
        LayerKey k;
        ParamSnapshot s;
        const int* tm = KeyOf(L, &k) ? ArtTimeOf(k) : nullptr;
        const MergeLayerPlan* lp = nullptr;
        std::shared_ptr<const MergePlan> plan;
        if (tm && ParamsFor(L, tm, &s)) plan = MergePlanOf(L, tm, s, false, &lp);
        if (lp && !lp->clusters.empty()) {
            if (t_biasLayer != L) t_biasLayer = L, t_biasLayerSubs = 0, t_biasBase = v17 + t_biasShift;
            const int j = t_biasLayerSubs++;
            if (j < (int)lp->subs.size()) index = t_biasBase + lp->subs[j].copy;
            if (j == (int)lp->subs.size() - 1) t_biasShift += s.p.count - (int)lp->subs.size(), t_biasLayer = nullptr;
        }
    }
    if (index != v17) {
        *(int*)(entry + 0x34) = index;
        int n = 0;
        if (g_biasLog.Take(4, true, &n)) RLog("group: depth-bias index %d -> %d (shift %d)", v17, index, t_biasShift);
    }
    return e;
}

void InstallQualityHook()
{
    HMODULE a3d = GetModuleHandleA("Advanced3D.aex");
    AEGP_LayerSuite9* ls = nullptr;
    // Advanced3D acquires the layer suite with version 10: patch that version's table
    if (!a3d || !g_pica || g_pica->AcquireSuite(kAEGPLayerSuite, 10, (const void**)&ls) || !ls) {
        RLog("merge: quality hook not installed (Advanced3D %p)", (void*)a3d);
        return;
    }
    auto dos = (IMAGE_DOS_HEADER*)a3d;
    auto nt = (IMAGE_NT_HEADERS64*)((unsigned char*)a3d + dos->e_lfanew);
    g_a3dLo = (uintptr_t)a3d;
    g_a3dHi = g_a3dLo + nt->OptionalHeader.SizeOfImage;
    void** slot = (void**)&ls->AEGP_GetLayerQuality;
    g_origGetQuality = (GetLayerQualityFn)*slot;
    bool ok = WritePtr(slot, (void*)&HookGetLayerQuality);
    RLog("merge: quality hook %s (table %p slot %p)", ok ? "installed" : "failed", (void*)ls, (void*)slot);
    void** islot = (void**)&ls->AEGP_GetLayerIndex;
    g_origGetLayerIndex = (GetLayerIndexFn)*islot;
    RLog("group: layer index hook %s", WritePtr(islot, (void*)&HookGetLayerIndex) ? "installed" : "failed");
    InstallDownsampleHook(g_a3dLo, g_a3dHi);
    InstallA3dDrawHook(a3d);
}

// ---- installation
int FindSlot(void** vt, HMODULE bee, const char* mangled)
{
    void* fn = (void*)GetProcAddress(bee, mangled);
    if (!fn) return -1;
    for (int i = 0; i < bee::kVtableScanSlots; i++)
        if (vt[i] == fn) return i;
    return -1;
}

void** FindImportSlot(HMODULE mod, const char* dll, const char* name)
{
    auto base = (unsigned char*)mod;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((const char*)(base + d->Name), dll)) continue;
        auto names = (IMAGE_THUNK_DATA64*)(base + d->OriginalFirstThunk);
        auto iat = (IMAGE_THUNK_DATA64*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (!std::strcmp((const char*)ibn->Name, name)) return (void**)&iat->u1.Function;
        }
    }
    return nullptr;
}

// The vtable slot of the first `call [rax+disp]` in fn (MSVC: FF 50 disp8 / FF 90 disp32), or -1.
int FirstVirtualCallSlot(const void* fn)
{
    auto p = (const unsigned char*)fn;
    for (int i = 0; i + 6 <= 48; i++) {
        if (p[i] == 0xFF && p[i + 1] == 0x90) return (int)(*(const int*)(p + i + 2) / 8);
        if (p[i] == 0xFF && p[i + 1] == 0x50) return (int)((signed char)p[i + 2] / 8);
        if (p[i] == 0xC3) break;  // ret
    }
    return -1;
}

bool WritePtr(void** where, void* value)
{
    DWORD old = 0;
    if (!VirtualProtect(where, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *where = value;
    VirtualProtect(where, sizeof(void*), old, &old);
    return true;
}

}  // namespace

HookStatus InstallHooks(SPBasicSuite* pica, AEGP_PluginID pluginId)
{
    if (g_status != HookStatus::NotInstalled) return g_status;
    g_mainThread = GetCurrentThreadId();
    g_pica = pica;
    g_pluginId = pluginId;
    HMODULE bee = GetModuleHandleA("BEE.dll");
    HMODULE u = GetModuleHandleA("U.dll");
    HMODULE drv = GetModuleHandleA(bee::kAEGPDriverModule);
    void** vt = bee ? (void**)GetProcAddress(bee, bee::kAVLayerVtable) : nullptr;
    int sIs = -1, sSep = -1, sCnt = -1, sRc = -1;
    void** iat = nullptr;
    if (vt) {
        sIs = FindSlot(vt, bee, bee::kIsLayerType);
        sSep = FindSlot(vt, bee, bee::kSubLayersRenderSeparately);
        sCnt = FindSlot(vt, bee, bee::kSubLayerRenderCount);
        sRc = FindSlot(vt, bee, bee::kGetSubLayerRects);
    }
    if (drv) iat = FindImportSlot(drv, "BEE.dll", bee::kGetArtisanLayerOpacity);
    g_layerGetId = bee ? (LayerGetIdFn)GetProcAddress(bee, bee::kLayerGetID) : nullptr;
    g_layerGetComp = bee ? (LayerGetCompFn)GetProcAddress(bee, bee::kLayerGetParentComp) : nullptr;
    g_itemGetId = bee ? (ItemGetIdFn)GetProcAddress(bee, bee::kItemGetID) : nullptr;
    g_betaFn = bee ? (uintptr_t)GetProcAddress(bee, bee::kBetaExpressionEffects) : 0;
    g_m3Identity = u ? (M3IdentityFn)GetProcAddress(u, bee::kM3SetIdentity) : nullptr;
    g_hasEffectApplied = bee ? (HasEffectAppliedFn)GetProcAddress(bee, bee::kLayerHasEffectApplied) : nullptr;
    if (bee) {
        g_compIsCalder = (CompBoolFn)GetProcAddress(bee, bee::kCompIsCalder);
        g_compIsErnst = (CompBoolFn)GetProcAddress(bee, bee::kCompIsErnst);
        g_compIsEscher = (CompBoolFn)GetProcAddress(bee, bee::kCompIsEscher);
        g_compIsPicasso = (CompBoolFn)GetProcAddress(bee, bee::kCompIsPicasso);
    }
    HMODULE flt = GetModuleHandleA(bee::kFltModule);
    g_paradeNum = flt ? (ParadeNumSeqsFn)GetProcAddress(flt, bee::kParadeNumSeqs) : nullptr;
    g_paradeGet = flt ? (ParadeGetEffectFn)GetProcAddress(flt, bee::kParadeGetEffect) : nullptr;
    g_effectActive = flt ? (EffectIsActiveFn)GetProcAddress(flt, bee::kEffectIsActiveVideo) : nullptr;
    if (vt) {
        g_slotParade = FindSlot(vt, bee, bee::kLayerGetFilterParade);
        if (sSep >= 0) g_slotSwitches = FirstVirtualCallSlot(vt[sSep]);
    }
    int sites = (bee && g_betaFn) ? ScanFlattenSites(bee) : 0;
    RLog("hooks: flatten check sites %d", sites);
    RLog("hooks: parade slot %d switches slot %d FLT numSeqs=%p getEffect=%p isActive=%p", g_slotParade,
         g_slotSwitches, (void*)g_paradeNum, (void*)g_paradeGet, (void*)g_effectActive);
    RLog("hooks: BEE=%p vt=%p slots IsLayerType=%d Separately=%d Count=%d Rects=%d; AEGPDriver=%p iat=%p; "
         "GetID=%p GetParentComp=%p Item::GetID=%p beta=%p M3Id=%p HasEffectApplied=%p",
         (void*)bee, (void*)vt, sIs, sSep, sCnt, sRc, (void*)drv, (void*)iat, (void*)g_layerGetId,
         (void*)g_layerGetComp, (void*)g_itemGetId, (void*)g_betaFn, (void*)g_m3Identity, (void*)g_hasEffectApplied);
    if (!vt || sIs < 0 || sSep < 0 || sCnt < 0 || sRc < 0 || !iat || !g_layerGetId || !g_layerGetComp ||
        !g_itemGetId || !g_betaFn || !g_m3Identity || !g_hasEffectApplied || g_slotParade < 0 ||
        g_slotSwitches < 0 || g_slotSwitches >= bee::kVtableScanSlots || !g_paradeNum || !g_paradeGet ||
        !g_effectActive || sites < 1 || !g_compIsCalder || !g_compIsErnst || !g_compIsEscher || !g_compIsPicasso) {
        g_status = HookStatus::MissingSymbols;
        RLog("hooks: something is missing -> not installing");
        return g_status;
    }
    g_origIsLayerType = (Fwd8)vt[sIs];
    g_origSeparately = (Fwd8)vt[sSep];
    g_origCount = (Fwd8)vt[sCnt];
    g_origRects = (Fwd8)vt[sRc];
    g_origArtOpacity = (ArtOpacityFn)*iat;
    // dbg: RANDAMU_HOOKMASK (diagnosis only): 1 Separately, 2 Count, 4 Rects, 8 IsLayerType, 16 opacity
    unsigned mask = 31;
    char mb[32] = {0};
    if (GetEnvironmentVariableA("RANDAMU_HOOKMASK", mb, sizeof(mb)) > 0) mask = (unsigned)strtoul(mb, nullptr, 0);
    RLog("dbg: hook mask %u", mask);
    bool ok = (!(mask & 1) || WritePtr(&vt[sSep], (void*)&HookSeparately)) &&
              (!(mask & 2) || WritePtr(&vt[sCnt], (void*)&HookCount)) &&
              (!(mask & 4) || WritePtr(&vt[sRc], (void*)&HookRects)) &&
              (!(mask & 8) || WritePtr(&vt[sIs], (void*)&HookIsLayerType)) &&
              (!(mask & 16) || WritePtr(iat, (void*)&HookArtOpacity));
    if (ok) {
        // merge: both texture entry points, or none (then nothing merges)
        void** t1 = FindImportSlot(drv, "BEE.dll", bee::kRenderTextureForArtisan);
        void** t2 = FindImportSlot(drv, "BEE.dll", bee::kRenderTextureForArtisanWithReceipt);
        if (t1 && t2) {
            g_origTex = (TexFn)*t1;
            g_origTexRc = (TexRcFn)*t2;
            g_texHooked = WritePtr(t1, (void*)&HookTex) && WritePtr(t2, (void*)&HookTexRc);
        }
        RLog("hooks: texture entry points %p %p -> merge %s", (void*)t1, (void*)t2, g_texHooked ? "available" : "off");
        if (g_texHooked) InstallQualityHook();
    }
    g_status = ok ? HookStatus::Installed : HookStatus::MissingSymbols;
    RLog("hooks: %s (main thread %lu)", ok ? "installed" : "VirtualProtect failed", g_mainThread);
    return g_status;
}

void ResolveEffectTypeKey(const void* layer, int effectIndex)
{
    if (g_effectKey.load() >= 0 || !g_hasEffectApplied || effectIndex < 0) return;
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    if (g_effectKey.load() >= 0) return;
    // HasEffectApplied appends the matching parade indices to the vector with AE's allocator (ucrtbase
    // malloc, small blocks unaligned); free them the same way.
    static HMODULE ucrt = GetModuleHandleA("ucrtbase.dll");
    typedef void (*FreeF)(void*);
    static FreeF ufree = ucrt ? (FreeF)GetProcAddress(ucrt, "free") : nullptr;
    if (!ufree) return;
    for (int k = 0; k < 32768; k++) {
        bee::RawVec v = {nullptr, nullptr, nullptr};
        bool has = g_hasEffectApplied(layer, (short)k, &v);
        bool hit = false;
        for (const int* p = (const int*)v.first; has && p && p < (const int*)v.last; p++)
            if (*p == effectIndex) hit = true;
        if (v.first) {
            if ((size_t)(v.end - v.first) >= 4096) ufree(*((void**)v.first - 1));
            else ufree(v.first);
        }
        if (hit) {
            g_effectKey.store(k, std::memory_order_release);
            RLog("hooks: effect type key %d (parade index %d)", k, effectIndex);
            return;
        }
    }
    RLog("hooks: effect type key not found (parade index %d)", effectIndex);
}

bool EffectTypeKeyKnown() { return g_effectKey.load(std::memory_order_acquire) >= 0; }

bool LayerHasThisEffect(const void* layer)
{
    int key = g_effectKey.load(std::memory_order_acquire);
    return key < 0 || !g_hasEffectApplied || g_hasEffectApplied(layer, (short)key, nullptr);
}

// The layer is 3D and has this effect switched on, read from BEE/FLT directly (safe on any thread).
// The registry snapshot alone goes stale: AE sends the effect no command when the effect is deleted,
// switched off, or the layer's 3D switch changes, and the hooks must then answer like the original.
bool LayerIsLive(const void* layer)
{
    void** vt = *(void***)layer;
    unsigned long long sw = ((VirtGetU64Fn)vt[g_slotSwitches])(layer);
    if (!(sw & bee::kSwitch3D)) return false;
    // Classic 3D only (other renderers: the layer is drawn as is).
    const void* comp = g_layerGetComp(layer);
    if (!comp || g_compIsCalder(comp) || g_compIsErnst(comp)) {
        static std::atomic<int> dbgR{0};
        if (++dbgR <= 5)
            RLog("hook: not Classic 3D (calder=%d ernst=%d escher=%d picasso=%d): copies off",
                 comp ? (int)g_compIsCalder(comp) : -1, comp ? (int)g_compIsErnst(comp) : -1,
                 comp ? (int)g_compIsEscher(comp) : -1, comp ? (int)g_compIsPicasso(comp) : -1);
        return false;
    }
    int key = g_effectKey.load(std::memory_order_acquire);
    if (key < 0) return true;  // not resolved yet: registry only
    void* parade = ((VirtGetPtrFn)vt[g_slotParade])(layer);
    int n = 0;
    int rc = parade ? g_paradeNum(parade, &n) : -1;
    bool live = false;
    for (int i = 0; rc == 0 && i < n && !live; i++) {
        const char* eff = (const char*)g_paradeGet(parade, (unsigned)i);
        if (eff && *(const unsigned short*)(eff + bee::kEffectTypeKey) == (unsigned short)key && g_effectActive(eff))
            live = true;
    }
    static std::atomic<int> dbgN{0};
    if (++dbgN <= 20)
        RLog("dbg: LayerIsLive sw=%llx numSeqs rc=%d n=%d -> %d (escher=%d picasso=%d)", sw, rc, n, (int)live,
             (int)g_compIsEscher(comp), (int)g_compIsPicasso(comp));
    return live;
}

HookStatus GetHookStatus() { return g_status; }
bool IsMainThread() { return GetCurrentThreadId() == g_mainThread; }

}  // namespace rnd
