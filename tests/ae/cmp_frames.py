# Compares the viewer frames of two preview variants (preview_step.jsx PNGs) frame by frame:
#   python tests/ae/cmp_frames.py build/test/preview pv_merge10 pv_merge10_draft
import os
import sys

import numpy as np
from PIL import Image

d, a, b = sys.argv[1:4]
n = 0
worst = 0
for i in range(1, 1000):
    pa, pb = (os.path.join(d, f"frame_{v}_{i}.png") for v in (a, b))
    if not (os.path.exists(pa) and os.path.exists(pb)):
        continue
    x = np.asarray(Image.open(pa).convert("RGBA")).astype(int)
    y = np.asarray(Image.open(pb).convert("RGBA")).astype(int)
    diff = np.abs(x - y).max(axis=2)
    n += 1
    worst = max(worst, int(diff.max()))
    if i <= 3 or diff.max() > 2:
        print(f"frame {i}: max {diff.max()} mean {diff.mean():.4f} >2 {np.mean(diff > 2) * 100:.3f}% >32 {np.mean(diff > 32) * 100:.3f}%")
print(f"{n} frames compared, worst max {worst}")
