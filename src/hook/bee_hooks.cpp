// BEE.dll / AEGPDriver.aex replacements. A layer is "ours" when its (comp item ID, layer ID) is in
// the registry with a valid snapshot; every other layer goes straight to the original function.
#include "hook/bee_hooks.h"

#include <intrin.h>

#include <atomic>
#include <cstring>

#include "core/scatter.h"
#include "effect/randamu.h"
#include "hook/bee_abi.h"
#include "hook/flatten_site.h"
#include "hook/registry.h"

namespace rnd {

namespace {

typedef uintptr_t (*Fwd8)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef double (*ArtOpacityFn)(void*, void*, const void*, const void*);
typedef int (*LayerGetIdFn)(const void*);
typedef const void* (*LayerGetCompFn)(const void*);
typedef const int& (*ItemGetIdFn)(const void*);
typedef void (*M3IdentityFn)(void*);

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

// Registered with a valid snapshot (the effect is on, the layer is 3D, the hooks may answer).
bool IsMine(const void* layer)
{
    LayerKey k;
    ParamSnapshot s;
    return KeyOf(layer, &k) && RegLookup(k, &s) && s.valid;
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

// Reads this layer's randamu parameters and anchor at layer time t through AEGP. Render threads only.
// Returns false (leaving *out untouched) if anything is missing; "inactive" sets out->valid = false.
bool ReadParamsAegp(const void* layer, const A_Time& t, ParamSnapshot* out)
{
    Suite<AEGP_EffectSuite4> es(kAEGPEffectSuite, kAEGPEffectSuiteVersion4);
    Suite<AEGP_StreamSuite6> ss(kAEGPStreamSuite, kAEGPStreamSuiteVersion6);
    if (!es || !ss) return false;
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
    if (!eff) return false;
    bool ok = true;
    AEGP_EffectFlags flags = 0;
    if (!es->AEGP_GetEffectFlags(eff, &flags) && !(flags & AEGP_EffectFlags_ACTIVE)) {
        out->valid = false;
        es->AEGP_DisposeEffect(eff);
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
    double count = 0, seed = 0, keep = 0;
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
    es->AEGP_DisposeEffect(eff);
    AEGP_StreamRefH as = nullptr;
    if (!ss->AEGP_GetNewLayerStream(g_pluginId, L, AEGP_LayerStream_ANCHORPOINT, &as) && as) {
        AEGP_StreamValue2 val;
        std::memset(&val, 0, sizeof(val));
        if (!ss->AEGP_GetNewStreamValue(g_pluginId, as, AEGP_LTimeMode_LayerTime, &t, FALSE, &val)) {
            r.anchorX = val.val.three_d.x;
            r.anchorY = val.val.three_d.y;
            ss->AEGP_DisposeStreamValue(&val);
        } else {
            ok = false;
        }
        ss->AEGP_DisposeStream(as);
    } else {
        ok = false;
    }
    if (!ok) return false;
    r.p.count = count < 0 ? 0 : (int)(count + 0.5);
    r.p.seed = (int)(seed + 0.5);
    r.p.keepFirst = keep != 0;
    *out = r;
    return true;
}

// The parameters for `layer` at time t (t may be null). Snapshot from the effect's own commands; on
// render threads the AEGP values at t replace it.
bool ParamsFor(const void* layer, const void* tm, ParamSnapshot* out)
{
    LayerKey k;
    if (!KeyOf(layer, &k) || !RegLookup(k, out) || !out->valid) return false;
    if (tm && GetCurrentThreadId() != g_mainThread) {
        const int* ti = (const int*)tm;
        A_Time t = {ti[0], (A_u_long)(unsigned)ti[1]};
        if (t.scale == 0) t.scale = 1;
        if (!ReadParamsAegp(layer, t, out)) {
            static std::atomic<int> warned{0};
            if (++warned <= 5) RLog("hook: AEGP param read failed (comp %d layer %d), using snapshot", k.compItemId, k.layerId);
            RegLookup(k, out);
        }
    }
    return out->valid;
}

// ---- replacements
uintptr_t HookSeparately(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g,
                         uintptr_t h)
{
    if (IsMine((const void*)a)) return 1;
    return g_origSeparately(a, b, c, d, e, f, g, h);
}

// (this, const T_Time&, const shared_ptr<BEE_XformTimeCache>&, const TDB_ParamBag*)
uintptr_t HookCount(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g,
                    uintptr_t h)
{
    // Rasterize2DGraph asks with an empty cache and switches to the layer-set raster node (empty for
    // raster layers) when the answer is >= 2: give it the original answer so the whole layer is rasterised.
    bool emptyCache = c && *(void**)c == nullptr;
    if (!emptyCache && IsMine((const void*)a)) {
        ParamSnapshot s;
        if (ParamsFor((const void*)a, (const void*)b, &s)) return (uintptr_t)(unsigned)s.p.count;
    }
    return g_origCount(a, b, c, d, e, f, g, h);
}

// (this, M_FloatRect* ret, const T_Time&, const M_FloatRect& rect, vector<M_FloatRect>&, vector<M_Matrix3>&,
//  vector<M_Matrix4>&, const TDB_ParamBag*) -> ret
uintptr_t HookRects(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g,
                    uintptr_t h)
{
    ParamSnapshot s;
    if (!IsMine((const void*)a) || !ParamsFor((const void*)a, (const void*)c, &s))
        return g_origRects(a, b, c, d, e, f, g, h);
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
    for (size_t i = 0; i < n; i++) {
        std::memcpy(vR->last, rect, bee::kRectBytes);
        vR->last += bee::kRectBytes;
        g_m3Identity(v3->last);
        v3->last += bee::kM3Bytes;
        CopyXform x;
        ComputeCopy(s.p, (int)i, s.anchorX, s.anchorY, &x);
        std::memcpy(v4->last, x.m4, bee::kM4Bytes);
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

std::atomic<uintptr_t> g_siteYes[8], g_siteNo[16];
bool IsFlattenSite(uintptr_t ra)
{
    for (auto& y : g_siteYes)
        if (y.load() == ra) return true;
    for (auto& n : g_siteNo)
        if (n.load() == ra) return false;
    bool ok = MatchFlattenCheckSite((const unsigned char*)ra, g_betaFn);
    auto* arr = ok ? g_siteYes : g_siteNo;
    size_t cap = ok ? 8 : 16;
    for (size_t k = 0; k < cap; k++) {
        uintptr_t z = 0;
        if (arr[k].compare_exchange_strong(z, ra)) break;
    }
    return ok;
}

uintptr_t HookIsLayerType(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g,
                          uintptr_t h)
{
    uintptr_t ra = (uintptr_t)_ReturnAddress();
    if ((int)b == 0 && IsMine((const void*)a) && IsFlattenSite(ra)) return 0;
    return g_origIsLayerType(a, b, c, d, e, f, g, h);
}

double HookArtOpacity(void* ctx, void* trace, const void* t, const void* bag)
{
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
    RLog("hooks: BEE=%p vt=%p slots IsLayerType=%d Separately=%d Count=%d Rects=%d; AEGPDriver=%p iat=%p; "
         "GetID=%p GetParentComp=%p Item::GetID=%p beta=%p M3Id=%p",
         (void*)bee, (void*)vt, sIs, sSep, sCnt, sRc, (void*)drv, (void*)iat, (void*)g_layerGetId,
         (void*)g_layerGetComp, (void*)g_itemGetId, (void*)g_betaFn, (void*)g_m3Identity);
    if (!vt || sIs < 0 || sSep < 0 || sCnt < 0 || sRc < 0 || !iat || !g_layerGetId || !g_layerGetComp ||
        !g_itemGetId || !g_betaFn || !g_m3Identity) {
        g_status = HookStatus::MissingSymbols;
        RLog("hooks: something is missing -> not installing");
        return g_status;
    }
    g_origIsLayerType = (Fwd8)vt[sIs];
    g_origSeparately = (Fwd8)vt[sSep];
    g_origCount = (Fwd8)vt[sCnt];
    g_origRects = (Fwd8)vt[sRc];
    g_origArtOpacity = (ArtOpacityFn)*iat;
    bool ok = WritePtr(&vt[sSep], (void*)&HookSeparately) && WritePtr(&vt[sCnt], (void*)&HookCount) &&
              WritePtr(&vt[sRc], (void*)&HookRects) && WritePtr(&vt[sIs], (void*)&HookIsLayerType) &&
              WritePtr(iat, (void*)&HookArtOpacity);
    g_status = ok ? HookStatus::Installed : HookStatus::MissingSymbols;
    RLog("hooks: %s (main thread %lu)", ok ? "installed" : "VirtualProtect failed", g_mainThread);
    return g_status;
}

HookStatus GetHookStatus() { return g_status; }

}  // namespace rnd
