"""List imported functions matching a regex and decompile the functions that call them.

usage: python ida_importers.py <db.i64> <out_dir> <import_regex> [max_callers_per_import]
"""
import os
import re
import sys

import idapro  # must come first

dbp, out_dir, pat = sys.argv[1], sys.argv[2], sys.argv[3]
maxc = int(sys.argv[4]) if len(sys.argv) > 4 else 5
os.makedirs(out_dir, exist_ok=True)
idapro.open_database(dbp, run_auto_analysis=False)
import ida_funcs  # noqa: E402
import ida_hexrays  # noqa: E402
import ida_nalt  # noqa: E402
import ida_name  # noqa: E402
import idautils  # noqa: E402
import idc  # noqa: E402

ida_hexrays.init_hexrays_plugin()
rx = re.compile(pat)
hits = []


def cb(ea, name, ordv):
    if name:
        d = idc.demangle_name(name, idc.get_inf_attr(idc.INF_SHORT_DN)) or name
        if rx.search(name) or rx.search(d):
            hits.append((ea, d))
    return True


for i in range(ida_nalt.get_import_module_qty()):
    ida_nalt.enum_import_names(i, cb)

index = []
done = set()
for ea, d in hits:
    callers = []
    for x in idautils.XrefsTo(ea, 0):
        f = ida_funcs.get_func(x.frm)
        if f:
            callers.append(f.start_ea)
        else:  # thunk / jump stub: follow one level
            for y in idautils.XrefsTo(x.frm, 0):
                g = ida_funcs.get_func(y.frm)
                if g:
                    callers.append(g.start_ea)
    callers = sorted(set(callers))
    index.append(f"IMPORT {d}  callers={len(callers)}: " + " ".join(f"0x{c:X}" for c in callers))
    for c in callers[:maxc]:
        if c in done:
            continue
        done.add(c)
        try:
            body = str(ida_hexrays.decompile(c))
        except Exception as e:  # noqa
            body = f"(decompile failed: {e})"
        n = ida_name.get_name(c) or f"sub_{c:X}"
        with open(os.path.join(out_dir, f"c_{c:X}.c"), "w", encoding="utf-8") as fh:
            fh.write(f"// {n} (calls {d})\n\n{body}\n")
with open(os.path.join(out_dir, "index.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(index) + "\n")
print("imports:", len(hits))
idapro.close_database(save=False)
