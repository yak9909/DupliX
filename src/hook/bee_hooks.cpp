// BEE.dll / AEGPDriver.aex replacements. A layer is "ours" when its (comp item ID, layer ID) is in
// the registry with a valid snapshot; every other layer goes straight to the original function.
#include "hook/bee_hooks.h"

#include <intrin.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "core/fast_plane.h"
#include "core/scatter.h"
#include "core/world.h"
#include "effect/randamu.h"
#include "hook/bee_abi.h"
#include "hook/flatten_site.h"
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
    r.p.count = count < 0 ? 0 : (int)(count + 0.5);
    r.p.seed = (int)(seed + 0.5);
    r.p.keepFirst = keep != 0;
    r.p.fast = fast != 0;
    r.valid = !r.p.fast;  // fast mode is drawn by the effect itself: the hooks leave the layer alone
    *out = r;
    return true;
}

namespace {

// Finds this layer's randamu effect and reads it at layer time t. Render threads only.
// Returns false if the read failed; an absent or inactive effect gives true with out->valid = false.
bool ReadParamsAegp(const void* layer, const A_Time& t, ParamSnapshot* out)
{
    Suite<AEGP_EffectSuite4> es(kAEGPEffectSuite, kAEGPEffectSuiteVersion4);
    if (!es) return false;
    AEGP_LayerH L = (AEGP_LayerH)layer;
    A_long n = 0;
    if (es->AEGP_GetLayerNumEffects(L, &n)) return false;
    AEGP_EffectRefH eff = nullptr;
    for (A_long i = 0; i < n && !eff; i++) {
        AEGP_EffectRefH e = nullptr;
        if (es->AEGP_GetLayerEffectByIndex(g_pluginId, L, i, &e) || !e) continue;
        AEGP_InstalledEffectKey key = 0;
        A_char mn[AEGP_MAX_EFFECT_MATCH_NAME_SIZE + 1] = {0};
        if (!es->AEGP_GetInstalledKeyFromLayerEffect(e, &key) && !es->AEGP_GetEffectMatchName(key, mn) &&
            !std::strcmp(mn, RANDAMU_MATCH_NAME))
            eff = e;
        else
            es->AEGP_DisposeEffect(e);
    }
    if (!eff) {  // the effect was removed: the layer is no longer ours
        out->valid = false;
        return true;
    }
    bool ok = ReadParamsFromEffect(eff, L, t, out);
    es->AEGP_DisposeEffect(eff);
    return ok;
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
    size_t n = (size_t)s.p.count;
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
        CopyXform x;
        ComputeCopy(s.p, (int)i, s.anchorX, s.anchorY, &x);
        double m[16];
        Mul4(x.m4, G, m);
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

double HookArtOpacity(void* ctx, void* trace, const void* t, const void* bag)
{
    DbgScope ds("ArtOpacity", g_dbgOp);
    Prof pf(PF_OPAC);
    long long t0 = TimingNow();
    double op = g_origArtOpacity(ctx, trace, t, bag);
    if (!trace) return op;
    char* v = *(char**)((char*)trace + bee::kTraceArtLayer);
    if (!v) return op;
    const void* layer = *(void**)(v + bee::kArtLayerToLayer);
    int index = *(int*)(v + bee::kArtLayerToSubIndex);
    ParamSnapshot s;
    if (index < 0 || !IsMine(layer) || !ParamsFor(layer, t, &s)) return op;
    CopyXform x;
    ComputeCopy(s.p, index, s.anchorX, s.anchorY, &x);
    MarkTiming(layer, t, TK_COPY, t0);
    return op * x.opacity;
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
