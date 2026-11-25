"""Decompile the functions that reference strings matching a regex (e.g. an error message naming the function).

usage: python ida_strref.py <db.i64> <out_dir> <string_regex> [max_funcs]
"""
import os
import re
import sys

import idapro  # must come first

dbp, out_dir, pat = sys.argv[1], sys.argv[2], sys.argv[3]
maxf = int(sys.argv[4]) if len(sys.argv) > 4 else 10
os.makedirs(out_dir, exist_ok=True)
idapro.open_database(dbp, run_auto_analysis=False)
import ida_funcs  # noqa: E402
import ida_hexrays  # noqa: E402
import ida_name  # noqa: E402
import idautils  # noqa: E402

ida_hexrays.init_hexrays_plugin()
rx = re.compile(pat)
index = []
done = set()
for s in idautils.Strings():
    t = str(s)
    if not rx.search(t):
        continue
    funcs = sorted({ida_funcs.get_func(x.frm).start_ea for x in idautils.XrefsTo(s.ea, 0) if ida_funcs.get_func(x.frm)})
    index.append(f"STRING {t!r} at 0x{s.ea:X}: " + " ".join(f"0x{f:X}" for f in funcs))
    for f in funcs:
        if f in done or len(done) >= maxf:
            continue
        done.add(f)
        try:
            body = str(ida_hexrays.decompile(f))
        except Exception as e:  # noqa
            body = f"(decompile failed: {e})"
        n = ida_name.get_name(f) or f"sub_{f:X}"
        with open(os.path.join(out_dir, f"s_{f:X}.c"), "w", encoding="utf-8") as fh:
            fh.write(f"// {n} (references {t!r})\n\n{body}\n")
with open(os.path.join(out_dir, "index.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(index) + "\n")
print("strings:", len(index))
idapro.close_database(save=False)
