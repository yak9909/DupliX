"""Disassembly of functions into <out_dir>/<ea>.asm (for functions Hex-Rays leaves as __asm blocks).

usage: python ida_disasm.py <db.i64> <out_dir> 0xEA [0xEA ...]
"""
import os
import sys

import idapro  # must come first (idalib)

dbp, out_dir = sys.argv[1], sys.argv[2]
os.makedirs(out_dir, exist_ok=True)
idapro.open_database(dbp, run_auto_analysis=False)

import ida_funcs  # noqa: E402
import idautils  # noqa: E402
import idc  # noqa: E402

for a in sys.argv[3:]:
    ea = int(a, 16)
    f = ida_funcs.get_func(ea)
    lines = []
    for h in idautils.Heads(f.start_ea, f.end_ea):
        lines.append(f"{h:X}  {idc.generate_disasm_line(h, 0)}")
    with open(os.path.join(out_dir, f"{ea:X}.asm"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")
idapro.close_database(save=False)
