"""Decompile all callers of given addresses (code xrefs), recursively up to depth.

usage: python ida_callers.py <db.i64> <out_dir> <depth> 0xADDR [0xADDR ...]
"""
import os
import re
import sys

import idapro  # noqa

dbp, out_dir, depth = sys.argv[1], sys.argv[2], int(sys.argv[3])
targets = [int(x, 16) for x in sys.argv[4:]]
os.makedirs(out_dir, exist_ok=True)
idapro.open_database(dbp, run_auto_analysis=False)
import ida_funcs  # noqa: E402
import ida_hexrays  # noqa: E402
import ida_name  # noqa: E402
import idautils  # noqa: E402
import idc  # noqa: E402

ida_hexrays.init_hexrays_plugin()


def fname(ea):
    n = ida_name.get_name(ea) or f"sub_{ea:X}"
    return idc.demangle_name(n, idc.get_inf_attr(idc.INF_SHORT_DN)) or n


lines = []
seen = set()


def visit(ea, d, ind):
    lines.append(f"{'  ' * ind}0x{ea:X} {fname(ea)}")
    if d >= depth:
        return
    for x in idautils.XrefsTo(ea, 0):
        f = ida_funcs.get_func(x.frm)
        if not f:
            lines.append(f"{'  ' * (ind + 1)}DATAREF 0x{x.frm:X} {fname(x.frm)}")
            continue
        s = f.start_ea
        if s in seen:
            lines.append(f"{'  ' * (ind + 1)}0x{s:X} {fname(s)} (seen, site 0x{x.frm:X})")
            continue
        seen.add(s)
        p = os.path.join(out_dir, f"fn_{s:X}_{re.sub(r'[^A-Za-z0-9_]+', '_', fname(s))[:90]}.c")
        try:
            body = str(ida_hexrays.decompile(s))
        except Exception as e:  # noqa
            body = f"(decompile failed: {e})"
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(f"// {fname(s)}\n// ea=0x{s:X}\n\n{body}\n")
        lines[-0:]  # noqa
        lines.append(f"{'  ' * (ind + 1)}site 0x{x.frm:X}:")
        visit(s, d + 1, ind + 1)


for t in targets:
    visit(t, 0, 0)
with open(os.path.join(out_dir, "index.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines) + "\n")
print("done")
idapro.close_database(save=False)
