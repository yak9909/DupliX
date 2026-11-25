"""List the slots of vtables (by demangled-name regex) and decompile their targets into <out_dir>.

usage: python ida_vtables.py <db.i64> <out_dir> <regex> [slots]
"""
import os
import re
import sys

import idapro  # must come first (idalib)

dbp, out_dir, pat = sys.argv[1], sys.argv[2], re.compile(sys.argv[3])
slots = int(sys.argv[4]) if len(sys.argv) > 4 else 16
os.makedirs(out_dir, exist_ok=True)
idapro.open_database(dbp, run_auto_analysis=False)

import ida_bytes  # noqa: E402
import ida_hexrays  # noqa: E402
import ida_name  # noqa: E402
import idautils  # noqa: E402
import idc  # noqa: E402

ida_hexrays.init_hexrays_plugin()
lines = []
done = set()
for ea, name in idautils.Names():
    dn = idc.demangle_name(name, idc.get_inf_attr(idc.INF_SHORT_DN)) or name
    if "vftable" not in dn or not pat.search(dn):
        continue
    lines.append(f"{dn} @ {ea:#x}")
    for i in range(slots):
        t = ida_bytes.get_qword(ea + 8 * i)
        if not idc.get_func_name(t):
            break
        tn = idc.get_func_name(t)
        lines.append(f"  [{i}] {t:#x} {idc.demangle_name(tn, idc.get_inf_attr(idc.INF_SHORT_DN)) or tn}")
        if t in done:
            continue
        done.add(t)
        try:
            text = str(ida_hexrays.decompile(t))
        except Exception as e:  # noqa: BLE001
            text = f"// failed {e}"
        with open(os.path.join(out_dir, f"{t:X}.c"), "w", encoding="utf-8") as fh:
            fh.write(f"// {tn} (vtable {dn} slot {i})\n{text}")
with open(os.path.join(out_dir, "vtables.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines) + "\n")
idapro.close_database(save=False)
