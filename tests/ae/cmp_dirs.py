# Compares images pairwise and exits 1 if any pair differs:
#   python tests/ae/cmp_dirs.py <dir A> <dir B>                   same file names in two folders
#   python tests/ae/cmp_dirs.py <dir> <dir> --suffix OFF ND       <name>_OFF.tif against <name>_ND.tif
import os
import sys

import numpy as np
from PIL import Image

a, b = sys.argv[1:3]
sa = sb = None
if len(sys.argv) > 5 and sys.argv[3] == "--suffix":
    sa, sb = sys.argv[4], sys.argv[5]
same = differ = 0
for name in sorted(os.listdir(a)):
    if not name.lower().endswith((".tif", ".tiff", ".png")):
        continue
    if sa:
        stem, ext = os.path.splitext(name)
        if not stem.endswith("_" + sa):
            continue
        other = stem[: -len(sa)] + sb + ext
    else:
        other = name
    pb = os.path.join(b, other)
    if not os.path.exists(pb):
        print(f"missing: {other}")
        differ += 1
        continue
    x = np.asarray(Image.open(os.path.join(a, name))).astype(np.int64)
    y = np.asarray(Image.open(pb)).astype(np.int64)
    if x.shape != y.shape:
        print(f"{name}: shapes {x.shape} {y.shape}")
        differ += 1
        continue
    d = np.abs(x - y)
    if d.max() == 0:
        same += 1
    else:
        differ += 1
        dd = d.max(axis=-1) if d.ndim == 3 else d
        print(f"{name}: max {d.max()} pixels differing {np.mean(dd > 0) * 100:.3f}%")
print(f"identical {same}, differing {differ}")
sys.exit(1 if differ else 0)
