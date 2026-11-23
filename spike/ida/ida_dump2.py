"""Dump from an already-analyzed database (.i64): exported functions, vtables, and callers.

usage: python ida_dump2.py <db.i64> <out_dir> <export_regex> [<vtable_regex>] [<caller_regex>]
  export_regex : exported names (mangled or demangled) to decompile
  vtable_regex : vtable symbols (??_7...) whose slots are listed
  caller_regex : for exported functions matching this, list every caller (code xref and
                 data xref, e.g. vtable slot) and decompile the calling functions once each
"""
import os
import re
import sys

import idapro  # must come first (idalib)

dbp, out_dir, pat = sys.argv[1], sys.argv[2], sys.argv[3]
vpat = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] else None
cpat = sys.argv[5] if len(sys.argv) > 5 and sys.argv[5] else None
os.makedirs(out_dir, exist_ok=True)
idapro.open_database(dbp, run_auto_analysis=False)

import ida_bytes  # noqa: E402
import ida_funcs  # noqa: E402
import ida_hexrays  # noqa: E402
import ida_name  # noqa: E402
import idautils  # noqa: E402
import idc  # noqa: E402

ida_hexrays.init_hexrays_plugin()


def demangle(n):
    return idc.demangle_name(n, idc.get_inf_attr(idc.INF_SHORT_DN)) or n


def fname(ea):
    f = ida_funcs.get_func(ea)
    if not f:
        return f"(no func @0x{ea:X})"
    n = ida_name.get_name(f.start_ea) or f"sub_{f.start_ea:X}"
    return demangle(n)


def decomp(ea):
    if not ida_funcs.get_func(ea):
        idc.create_insn(ea)
        ida_funcs.add_func(ea)
    try:
        c = ida_hexrays.decompile(ea)
        return str(c) if c else "(no decompilation)"
    except Exception as e:  # noqa
        return f"(decompile failed: {e})"


def safe(s):
    return re.sub(r"[^A-Za-z0-9_]+", "_", s)[:110]


def write_fn(ea, prefix="fn"):
    f = ida_funcs.get_func(ea)
    start = f.start_ea if f else ea
    path = os.path.join(out_dir, f"{prefix}_{start:X}_{safe(fname(start))}.c")
    if not os.path.exists(path):
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(f"// {fname(start)}\n// ea=0x{start:X}\n\n{decomp(start)}\n")
    return start


rx = re.compile(pat)
vrx = re.compile(vpat) if vpat else None
crx = re.compile(cpat) if cpat else None
index = []
for _, _, ea, name in list(idautils.Entries()):
    if not name:
        continue
    d = demangle(name)
    if rx.search(name) or rx.search(d):
        write_fn(ea)
        index.append(f"FN 0x{ea:X} {d}")
    if crx and (crx.search(name) or crx.search(d)):
        for x in idautils.XrefsTo(ea, 0):
            frm = x.frm
            if ida_funcs.get_func(frm):
                s = write_fn(frm, "caller")
                index.append(f"CALLER of {d}: 0x{frm:X} in 0x{s:X} {fname(s)}")
            else:
                index.append(f"DATAREF to {d}: 0x{frm:X} {demangle(ida_name.get_name(frm) or '')}")
    if vrx and name.startswith("??_7") and (vrx.search(name) or vrx.search(d)):
        lines = [f"// vtable {d}  ({name}) at 0x{ea:X}"]
        for i in range(600):
            if i > 0 and ida_name.get_name(ea + 8 * i):
                break
            p = ida_bytes.get_qword(ea + 8 * i)
            tn = ida_name.get_name(p) or ""
            lines.append(f"[{i:3d}] +0x{8*i:03X} -> 0x{p:X} {demangle(tn) if tn else '(unnamed)'}")
        with open(os.path.join(out_dir, f"vt_{safe(d)}.txt"), "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")
        index.append(f"VT 0x{ea:X} {d} slots={len(lines)-1}")

with open(os.path.join(out_dir, "index.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(index) + "\n")
print("items:", len(index))
idapro.close_database(save=False)
