#include "hook/merge_scene.h"

#include <cmath>
#include <cstring>

#include "core/scatter.h"
#include "core/world.h"
#include "effect/randamu.h"
#include "hook/bee_hooks.h"
#include "hook/registry.h"

namespace rnd {

namespace {

template <class T>
struct S {
    T* s = nullptr;
    const char* name;
    int32 ver;
    S(const char* n, int32 v) : name(n), ver(v)
    {
        SPBasicSuite* p = HookPica();
        if (!p || p->AcquireSuite(n, v, (const void**)&s) != kSPNoError) s = nullptr;
    }
    ~S()
    {
        if (s) HookPica()->ReleaseSuite(name, ver);
    }
    T* operator->() const { return s; }
    explicit operator bool() const { return s != nullptr; }
};

void ToM(const A_Matrix4& m, double out[16])
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[r * 4 + c] = m.mat[r][c];
}

// One value of a layer stream at comp time (one-dimensional streams only).
bool OneD(AEGP_StreamSuite6* ss, AEGP_LayerH L, AEGP_LayerStream which, const A_Time& compT, double* v)
{
    AEGP_StreamRefH s = nullptr;
    if (ss->AEGP_GetNewLayerStream(HookPluginId(), L, which, &s) || !s) return false;
    AEGP_StreamValue2 val;
    std::memset(&val, 0, sizeof(val));
    bool ok = !ss->AEGP_GetNewStreamValue(HookPluginId(), s, AEGP_LTimeMode_CompTime, &compT, FALSE, &val);
    if (ok) {
        *v = val.val.one_d;
        ss->AEGP_DisposeStreamValue(&val);
    }
    ss->AEGP_DisposeStream(s);
    return ok;
}

int ActiveEffects(AEGP_EffectSuite4* es, AEGP_LayerH L)
{
    A_long n = 0;
    if (es->AEGP_GetLayerNumEffects(L, &n)) return 1;  // unknown: assume it has some
    int active = 0;
    for (A_long i = 0; i < n; i++) {
        AEGP_EffectRefH e = nullptr;
        if (es->AEGP_GetLayerEffectByIndex(HookPluginId(), L, i, &e) || !e) {
            active++;
            continue;
        }
        AEGP_EffectFlags fl = 0;
        if (es->AEGP_GetEffectFlags(e, &fl) || (fl & AEGP_EffectFlags_ACTIVE)) active++;
        es->AEGP_DisposeEffect(e);
    }
    return active;
}

// What the whole-layer reasons need from the comp.
struct CompFacts {
    AEGP_CompFlags compFlags;
    bool anyLight, anyShadowLight, dof;
};

// The whole-layer reasons not to merge of DupliX layer L (MergeBlockReasons; MB_UNREADABLE if a value could not
// be read). layerFlags: L's AEGP layer flags.
unsigned LayerBlock(AEGP_LayerSuite9* ls, AEGP_ItemSuite9* is, AEGP_StreamSuite6* ss, AEGP_FootageSuite5* fs,
                    AEGP_LayerH L, AEGP_LayerFlags layerFlags, const A_Time& compT, const CompFacts& c, bool otherEffects)
{
    LayerFlagsForMerge f;
    f.otherEffects = otherEffects;
    {
        // a solid: AE fills its copies without a texture
        AEGP_ItemH src = nullptr;
        AEGP_ItemType it = AEGP_ItemType_NONE;
        AEGP_FootageH foot = nullptr;
        AEGP_FootageSignature sig = AEGP_FootageSignature_NONE;
        f.solid = !ls->AEGP_GetLayerSourceItem(L, &src) && src && !is->AEGP_GetItemType(src, &it) &&
                  it == AEGP_ItemType_FOOTAGE && fs && !fs->AEGP_GetMainFootageFromItem(src, &foot) && foot &&
                  !fs->AEGP_GetFootageSignature(foot, &sig) && sig == AEGP_FootageSignature_SOLID;
    }
    f.compMotionBlur = (c.compFlags & AEGP_CompFlag_ENABLE_MOTION_BLUR) != 0;
    f.layerMotionBlur = (layerFlags & AEGP_LayerFlag_MOTION_BLUR) != 0;
    f.dof = c.dof;
    f.anyLight = c.anyLight;
    f.anyShadowLight = c.anyShadowLight;
    double accepts = 0, casts = 0, opacity = 100;
    AEGP_LayerTransferMode tm;
    std::memset(&tm, 0, sizeof(tm));
    A_Boolean matte = FALSE;
    if (!OneD(ss, L, AEGP_LayerStream_ACCEPTS_LIGHTS, compT, &accepts) ||
        !OneD(ss, L, AEGP_LayerStream_CASTS_SHADOWS, compT, &casts) || !OneD(ss, L, AEGP_LayerStream_OPACITY, compT, &opacity) ||
        ls->AEGP_GetLayerTransferMode(L, &tm) || ls->AEGP_DoesLayerHaveTrackMatte(L, &matte))
        return MB_UNREADABLE;
    f.acceptsLights = accepts != 0;
    f.castsShadows = casts != 0;
    f.fullOpacity = opacity >= 100 - 1e-9;
    f.normalBlend = tm.mode == PF_Xfer_IN_FRONT;
    f.trackMatte = matte != FALSE;
    return MergeBlockReasons(f);
}

// Pass 1 of a comp frame: the topmost active camera (null if none) and whether any light / shadow-casting light is
// active. False if something could not be read.
bool ReadCompLights(AEGP_LayerSuite9* ls, AEGP_StreamSuite6* ss, AEGP_LightSuite2* lights, AEGP_CompH comp,
                    const A_Time& compT, AEGP_LayerH* camera, bool* anyLight, bool* anyShadowLight)
{
    A_long n = 0;
    if (ls->AEGP_GetCompNumLayers(comp, &n)) return false;
    *camera = nullptr;
    *anyLight = *anyShadowLight = false;
    for (A_long i = 0; i < n; i++) {
        AEGP_LayerH Li = nullptr;
        AEGP_ObjectType type = AEGP_ObjectType_NONE;
        A_Boolean active = FALSE;
        if (ls->AEGP_GetCompLayerByIndex(comp, i, &Li) || !Li || ls->AEGP_GetLayerObjectType(Li, &type) ||
            ls->AEGP_IsVideoActive(Li, AEGP_LTimeMode_CompTime, &compT, &active))
            return false;
        if (!active) continue;
        if (type == AEGP_ObjectType_CAMERA && !*camera) *camera = Li;
        if (type == AEGP_ObjectType_LIGHT) {
            *anyLight = true;
            AEGP_LightType lt = AEGP_LightType_NONE;
            double shadows = 0;
            if (lights->AEGP_GetLightType(Li, &lt) || !OneD(ss, Li, AEGP_LayerStream_CASTS_SHADOWS, compT, &shadows))
                return false;
            if (lt != AEGP_LightType_AMBIENT && shadows != 0) *anyShadowLight = true;
        }
    }
    return true;
}

}  // namespace

