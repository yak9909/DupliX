"""Full auto-analysis of a binary, then save the .i64 next to it (reuse later with ida_dump.py).

usage: python ida_analyze.py <binary>
"""
import sys
import time

import idapro  # must come first (idalib)

t0 = time.time()
idapro.open_database(sys.argv[1], run_auto_analysis=True)
import ida_auto  # noqa: E402

ida_auto.auto_wait()
print(f"analysis done in {time.time() - t0:.0f}s", flush=True)
idapro.close_database(save=True)
print("saved", flush=True)
