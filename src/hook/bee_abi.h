// Facts about AE 2025's BEE.dll that the hooks rely on (found by static analysis, see HANDOFF.md
// tests 3-23). Everything is looked up by exported name at run time; nothing is a fixed address.
#pragma once
#include <cstddef>
#include <cstdint>

namespace rnd::bee {

// Shared vtable of every AV layer.
constexpr const char* kAVLayerVtable = "??_7BEE_AVLayer@@6B@";
constexpr int kVtableScanSlots = 283;  // slots to scan when matching exported functions

// Virtuals we replace (their exported names give the slot numbers).
constexpr const char* kIsLayerType = "?IsLayerType@BEE_AVLayer@@UEBAEH@Z";
constexpr const char* kSubLayersRenderSeparately = "?SubLayersRenderSeparately@BEE_AVLayer@@UEBA_NXZ";
constexpr const char* kSubLayerRenderCount =
    "?SubLayerRenderCount@BEE_AVLayer@@UEBAHAEBUT_Time@@AEBV?$shared_ptr@VBEE_XformTimeCache@@@std@@PEBVTDB_ParamBag@@@Z";
constexpr const char* kGetSubLayerRects =
    "?GetSubLayerRects@BEE_AVLayer@@UEBA?AUM_FloatRect@@AEBUT_Time@@AEBU2@AEAV?$vector@UM_FloatRect@@V?$allocator@"
    "UM_FloatRect@@@std@@@std@@AEAV?$vector@VM_Matrix3@@V?$allocator@VM_Matrix3@@@std@@@5@AEAV?$vector@VM_Matrix4@@V?$"
    "allocator@VM_Matrix4@@@std@@@5@PEBVTDB_ParamBag@@@Z";

// Imported by AEGPDriver.aex: the per-layer opacity Artisan (classic 3D) uses.
constexpr const char* kAEGPDriverModule = "AEGPDriver.aex";
constexpr const char* kGetArtisanLayerOpacity =
    "?BEE_GetArtisanLayerOpacity@@YANPEAXPEAVBEE_RenderTrace@@PEBUT_Time@@PEBVTDB_ParamBag@@@Z";
// BEE_RenderTrace -> artisan layer record: v = *(char**)(trace + 16); layer = *(void**)(v - 48); index = *(int*)(v - 40)
constexpr ptrdiff_t kTraceArtLayer = 16;
constexpr ptrdiff_t kArtLayerToLayer = -48;
constexpr ptrdiff_t kArtLayerToSubIndex = -40;

// Plain exported helpers.
constexpr const char* kLayerGetID = "?GetID@BEE_Layer@@QEBA?BHXZ";                                  // int (const BEE_Layer*)
constexpr const char* kLayerGetParentComp = "?GetParentComp@BEE_Layer@@QEBAQEBVBEE_CompItem@@XZ";   // const BEE_CompItem*
constexpr const char* kItemGetID = "?GetID@BEE_Item@@QEBAAEBHXZ";                                   // const int& (const BEE_Item*)
// bool (BEE_Layer*, short effectKey, std::vector<int>* indicesOrNull): walks the layer's effect parade and
// compares each effect's type key (the 16-bit value at effect + 618) with effectKey. No AEGP inside.
constexpr const char* kLayerHasEffectApplied =
    "?HasEffectApplied@BEE_Layer@@UEAA_NFPEAV?$vector@HV?$allocator@H@std@@@std@@@Z";
// The layer's effect list and the effect's on/off switch, read without AEGP (AEGP must not be called
// from BEE virtuals on the UI thread).
constexpr const char* kLayerGetFilterParade = "?GetFilterParade@BEE_AVLayer@@UEBAPEBVFLT_Parade@@XZ";  // vtable slot
constexpr const char* kFltModule = "FLT.dll";
constexpr const char* kParadeNumSeqs = "?FLT_InqParadeNumSeqs@@YAHPEAVFLT_Parade@@PEAH@Z";           // int (parade, int*)
constexpr const char* kParadeGetEffect = "?GetEffect@FLT_Parade@@QEAAPEAVFLT_FCSeqSpec@@I@Z";          // (parade, unsigned)
constexpr const char* kEffectIsActiveVideo = "?IsActiveVideoEffect@FLT_FCSeqSpec@@QEBA_NXZ";
constexpr ptrdiff_t kEffectTypeKey = 618;  // unsigned short: the effect's type key (see HasEffectApplied)
// BEE_AVLayer::SubLayersRenderSeparately starts with `call [rax+disp]` to the layer's switch getter;
// for AV layers the 3D switch is bit 0x400 of its result.
constexpr unsigned kSwitch3D = 0x400;
// The comp's 3D renderer (bool (const BEE_CompItem*)). Copies are only made with Classic 3D: the
// Advanced 3D renderer (Calder) crashed when asked for sub-layers; Cinema 4D (Ernst) is untested.
constexpr const char* kCompIsCalder = "?ArtisanIsCalder@BEE_CompItem@@QEBA_NXZ";
constexpr const char* kCompIsErnst = "?ArtisanIsErnst@BEE_CompItem@@QEBA_NXZ";
constexpr const char* kCompIsEscher = "?ArtisanIsEscher@BEE_CompItem@@QEBA_NXZ";
constexpr const char* kCompIsPicasso = "?ArtisanIsPicasso@BEE_CompItem@@QEBA_NXZ";
constexpr const char* kBetaExpressionEffects = "?BEE_BetaFeature_AreExpressionEffectsAlwaysAllowedEnabled@@YA_NXZ";
constexpr const char* kM3SetIdentity = "?SetIdentity@M_Matrix3@@QEAAXXZ";  // U.dll

// GetSubLayerRects output vectors (MSVC std::vector layout) and element sizes.
struct RawVec {
    char* first;
    char* last;
    char* end;
};
constexpr size_t kRectBytes = 32;   // M_FloatRect: 4 doubles
constexpr size_t kM3Bytes = 72;     // M_Matrix3: 9 doubles
constexpr size_t kM4Bytes = 128;    // M_Matrix4: 16 doubles, row vectors, row 3 = translation

// Empties v and makes room for n elements, allocating like MSVC's STL does inside AE (ucrtbase malloc;
// blocks of 4096 bytes or more are 32-byte aligned with the raw pointer stored just before them),
// because AE frees these vectors itself.
bool VecReserve(RawVec* v, size_t elemBytes, size_t n);

}  // namespace rnd::bee
