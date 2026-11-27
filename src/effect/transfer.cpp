#include "effect/transfer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "hook/bee_hooks.h"

namespace {

template <class T>
struct Acquired {
    SPBasicSuite* sp;
    const char* name;
    int32 ver;
    T* s = nullptr;
    Acquired(SPBasicSuite* p, const char* n, int32 v) : sp(p), name(n), ver(v)
    {
        if (sp->AcquireSuite(n, v, (const void**)&s) != kSPNoError) s = nullptr;
    }
    ~Acquired()
    {
        if (s) sp->ReleaseSuite(name, ver);
    }
    T* operator->() const { return s; }
    explicit operator bool() const { return s != nullptr; }
};

}  // namespace

TransferResult TransferLayerToWorld(PF_InData* in_data, AEGP_PluginID id, const rnd::WorldXform& cur,
                                    rnd::WorldXform* out, bool* worldChanged)
{
    *worldChanged = false;
    SPBasicSuite* sp = in_data->pica_basicP;
    Acquired<AEGP_PFInterfaceSuite1> pfi(sp, kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1);
    Acquired<AEGP_LayerSuite9> ls(sp, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    Acquired<AEGP_CompSuite12> cs(sp, kAEGPCompSuite, kAEGPCompSuiteVersion12);
    Acquired<AEGP_ItemSuite9> is(sp, kAEGPItemSuite, kAEGPItemSuiteVersion9);
    Acquired<AEGP_StreamSuite6> ss(sp, kAEGPStreamSuite, kAEGPStreamSuiteVersion6);
    Acquired<AEGP_KeyframeSuite5> ks(sp, kAEGPKeyframeSuite, kAEGPKeyframeSuiteVersion5);
    Acquired<AEGP_UtilitySuite6> us(sp, kAEGPUtilitySuite, kAEGPUtilitySuiteVersion6);
    if (!pfi || !ls || !cs || !is || !ss || !ks || !us) return TransferResult::Failed;

    // 1. read everything before changing anything
    AEGP_LayerH L = nullptr;
    AEGP_CompH comp = nullptr;
    AEGP_ItemH item = nullptr;
    A_Time ct = {0, 1};
    A_long w = 0, h = 0;
    A_Matrix4 l2w;
    if (pfi->AEGP_GetEffectLayer(in_data->effect_ref, &L) || !L || ls->AEGP_GetLayerParentComp(L, &comp) ||
        cs->AEGP_GetItemFromComp(comp, &item) || is->AEGP_GetItemDimensions(item, &w, &h) || w <= 0 || h <= 0 ||
        pfi->AEGP_ConvertEffectToCompTime(in_data->effect_ref, in_data->current_time, in_data->time_scale, &ct) ||
        ls->AEGP_GetLayerToWorldXform(L, &ct, &l2w))
        return TransferResult::Failed;
    A_Time lt = {in_data->current_time, (A_u_long)(in_data->time_scale ? in_data->time_scale : 1)};
    double ax = 0, ay = 0;
    if (!rnd::ReadLayerAnchor(L, lt, &ax, &ay)) return TransferResult::Failed;
    double m[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) m[r * 4 + c] = l2w.mat[r][c];
    RLog("transfer: l2w [%.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f | %.2f %.2f %.2f] anchor (%.2f,%.2f) t=%d/%d",
         m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14], ax, ay, (int)ct.value,
         (int)ct.scale);
    if (!rnd::TransferToWorld(cur, m, ax, ay, (double)w, (double)h, out)) return TransferResult::Failed;

    // 2. checks, still before any change
    Acquired<AEGP_DynamicStreamSuite4> ds(sp, kAEGPDynamicStreamSuite, kAEGPDynamicStreamSuiteVersion4);
    {
        AEGP_StreamRefH s = nullptr;
        A_Boolean sep = FALSE;
        if (ds && !ss->AEGP_GetNewLayerStream(id, L, AEGP_LayerStream_POSITION, &s) && s) {
            ds->AEGP_AreDimensionsSeparated(s, &sep);
            ss->AEGP_DisposeStream(s);
        }
        if (sep) return TransferResult::RefusedSeparated;
    }
    double gCur[16], gNew[16], d = 0;
    rnd::WorldM4(cur, ax, ay, gCur);
    rnd::WorldM4(*out, ax, ay, gNew);
    for (int i = 0; i < 16; i++) d = std::max(d, std::fabs(gCur[i] - gNew[i]));
    *worldChanged = d > 1e-6;
    if (*worldChanged) {
        // Writing params[] of a keyframed parameter adds a keyframe at this time only (dev log 11/25): the
        // other keyframes would miss the layer's transform. Refuse instead.
        Acquired<AEGP_EffectSuite4> es(sp, kAEGPEffectSuite, kAEGPEffectSuiteVersion4);
        AEGP_EffectRefH eff = nullptr;
        bool animated = false;
        if (!es || pfi->AEGP_GetNewEffectForEffect(id, in_data->effect_ref, &eff) || !eff) return TransferResult::Failed;
        for (int i = RP_WORLD_POS_X; i <= RP_WORLD_SCALE_Z && !animated; i++) {
            AEGP_StreamRefH s = nullptr;
            if (ss->AEGP_GetNewEffectStreamByIndex(id, eff, i, &s) || !s) continue;
            A_long nk = 0;
            A_Boolean expr = FALSE;
            animated = (!ks->AEGP_GetStreamNumKFs(s, &nk) && nk > 0) ||
                       (!ss->AEGP_GetExpressionState(id, s, &expr) && expr);
            ss->AEGP_DisposeStream(s);
        }
        es->AEGP_DisposeEffect(eff);
        if (animated) {
            *worldChanged = false;
            return TransferResult::RefusedWorldAnimated;
        }
    }

    // 3. the layer to its initial 2D state, as one undo step
    bool dropped = false, failed = false;
    us->AEGP_StartUndoGroup("DupliX Render Mode");
    auto reset = [&](AEGP_LayerStream which, double x, double y, double z) {
        AEGP_StreamRefH s = nullptr;
        if (ss->AEGP_GetNewLayerStream(id, L, which, &s) || !s) {
            failed = true;
            return;
        }
        A_long nk = 0;
        if (!ks->AEGP_GetStreamNumKFs(s, &nk) && nk > 0) {
            dropped = true;
            for (A_long k = nk - 1; k >= 0; k--) ks->AEGP_DeleteKeyframe(s, k);
        }
        A_Boolean expr = FALSE;
        if (!ss->AEGP_GetExpressionState(id, s, &expr) && expr) {
            dropped = true;
            ss->AEGP_SetExpressionState(id, s, FALSE);
        }
        AEGP_StreamType type = AEGP_StreamType_NO_DATA;
        ss->AEGP_GetStreamType(s, &type);
        AEGP_StreamValue2 v;
        std::memset(&v, 0, sizeof(v));
        v.streamH = s;
        if (type == AEGP_StreamType_ThreeD || type == AEGP_StreamType_ThreeD_SPATIAL) {
            v.val.three_d.x = x, v.val.three_d.y = y, v.val.three_d.z = z;
        } else if (type == AEGP_StreamType_TwoD || type == AEGP_StreamType_TwoD_SPATIAL) {
            v.val.two_d.x = x, v.val.two_d.y = y;
        } else {
            v.val.one_d = x;
        }
        if (ss->AEGP_SetStreamValue(id, s, &v)) failed = true;
        ss->AEGP_DisposeStream(s);
    };
    reset(AEGP_LayerStream_POSITION, w / 2.0, h / 2.0, 0);
    reset(AEGP_LayerStream_SCALE, 100, 100, 100);
    reset(AEGP_LayerStream_ORIENTATION, 0, 0, 0);
    reset(AEGP_LayerStream_ROTATE_X, 0, 0, 0);
    reset(AEGP_LayerStream_ROTATE_Y, 0, 0, 0);
    reset(AEGP_LayerStream_ROTATION, 0, 0, 0);  // Z
    if (ls->AEGP_SetLayerParent(L, nullptr)) failed = true;
    if (ls->AEGP_SetLayerFlag(L, AEGP_LayerFlag_LAYER_IS_3D, FALSE)) failed = true;
    us->AEGP_EndUndoGroup();
    RLog("transfer: layer->world pos (%.2f,%.2f,%.2f) rot (%.2f,%.2f,%.2f) scale (%.2f,%.2f,%.2f) dropped=%d failed=%d",
         out->pos[0], out->pos[1], out->pos[2], out->rotDeg[0], out->rotDeg[1], out->rotDeg[2], out->scale[0],
         out->scale[1], out->scale[2], (int)dropped, (int)failed);
    if (failed) return TransferResult::Partial;  // the layer changed: the World Transform must still be written
    return dropped ? TransferResult::DoneAnimationDropped : TransferResult::Done;
}

bool SetLayer3D(PF_InData* in_data, bool on)
{
    SPBasicSuite* sp = in_data->pica_basicP;
    Acquired<AEGP_PFInterfaceSuite1> pfi(sp, kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1);
    Acquired<AEGP_LayerSuite9> ls(sp, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    AEGP_LayerH L = nullptr;
    if (!pfi || !ls || pfi->AEGP_GetEffectLayer(in_data->effect_ref, &L) || !L) return false;
    return ls->AEGP_SetLayerFlag(L, AEGP_LayerFlag_LAYER_IS_3D, on ? TRUE : FALSE) == A_Err_NONE;
}
