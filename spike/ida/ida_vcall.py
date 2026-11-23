"""Find functions containing indirect calls `call qword ptr [reg+DISP]` for given displacements.

usage: python ida_vcall.py <db.i64> <out_dir> <disp_hex> [<disp_hex> ...] [--decomp N]
Writes index.txt (disp -> call sites with function names) and decompiles up to N functions per disp.
"""
import os
import re
import sys

import idapro  # noqa

args = sys.argv[1:]
nde = 0
if "--decomp" in args:
    i = args.index("--decomp")
    nde = int(args[i + 1])
    args = args[:i] + args[i + 2:]
dbp, out_dir = args[0], args[1]
disps = [int(x, 16) for x in args[2:]]
os.makedirs(out_dir, exist_ok=True)
idapro.open_database(dbp, run_auto_analysis=False)
import ida_funcs  # noqa: E402
import ida_hexrays  # noqa: E402
import ida_name  # noqa: E402
import ida_ua  # noqa: E402
import idautils  # noqa: E402
import idc  # noqa: E402

ida_hexrays.init_hexrays_plugin()


def demangle(n):
    return idc.demangle_name(n, idc.get_inf_attr(idc.INF_SHORT_DN)) or n


def fname(ea):
    return demangle(ida_name.get_name(ea) or f"sub_{ea:X}")


hits = {d: [] for d in disps}
for fea in idautils.Functions():
    f = ida_funcs.get_func(fea)
    for ea in idautils.FuncItems(f.start_ea):
        m = idc.print_insn_mnem(ea)
        if m not in ("call", "mov", "jmp"):
            continue
        insn = ida_ua.insn_t()
        ida_ua.decode_insn(insn, ea)
        op = insn.ops[0] if m != "mov" else insn.ops[1]
        if op.type == ida_ua.o_displ and op.addr in hits:
            hits[op.addr].append((ea, f.start_ea))

lines = []
for d in disps:
    funcs = sorted(set(s for _, s in hits[d]))
    lines.append(f"DISP 0x{d:X}: {len(hits[d])} sites in {len(funcs)} funcs")
    for s in funcs:
        lines.append(f"  0x{s:X} {fname(s)}")
    for s in funcs[:nde]:
        p = os.path.join(out_dir, f"fn_{s:X}_{re.sub(r'[^A-Za-z0-9_]+', '_', fname(s))[:90]}.c")
        if not os.path.exists(p):
            try:
                body = str(ida_hexrays.decompile(s))
            except Exception as e:  # noqa
                body = f"(decompile failed: {e})"
            with open(p, "w", encoding="utf-8") as fh:
                fh.write(f"// {fname(s)}\n// ea=0x{s:X}\n\n{body}\n")
with open(os.path.join(out_dir, "index.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines) + "\n")
print("done")
idapro.close_database(save=False)
