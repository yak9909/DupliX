# Checks preview_edit.jsx (run_preview.ps1 -Edit): every edited frame edit_<comp>_<n>.png must equal the fresh
# duplicate's ref_<comp>_<n>.png exactly, and must differ from the frame before the edit (the edit did something).
#   python tests/ae/check_preview_edit.py build/test/preview
import glob
import os
import re
import sys

import numpy as np
from PIL import Image

d = sys.argv[1]
fails = 0
n = 0
for p in sorted(glob.glob(os.path.join(d, "ref_*_*.png"))):
    m = re.match(r"ref_(.+)_(\d+)\.png$", os.path.basename(p))
    comp, k = m.group(1), int(m.group(2))
    e = os.path.join(d, f"edit_{comp}_{k}.png")
    prev = os.path.join(d, f"edit_{comp}_{k - 1}.png")
    a = np.asarray(Image.open(e).convert("RGBA"), dtype=np.int16)
    b = np.asarray(Image.open(p).convert("RGBA"), dtype=np.int16)
    c = np.asarray(Image.open(prev).convert("RGBA"), dtype=np.int16)
    diff = int(np.abs(a - b).max()) if a.shape == b.shape else 999
    changed = a.shape != c.shape or int(np.abs(a - c).max()) > 0
    ok = diff == 0 and changed
    n += 1
    fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} {comp} edit {k}: max diff to fresh {diff}, changed by the edit {changed}")
if n == 0:
    print("FAIL no frames")
    sys.exit(1)
print("EDIT ALL PASS" if not fails else f"EDIT FAIL ({fails} of {n})")
sys.exit(1 if fails else 0)
