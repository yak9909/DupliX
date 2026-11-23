"""Dump decompilation of selected exported functions + vtables WITHOUT full auto-analysis.
Results go to text files (read later with Grep/Read, not through the MCP server).

usage: python ida_dump.py <binary> <out_dir> <export_regex> [<vtable_regex>]
  export_regex : exported (mangled or demangled) names to decompile
  vtable_regex : exported vtable symbols (??_7...) whose slots are listed (names + decompiled targets)
"""
import os
import re
import sys

import idapro  # must come first (idalib)

binp, out_dir, pat = sys.argv[1], sys.argv[2], sys.argv[3]
vpat = sys.argv[4] if len(sys.argv) > 4 else None
os.makedirs(out_dir, exist_ok=True)
idapro.open_database(binp, run_auto_analysis=False)

import ida_bytes
import ida_funcs
import ida_hexrays
import ida_name
import idautils
import idc

ida_hexrays.init_hexrays_plugin()


def demangle(n):
    return idc.demangle_name(n, idc.get_inf_attr(idc.INF_SHORT_DN)) or n


def ensure_func(ea):
    if not ida_funcs.get_func(ea):
        idc.create_insn(ea)
        ida_funcs.add_func(ea)
    return ida_funcs.get_func(ea) is not None


def decomp(ea):
    if not ensure_func(ea):
        return "(could not create function)"
    try:
        c = ida_hexrays.decompile(ea)
        return str(c) if c else "(no decompilation)"
    except Exception as e:  # noqa
        return f"(decompile failed: {e})"


def safe(s):
    return re.sub(r"[^A-Za-z0-9_]+", "_", s)[:120]


rx = re.compile(pat)
vrx = re.compile(vpat) if vpat else None
entries = list(idautils.Entries())   # (index, ordinal, ea, name)
index = []
for _, _, ea, name in entries:
    if not name:
        continue
    d = demangle(name)
    if rx.search(name) or rx.search(d):
        body = decomp(ea)
        fn = os.path.join(out_dir, f"fn_{ea:X}_{safe(d)}.c")
        with open(fn, "w", encoding="utf-8") as f:
            f.write(f"// {d}\n// {name}\n// ea=0x{ea:X}\n\n{body}\n")
        index.append(f"FN 0x{ea:X} {d}")
    if vrx and name.startswith("??_7") and (vrx.search(name) or vrx.search(d)):
        lines = [f"// vtable {d}  ({name}) at 0x{ea:X}"]
        for i in range(400):
            p = ida_bytes.get_qword(ea + 8 * i)
            if i > 0 and ida_name.get_name(ea + 8 * i):   # next symbol starts: end of this vtable
                break
            tn = ida_name.get_name(p) or ""
            lines.append(f"[{i:3d}] +0x{8*i:03X} -> 0x{p:X} {demangle(tn) if tn else '(unnamed)'}")
        with open(os.path.join(out_dir, f"vt_{safe(d)}.txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        index.append(f"VT 0x{ea:X} {d} slots={len(lines)-1}")

with open(os.path.join(out_dir, "index.txt"), "w", encoding="utf-8") as f:
    f.write("\n".join(index) + "\n")
print("items:", len(index))
idapro.close_database(save=True)
