"""Decompile given functions (by address or name regex) and their callees to a depth.

usage: python ida_fn.py <db.i64> <out_dir> <depth> <target> [<target> ...]
  target: 0xADDR, or name regex (matched against demangled exported/named functions)
  depth : 0 = only the targets, 1 = plus direct callees, ...
Writes fn_<ea>_<name>.c per function and index.txt (call tree).
"""
import os
import re
import sys

import idapro  # must come first (idalib)

dbp, out_dir, depth = sys.argv[1], sys.argv[2], int(sys.argv[3])
targets = sys.argv[4:]
os.makedirs(out_dir, exist_ok=True)
idapro.open_database(dbp, run_auto_analysis=False)

import ida_funcs  # noqa: E402
import ida_hexrays  # noqa: E402
import ida_name  # noqa: E402
import idautils  # noqa: E402
import idc  # noqa: E402

ida_hexrays.init_hexrays_plugin()


def demangle(n):
    return idc.demangle_name(n, idc.get_inf_attr(idc.INF_SHORT_DN)) or n


def fname(ea):
    n = ida_name.get_name(ea) or f"sub_{ea:X}"
    return demangle(n)


def safe(s):
    return re.sub(r"[^A-Za-z0-9_]+", "_", s)[:100]


def decomp(ea):
    try:
        c = ida_hexrays.decompile(ea)
        return str(c) if c else "(no decompilation)"
    except Exception as e:  # noqa
        return f"(decompile failed: {e})"


def callees(ea):
    f = ida_funcs.get_func(ea)
    out = []
    if not f:
        return out
    for item in idautils.FuncItems(f.start_ea):
        for x in idautils.CodeRefsFrom(item, 0):
            g = ida_funcs.get_func(x)
            if g and g.start_ea == x and x != f.start_ea and x not in out:
                out.append(x)
    return out


import ida_bytes  # noqa: E402

starts = []
vt_lines = []
for t in targets:
    if t.startswith("vt:"):
        # vt:<regex>[:<nslots>] -> vtable symbols; decompile first n slots
        parts = t.split(":")
        rx = re.compile(parts[1])
        nslots = int(parts[2]) if len(parts) > 2 else 8
        for ea, n in idautils.Names():
            d = demangle(n)
            if "vftable" in d and (rx.search(n) or rx.search(d)):
                vt_lines.append(f"VT 0x{ea:X} {d}")
                for i in range(nslots):
                    p = ida_bytes.get_qword(ea + 8 * i)
                    vt_lines.append(f"  [{i}] +{8*i} -> 0x{p:X} {fname(p)}")
                    if ida_funcs.get_func(p) or (idc.create_insn(p) and ida_funcs.add_func(p)):
                        starts.append(ida_funcs.get_func(p).start_ea)
        continue
    if t.startswith("asm:"):
        # asm:0xADDR -> full disassembly of the function, with names of call targets
        ea0 = int(t[4:], 16)
        f = ida_funcs.get_func(ea0)
        lines = []
        for item in idautils.FuncItems(f.start_ea):
            lines.append(f"{item:X}  {idc.GetDisasm(item)}")
        with open(os.path.join(out_dir, f"asm_{f.start_ea:X}_{safe(fname(f.start_ea))}.asm"), "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")
        continue
    if t.lower().startswith("0x"):
        ea = int(t, 16)
        if not ida_funcs.get_func(ea):
            idc.create_insn(ea)
            ida_funcs.add_func(ea)
        f = ida_funcs.get_func(ea)
        starts.append(f.start_ea if f else ea)
    else:
        rx = re.compile(t)
        for ea, n in idautils.Names():
            if ida_funcs.get_func(ea) and ida_funcs.get_func(ea).start_ea == ea:
                if rx.search(n) or rx.search(demangle(n)):
                    starts.append(ea)

index = []
seen = set()


def visit(ea, d, indent):
    name = fname(ea)
    mark = "" if ea not in seen else " (seen)"
    index.append(f"{'  ' * indent}0x{ea:X} {name}{mark}")
    if ea in seen:
        return
    seen.add(ea)
    path = os.path.join(out_dir, f"fn_{ea:X}_{safe(name)}.c")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(f"// {name}\n// ea=0x{ea:X}\n\n{decomp(ea)}\n")
    if d < depth:
        for c in callees(ea):
            visit(c, d + 1, indent + 1)


for s in starts:
    visit(s, 0, 0)

with open(os.path.join(out_dir, "index.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(vt_lines + index) + "\n")
print("funcs:", len(seen))
idapro.close_database(save=False)