bool ReadMergeBlock(AEGP_LayerH L, const A_Time& compT, unsigned* block)
{
    S<AEGP_LayerSuite9> ls(kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    S<AEGP_CompSuite12> cs(kAEGPCompSuite, kAEGPCompSuiteVersion12);
    S<AEGP_ItemSuite9> is(kAEGPItemSuite, kAEGPItemSuiteVersion9);
    S<AEGP_StreamSuite6> ss(kAEGPStreamSuite, kAEGPStreamSuiteVersion6);
    S<AEGP_EffectSuite4> es(kAEGPEffectSuite, kAEGPEffectSuiteVersion4);
    S<AEGP_LightSuite2> lights(kAEGPLightSuite, kAEGPLightSuiteVersion2);
    S<AEGP_FootageSuite5> fs(kAEGPFootageSuite, kAEGPFootageSuiteVersion5);
    if (!ls || !cs || !is || !ss || !es || !lights) return false;
    AEGP_CompH comp = nullptr;
    AEGP_CompFlags compFlags = 0;
    AEGP_LayerFlags flags = 0;
    if (ls->AEGP_GetLayerParentComp(L, &comp) || !comp || cs->AEGP_GetCompFlags(comp, &compFlags) ||
        ls->AEGP_GetLayerFlags(L, &flags))
        return false;
    AEGP_LayerH camera = nullptr;
    bool anyLight = false, anyShadowLight = false, dof = false;
    if (!ReadCompLights(ls.s, ss.s, lights.s, comp, compT, &camera, &anyLight, &anyShadowLight)) return false;
    double d = 0;
    if (camera && !OneD(ss.s, camera, AEGP_LayerStream_DEPTH_OF_FIELD, compT, &d)) return false;
    dof = d != 0;
    // this effect is one of the layer's active effects
    const int others = ActiveEffects(es.s, L) - 1;
    const CompFacts facts = {compFlags, anyLight, anyShadowLight, dof};
    *block = LayerBlock(ls.s, is.s, ss.s, fs.s, L, flags, compT, facts, others > 0);
    return true;
}

bool ReadMergeScene(const void* layer, const A_Time& layerTime, MergeSceneInput* out)
{
    S<AEGP_LayerSuite9> ls(kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    S<AEGP_CompSuite12> cs(kAEGPCompSuite, kAEGPCompSuiteVersion12);
    S<AEGP_ItemSuite9> is(kAEGPItemSuite, kAEGPItemSuiteVersion9);
    S<AEGP_StreamSuite6> ss(kAEGPStreamSuite, kAEGPStreamSuiteVersion6);
    S<AEGP_EffectSuite4> es(kAEGPEffectSuite, kAEGPEffectSuiteVersion4);
    S<AEGP_CameraSuite2> cams(kAEGPCameraSuite, kAEGPCameraSuiteVersion2);
    S<AEGP_LightSuite2> lights(kAEGPLightSuite, kAEGPLightSuiteVersion2);
    S<AEGP_FootageSuite5> fs(kAEGPFootageSuite, kAEGPFootageSuiteVersion5);
    if (!ls || !cs || !is || !ss || !es || !cams || !lights) return false;
    *out = MergeSceneInput();
    AEGP_LayerH self = (AEGP_LayerH)layer;
    AEGP_CompH comp = nullptr;
    AEGP_ItemH item = nullptr;
    A_long w = 0, h = 0, n = 0, compId = 0;
    A_Time compT = {0, 1};
    if (ls->AEGP_GetLayerParentComp(self, &comp) || !comp || cs->AEGP_GetItemFromComp(comp, &item) ||
        is->AEGP_GetItemDimensions(item, &w, &h) || is->AEGP_GetItemID(item, &compId) ||
        ls->AEGP_ConvertLayerToCompTime(self, &layerTime, &compT) || ls->AEGP_GetCompNumLayers(comp, &n))
        return false;
    AEGP_CompFlags compFlags = 0;
    if (cs->AEGP_GetCompFlags(comp, &compFlags)) return false;

    // pass 1: the camera (the topmost active one) and the lights
    AEGP_LayerH camera = nullptr;
    bool anyLight = false, anyShadowLight = false;
    if (!ReadCompLights(ls.s, ss.s, lights.s, comp, compT, &camera, &anyLight, &anyShadowLight)) return false;
    bool dof = false;
    if (camera) {
        A_Matrix4 m;
        double zoom = 0, d = 0;
        if (ls->AEGP_GetLayerToWorldXform(camera, &compT, &m) || !OneD(ss.s, camera, AEGP_LayerStream_ZOOM, compT, &zoom) ||
            !(zoom > 0) || !OneD(ss.s, camera, AEGP_LayerStream_DEPTH_OF_FIELD, compT, &d))
            return false;
        ToM(m, out->cam.camToWorld);
        out->cam.dist = zoom;
        out->cam.compW = (double)w;
        out->cam.compH = (double)h;
        dof = d != 0;
    } else {
        A_FpLong dist = 0;
        if (cams->AEGP_GetDefaultCameraDistanceToImagePlane(comp, &dist) || !(dist > 0)) return false;
        DefaultCamera((double)w, (double)h, dist, &out->cam);
    }

    // pass 2: the surfaces and the DupliX layers
    for (A_long i = 0; i < n; i++) {
        AEGP_LayerH Li = nullptr;
        AEGP_ObjectType type = AEGP_ObjectType_NONE;
        AEGP_LayerFlags flags = 0;
        A_Boolean active = FALSE;
        if (ls->AEGP_GetCompLayerByIndex(comp, i, &Li) || !Li || ls->AEGP_GetLayerObjectType(Li, &type) ||
            ls->AEGP_GetLayerFlags(Li, &flags) || ls->AEGP_IsVideoActive(Li, AEGP_LTimeMode_CompTime, &compT, &active))
            return false;
        // 2D layers are composited by layer order (not depth); cameras, lights, nulls and adjustment layers are no
        // surfaces. Conservative: every 3D layer counts, whichever 3D group it is in (spec: fewer merges only).
        if (!active || !(flags & AEGP_LayerFlag_LAYER_IS_3D) || type == AEGP_ObjectType_CAMERA ||
            type == AEGP_ObjectType_LIGHT || (flags & (AEGP_LayerFlag_NULL_LAYER | AEGP_LayerFlag_ADJUSTMENT_LAYER)))
            continue;
        A_Matrix4 m;
        A_FloatRect b;
        if (ls->AEGP_GetLayerToWorldXform(Li, &compT, &m) ||
            ls->AEGP_GetLayerMaskedBounds(Li, AEGP_LTimeMode_CompTime, &compT, &b))
            return false;
        double toWorld[16];
        ToM(m, toWorld);
        const double rect[4] = {b.left, b.top, b.right, b.bottom};
        A_Time lt = {0, 1};
        if (ls->AEGP_ConvertCompToLayerTime(Li, &compT, &lt)) return false;

        ParamSnapshot snap;
        int others = 0;
        if (type == AEGP_ObjectType_AV && ReadLayerParams(Li, lt, &snap, nullptr, &others) && snap.valid) {
            AEGP_LayerIDVal id = 0;
            if (ls->AEGP_GetLayerID(Li, &id)) return false;
            if (snap.p.merge) {
                MergeLayerInput in;
                in.compItemId = (int)compId;
                in.layerId = (int)id;
                in.p = snap.p;
                in.world = snap.world;
                in.anchorX = snap.anchorX;
                in.anchorY = snap.anchorY;
                std::memcpy(in.layerToWorld, toWorld, sizeof(toWorld));
                std::memcpy(in.srcRect, rect, sizeof(rect));
                const CompFacts facts = {compFlags, anyLight, anyShadowLight, dof};
                in.block = LayerBlock(ls.s, is.s, ss.s, fs.s, Li, flags, compT, facts, others > 0);
                out->layers.push_back(in);
            } else {
                // a DupliX layer that does not merge: each copy is a surface
                double G[16];
                WorldM4(snap.world, snap.anchorX, snap.anchorY, G);
                for (int c = 0; c < snap.p.count; c++) {
                    CopyXform x;
                    ComputeCopy(snap.p, c, snap.anchorX, snap.anchorY, &x);
                    if (x.opacity <= 0) continue;
                    double a[16];
                    Mul4(x.m4, G, a);
                    ScenePlane pl;
                    Mul4(a, toWorld, pl.toWorld);
                    std::memcpy(pl.rect, rect, sizeof(rect));
                    pl.unbounded = others > 0;
                    out->planes.push_back(pl);
                }
            }
            continue;
        }
        bool box = type == AEGP_ObjectType_TEXT || type == AEGP_ObjectType_3D_MODEL;
        if (!box && (flags & AEGP_LayerFlag_COLLAPSE)) {
            AEGP_ItemH src = nullptr;
            AEGP_ItemType it = AEGP_ItemType_NONE;
            box = !ls->AEGP_GetLayerSourceItem(Li, &src) && src && !is->AEGP_GetItemType(src, &it) && it == AEGP_ItemType_COMP;
        }
        if (box) {  // per-character 3D text, 3D models, collapsed 3D precomps: depth unknown (conservative)
            SceneBox bx;
            for (int k = 0; k < 3; k++) bx.minW[k] = -1e300, bx.maxW[k] = 1e300;
            bx.unbounded = true;
            out->boxes.push_back(bx);
            continue;
        }
        ScenePlane pl;
        std::memcpy(pl.toWorld, toWorld, sizeof(toWorld));
        std::memcpy(pl.rect, rect, sizeof(rect));
        pl.unbounded = ActiveEffects(es.s, Li) > 0;  // an effect may draw outside the layer's rect
        out->planes.push_back(pl);
    }
    return true;
}

void LogMergeScene(const MergeSceneInput& in)
{
    RLog("merge: scene cam dist=%.2f comp %.0fx%.0f camPos=(%.2f,%.2f,%.2f) layers=%zu planes=%zu boxes=%zu", in.cam.dist,
         in.cam.compW, in.cam.compH, in.cam.camToWorld[12], in.cam.camToWorld[13], in.cam.camToWorld[14], in.layers.size(),
         in.planes.size(), in.boxes.size());
    for (const auto& l : in.layers)
        RLog("merge:   layer %d/%d count=%d block=%x rect=(%.1f,%.1f,%.1f,%.1f)", l.compItemId, l.layerId, l.p.count,
             l.block, l.srcRect[0], l.srcRect[1], l.srcRect[2], l.srcRect[3]);
    for (const auto& p : in.planes)
        RLog("merge:   plane origin=(%.1f,%.1f,%.1f) rect=(%.1f,%.1f,%.1f,%.1f) unbounded=%d", p.toWorld[12], p.toWorld[13],
             p.toWorld[14], p.rect[0], p.rect[1], p.rect[2], p.rect[3], (int)p.unbounded);
}

}  // namespace rnd
