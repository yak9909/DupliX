"""Checks the facts src/hook/bee_abi.h relies on against a BEE.dll (any AE version).

usage: python abi_check.py <BEE.dll>
Reports: exported names present, BEE_AVLayer vtable slots of the hooked virtuals, the switch-getter
slot used by SubLayersRenderSeparately, and whether HasEffectApplied still reads the type key at +618.
"""
import re
import struct
import sys

import pefile

NAMES = {
    "vtable": "??_7BEE_AVLayer@@6B@",
    "IsLayerType": "?IsLayerType@BEE_AVLayer@@UEBAEH@Z",
    "Separately": "?SubLayersRenderSeparately@BEE_AVLayer@@UEBA_NXZ",
    "Count": "?SubLayerRenderCount@BEE_AVLayer@@UEBAHAEBUT_Time@@AEBV?$shared_ptr@VBEE_XformTimeCache@@@std@@PEBVTDB_ParamBag@@@Z",
    "Rects": "?GetSubLayerRects@BEE_AVLayer@@UEBA?AUM_FloatRect@@AEBUT_Time@@AEBU2@AEAV?$vector@UM_FloatRect@@V?$allocator@"
             "UM_FloatRect@@@std@@@std@@AEAV?$vector@VM_Matrix3@@V?$allocator@VM_Matrix3@@@std@@@5@AEAV?$vector@VM_Matrix4@@V?$"
             "allocator@VM_Matrix4@@@std@@@5@PEBVTDB_ParamBag@@@Z",
    "GetFilterParade": "?GetFilterParade@BEE_AVLayer@@UEBAPEBVFLT_Parade@@XZ",
    "ArtOpacity": "?BEE_GetArtisanLayerOpacity@@YANPEAXPEAVBEE_RenderTrace@@PEBUT_Time@@PEBVTDB_ParamBag@@@Z",
    "Layer::GetID": "?GetID@BEE_Layer@@QEBA?BHXZ",
    "Layer::GetParentComp": "?GetParentComp@BEE_Layer@@QEBAQEBVBEE_CompItem@@XZ",
    "Item::GetID": "?GetID@BEE_Item@@QEBAAEBHXZ",
    "HasEffectApplied": "?HasEffectApplied@BEE_Layer@@UEAA_NFPEAV?$vector@HV?$allocator@H@std@@@std@@@Z",
    "BetaExprEffects": "?BEE_BetaFeature_AreExpressionEffectsAlwaysAllowedEnabled@@YA_NXZ",
}
VIRTUALS = ["IsLayerType", "Separately", "Count", "Rects", "GetFilterParade"]


def main() -> int:
    # BEE.dll has more exports than pefile's default limit (8192)
    pe = pefile.PE(sys.argv[1], fast_load=True, max_symbol_exports=200000)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
    base = pe.OPTIONAL_HEADER.ImageBase
    exports = {e.name.decode(): e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name}
    print(f"file: {sys.argv[1]}")
    missing = 0
    rva = {}
    for k, n in NAMES.items():
        a = exports.get(n)
        rva[k] = a
        if a is None:
            missing += 1
            # show near matches so a renamed signature is easy to spot
            short = re.match(r"\?\??\w+@\w+", n)
            pat = short.group(0) if short else n[:20]
            near = [x for x in exports if x.startswith(pat)][:3]
            print(f"  MISSING {k}: {n[:70]}...  near: {near}")
        else:
            print(f"  ok      {k} rva={a:#x}")
    if rva["vtable"] is not None:
        data = pe.get_data(rva["vtable"], 8 * 400)
        slots = [struct.unpack_from("<Q", data, 8 * i)[0] - base for i in range(400)]
        for k in VIRTUALS:
            s = slots.index(rva[k]) if rva[k] in slots else None
            print(f"  slot {k:<16} {s}")
        if rva["Separately"] is not None:
            code = pe.get_data(rva["Separately"], 48)
            m = re.search(rb"\xff\x90(....)|\xff\x50(.)", code, re.S)
            if m:
                disp = struct.unpack("<i", m.group(1))[0] if m.group(1) else struct.unpack("<b", m.group(2))[0]
                print(f"  switch getter slot (Separately's first vcall) {disp // 8}")
            else:
                print("  switch getter: pattern NOT found in Separately")
    if rva["HasEffectApplied"] is not None:
        code = pe.get_data(rva["HasEffectApplied"], 160)
        hit = re.search(rb"\x66\x39..\x6a\x02\x00\x00|\x66\x39.\x6a\x02\x00\x00|\x0f\xb7..\x6a\x02\x00\x00", code, re.S)
        print(f"  HasEffectApplied reads +0x26A (618): {'yes' if hit else 'not found by pattern'}")
    print(f"missing exports: {missing}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
