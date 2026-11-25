"""Decompile every function of an analysed database into <out_dir>/<ea>.c, plus index.txt
("ea size callees-count name"), for reading with Grep/Read instead of the MCP server.

usage: python ida_dump_all.py <db.i64> <out_dir> [min_size]
"""
import os
import sys

import idapro  # must come first (idalib)

dbp, out_dir = sys.argv[1], sys.argv[2]
min_size = int(sys.argv[3]) if len(sys.argv) > 3 else 0x20
os.makedirs(out_dir, exist_ok=True)
idapro.open_database(dbp, run_auto_analysis=False)

import ida_funcs  # noqa: E402
import ida_hexrays  # noqa: E402
import idautils  # noqa: E402
import idc  # noqa: E402

ida_hexrays.init_hexrays_plugin()
index = []
n_ok = n_fail = 0
for ea in idautils.Functions():
    f = ida_funcs.get_func(ea)
    if not f:
        continue
    size = f.end_ea - f.start_ea
    if size < min_size or (f.flags & (ida_funcs.FUNC_THUNK | ida_funcs.FUNC_LIB)):
        continue
    name = idc.get_func_name(ea)
    calls = sum(1 for _ in idautils.CodeRefsFrom(ea, 0))
    try:
        cf = ida_hexrays.decompile(ea)
        text = str(cf)
        n_ok += 1
    except Exception as e:  # noqa: BLE001
        text = f"// decompile failed: {e}\n"
        n_fail += 1
    with open(os.path.join(out_dir, f"{ea:X}.c"), "w", encoding="utf-8") as fh:
        fh.write(f"// {name} @ {ea:#x} size {size:#x}\n{text}")
    index.append(f"{ea:X} {size:6d} {name}")
with open(os.path.join(out_dir, "index.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(index) + "\n")
print(f"decompiled {n_ok}, failed {n_fail}", flush=True)
idapro.close_database(save=False)
