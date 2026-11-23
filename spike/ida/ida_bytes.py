"""Print qwords/doubles at addresses. usage: python ida_bytes.py <db> <0xADDR> [count]"""
import struct
import sys

import idapro  # noqa

idapro.open_database(sys.argv[1], run_auto_analysis=False)
import ida_bytes  # noqa: E402

ea = int(sys.argv[2], 16)
n = int(sys.argv[3]) if len(sys.argv) > 3 else 4
for i in range(n):
    q = ida_bytes.get_qword(ea + 8 * i)
    print(f"0x{ea + 8 * i:X}: 0x{q:016X}  double={struct.unpack('<d', struct.pack('<Q', q))[0]!r}")
idapro.close_database(save=False)
