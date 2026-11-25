# Compares <comp>_on_<frame>.tif with <comp>_off_<frame>.tif (run_user_merge.ps1): statistics like check_merge.py
# (raw and 5x5 averaged differences) and a difference image diff_<comp>_<frame>.png (off | on | |difference| x4).
#   python tests/ae/cmp_merge_frames.py <dir>
import os
import re
import sys

import numpy as np
from PIL import Image

d = sys.argv[1]
for name in sorted(os.listdir(d)):
    m = re.match(r"(\d+)_on_(\d+)\.tif$", name)
    if not m:
        continue
    comp, frame = m.group(1), m.group(2)
    off = os.path.join(d, f"{comp}_off_{frame}.tif")
    if not os.path.exists(off):
        print(f"{comp} frame {frame}: no merge-off frame")
        continue
    a = np.asarray(Image.open(os.path.join(d, name)).convert("RGBA")).astype(np.float64)
    b = np.asarray(Image.open(off).convert("RGBA")).astype(np.float64)
    diff = np.abs(a - b).max(axis=2)
    k = 5
    pad = lambda x: np.pad(x, ((k // 2, k // 2), (k // 2, k // 2), (0, 0)), mode="edge")
    def avg(x):
        p = pad(x)
        s = np.zeros_like(x)
        for dy in range(k):
            for dx in range(k):
                s += p[dy:dy + x.shape[0], dx:dx + x.shape[1]]
        return s / (k * k)
    da = np.abs(avg(a) - avg(b)).max(axis=2)
    print(f"{comp} frame {frame:>4}: max {diff.max():5.0f} mean {diff.mean():.3f} >2 {np.mean(diff > 2) * 100:6.2f}% "
          f">32 {np.mean(diff > 32) * 100:6.3f}%  avg5 max {da.max():5.1f} >16 {np.mean(da > 16) * 100:6.3f}%")
    vis = np.concatenate([b[:, :, :3], a[:, :, :3], np.repeat(np.minimum(diff * 4, 255)[:, :, None], 3, axis=2)], axis=1)
    Image.fromarray(vis.astype(np.uint8)).resize((vis.shape[1] // 2, vis.shape[0] // 2)).save(
        os.path.join(d, f"diff_{comp}_{frame}.png"))
