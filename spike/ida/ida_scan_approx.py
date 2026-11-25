"""Scan every function of a database for approximate / fused float instructions; list per function.
usage: python scan_approx.py <db.i64> <out.txt> [0xEA ...]  (functions to also disassemble into <out>.d/<ea>.asm)
"""
import os
import re
import sys

import idapro

dbp, out = sys.argv[1], sys.argv[2]
idapro.open_database(dbp, run_auto_analysis=False)
import ida_funcs  # noqa: E402
import idautils  # noqa: E402
import idc  # noqa: E402

pat = re.compile(r"^(v?rcp|v?rsqrt|vfn?m(add|sub)|vfmaddsub|vfmsubadd)", re.I)
rows = []
for f in idautils.Functions():
    hits = {}
    for h in idautils.FuncItems(f):
        m = idc.print_insn_mnem(h)
        if pat.match(m):
            hits[m] = hits.get(m, 0) + 1
    if hits:
        rows.append(f"{f:X} {idc.get_func_name(f)} " + " ".join(f"{k}={v}" for k, v in sorted(hits.items())))
with open(out, "w", encoding="utf-8") as fh:
    fh.write("\n".join(rows) + "\n")
d = out + ".d"
os.makedirs(d, exist_ok=True)
for a in sys.argv[3:]:
    ea = int(a, 16)
    fn = ida_funcs.get_func(ea)
    with open(os.path.join(d, f"{ea:X}.asm"), "w", encoding="utf-8") as fh:
        for h in idautils.Heads(fn.start_ea, fn.end_ea):
            fh.write(f"{h:X}  {idc.generate_disasm_line(h, 0)}\n")
idapro.close_database(save=False)
