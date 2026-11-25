"""For a function referenced from a data table (a suite's function table), decompile the table's neighbours.

usage: python ida_table_next.py <db.i64> <out_dir> <func_ea_hex> [before] [after]
"""
import os
import sys

import idapro  # must come first

dbp, out_dir, fea = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
before = int(sys.argv[4]) if len(sys.argv) > 4 else 2
after = int(sys.argv[5]) if len(sys.argv) > 5 else 4
os.makedirs(out_dir, exist_ok=True)
idapro.open_database(dbp, run_auto_analysis=False)
import ida_bytes  # noqa: E402
import ida_funcs  # noqa: E402
import ida_hexrays  # noqa: E402
import ida_name  # noqa: E402
import idautils  # noqa: E402

ida_hexrays.init_hexrays_plugin()
lines = []
for x in idautils.XrefsTo(fea, 0):
    if ida_funcs.get_func(x.frm):
        continue  # code reference
    lines.append(f"TABLE ref at 0x{x.frm:X}")
    for k in range(-before, after + 1):
        p = ida_bytes.get_qword(x.frm + 8 * k)
        n = ida_name.get_name(p) or f"0x{p:X}"
        lines.append(f"  [{k:+d}] 0x{p:X} {n}")
        f = ida_funcs.get_func(p)
        if f and k != 0:
            try:
                body = str(ida_hexrays.decompile(f.start_ea))
            except Exception as e:  # noqa
                body = f"(decompile failed: {e})"
            with open(os.path.join(out_dir, f"t_{k:+d}_{p:X}.c"), "w", encoding="utf-8") as fh:
                fh.write(f"// {n}\n\n{body}\n")
with open(os.path.join(out_dir, "index.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines) + "\n")
print("\n".join(lines))
idapro.close_database(save=False)
