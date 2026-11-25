# Compares every image with the same name in two folders (e.g. two runs of a test with and without a switch):
#   python tests/ae/cmp_dirs.py <dir A> <dir B>
# Prints the images that differ and the count of identical ones.
import os
import sys

import numpy as np
from PIL import Image

a, b = sys.argv[1:3]
same = differ = 0
for name in sorted(os.listdir(a)):
    if not name.lower().endswith((".tif", ".tiff", ".png")):
        continue
    pb = os.path.join(b, name)
    if not os.path.exists(pb):
        print(f"missing in B: {name}")
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
