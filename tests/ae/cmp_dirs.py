# Compares images pairwise and exits 1 if any pair differs:
#   python tests/ae/cmp_dirs.py <dir A> <dir B>                   same file names in two folders
#   python tests/ae/cmp_dirs.py <dir> <dir> --suffix OFF ND       <name>_OFF.tif against <name>_ND.tif
#   ... --tol 0.1 0.001   a pair passes when its mean difference <= 0.1 levels and <= 0.1 % of the values differ by
#                         more than 2 levels (otherwise it must be identical)
import os
import sys

import numpy as np
from PIL import Image

a, b = sys.argv[1:3]
sa = sb = None
tol = None
args = sys.argv[3:]
while args:
    if args[0] == "--suffix":
        sa, sb, args = args[1], args[2], args[3:]
    elif args[0] == "--tol":
        tol, args = (float(args[1]), float(args[2])), args[3:]
    else:
        sys.exit(f"unknown argument {args[0]}")
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
    if x.dtype == np.int64 and x.max() > 255:  # 16-bit images: in 8-bit levels
        d = d / 257.0
    if d.max() == 0:
        same += 1
    elif tol and d.mean() <= tol[0] and np.mean(d > 2) <= tol[1]:
        same += 1
        print(f"{name}: within tolerance (max {d.max():.1f} mean {d.mean():.4f} >2 {np.mean(d > 2) * 100:.3f}%)")
    else:
        differ += 1
        dd = d.max(axis=-1) if d.ndim == 3 else d
        print(f"{name}: max {d.max()} pixels differing {np.mean(dd > 0) * 100:.3f}%")
print(f"identical {same}, differing {differ}")
sys.exit(1 if differ else 0)
