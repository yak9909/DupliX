"""Compares dx_<case>.png with twin_<case>.png (tests/ae/text_twins.jsx): the copies of a text layer in 3D Layers
against real duplicated text layers. Usage: check_twins.py <dir>"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def main() -> int:
    d = Path(sys.argv[1])
    ok = True
    for dxp in sorted(d.glob("dx_*.png")):
        case = dxp.stem[3:]
        tw = d / f"twin_{case}.png"
        if not tw.exists():
            print(f"{case}: twin MISSING")
            ok = False
            continue
        a = np.asarray(Image.open(dxp).convert("RGB"), dtype=np.int16)
        b = np.asarray(Image.open(tw).convert("RGB"), dtype=np.int16)
        diff = np.abs(a - b).max(axis=2)
        n = int((diff > 0).sum())
        lit = int((a.max(axis=2) > 40).sum())
        if n:
            ys, xs = np.nonzero(diff)
            print(f"{case}: DIFF {n} px (>8: {int((diff > 8).sum())}) max {int(diff.max())} "
                  f"box x {xs.min()}..{xs.max()} y {ys.min()}..{ys.max()} (lit px {lit})")
            ok = False
        else:
            print(f"{case}: same (lit px {lit})")
    print("TWINS SAME" if ok else "TWINS DIFFER")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
