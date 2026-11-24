"""Lists imported functions matching a regex and the functions that reference them.

usage: python ida_imports_xref.py <db.i64> <regex>
Prints: import name, its address, and each referencing function (address, name, size).
"""
import re
import sys

import idapro  # noqa  (must come first)

idapro.open_database(sys.argv[1], run_auto_analysis=False)
import ida_funcs  # noqa: E402
import ida_nalt  # noqa: E402
import ida_name  # noqa: E402
import idautils  # noqa: E402

pat = re.compile(sys.argv[2])
hits = []


def cb(ea, name, ordinal):
    if name and pat.search(name):
        hits.append((ea, name))
    return True


for i in range(ida_nalt.get_import_module_qty()):
    ida_nalt.enum_import_names(i, cb)
for ea, name in hits:
    print(f"{ea:#x} {name[:110]}")
    seen = set()
    for x in idautils.XrefsTo(ea):
        f = ida_funcs.get_func(x.frm)
        if f and f.start_ea not in seen:
            seen.add(f.start_ea)
            print(f"    <- {f.start_ea:#x} {ida_name.get_name(f.start_ea)} size={f.end_ea - f.start_ea}")
idapro.close_database(save=False)
