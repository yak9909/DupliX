"""SPIKE check for spike_screen.jsx: every s_<case>.png must equal the pattern
R = 7x, G = 13y, B = 5(x+y) (mod 256) at comp pixel (x, y) when the plane lands 1:1 on the comp."""
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def main() -> int:
    d = Path(sys.argv[1])
    fails = 0
    for p in sorted(list(d.glob("s_*.png")) + list(d.glob("r_*.tif"))):
        a = np.asarray(Image.open(p).convert("RGB"), dtype=np.int16)
        h, w, _ = a.shape
        y, x = np.mgrid[0:h, 0:w]
        exp = np.stack([(x * 7) % 256, (y * 13) % 256, ((x + y) * 5) % 256], axis=2).astype(np.int16)
        diff = np.abs(a - exp).max(axis=2)
        m = int(diff.max())
        ok = m <= 1
        fails += 0 if ok else 1
        bad = np.argwhere(diff > 1)
        where = f" first bad px (y,x)={tuple(bad[0])} got={tuple(a[tuple(bad[0])])} exp={tuple(exp[tuple(bad[0])])}" if len(bad) else ""
        print(f"  {p.stem:<14} {'PASS' if ok else 'FAIL'} max={m} >1: {int((diff > 1).sum())} of {diff.size}{where}")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
