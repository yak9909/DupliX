"""Print the C string stored at (or inside) exported data symbols matching a regex.

usage: python ida_strings.py <db.i64> <symbol_regex>
"""
import re
import sys

import idapro  # must come first

idapro.open_database(sys.argv[1], run_auto_analysis=False)
import ida_bytes  # noqa: E402
import idautils  # noqa: E402
import idc  # noqa: E402

rx = re.compile(sys.argv[2])
for _, _, ea, name in list(idautils.Entries()):
    if not name:
        continue
    d = idc.demangle_name(name, idc.get_inf_attr(idc.INF_SHORT_DN)) or name
    if not (rx.search(name) or rx.search(d)):
        continue
    raw = ida_bytes.get_bytes(ea, 64) or b""
    s = raw.split(b"\0")[0]
    # some match names hold a pointer to the string
    ptr = ida_bytes.get_qword(ea)
    ps = (ida_bytes.get_bytes(ptr, 64) or b"").split(b"\0")[0] if ptr else b""
    print(f"{d}\n   inline={s!r}\n   via_ptr={ps!r}")
idapro.close_database(save=False)
