# Viewer frame latency from preview_step.jsx: frame_ms.txt ("<comp> <frame> <start epoch ms>") and the
# PNGs' write times. Prints the average latency per comp (frame 0 dropped: setup work).
#   python tests/ae/frame_latency.py build/test/preview
import os
import sys
from collections import defaultdict

d = sys.argv[1]
lat = defaultdict(list)
for line in open(os.path.join(d, "frame_ms.txt"), encoding="utf-8"):
    comp, n, t0 = line.split()
    p = os.path.join(d, f"frame_{comp}_{n}.png")
    if int(n) == 0 or not os.path.exists(p):
        continue
    lat[comp].append(os.path.getmtime(p) * 1000 - int(t0))
for comp in sorted(lat):
    v = sorted(lat[comp])
    print(f"{comp:<18} avg {sum(v) / len(v):7.1f} ms  median {v[len(v) // 2]:7.1f} ms  (n={len(v)})")
