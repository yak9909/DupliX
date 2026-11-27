"""Compares off_<case>.png with on_<case>.png (tests/ae/text_perchar_same.jsx): pixels that differ, the largest
difference per channel, and the bounding box of the differences. Usage: check_same.py <dir>"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def main() -> int:
    d = Path(sys.argv[1])
    same = True
    for off in sorted(d.glob("off_*.png")):
        case = off.stem[4:]
        on = d / f"on_{case}.png"
        if not on.exists():
            print(f"{case}: on_ MISSING")
            same = False
            continue
        a = np.asarray(Image.open(off).convert("RGB"), dtype=np.int16)
        b = np.asarray(Image.open(on).convert("RGB"), dtype=np.int16)
        diff = np.abs(a - b).max(axis=2)
        n = int((diff > 0).sum())
        lit = int((a.max(axis=2) > 40).sum())
        if n:
            ys, xs = np.nonzero(diff)
            box = f"box x {xs.min()}..{xs.max()} y {ys.min()}..{ys.max()}"
            big = int((diff > 8).sum())
            print(f"{case}: DIFF {n} px (>8: {big}) max {int(diff.max())} {box} (lit px {lit})")
            same = False
        else:
            print(f"{case}: same (lit px {lit})")
    print("ALL SAME" if same else "NOT SAME")
    return 0 if same else 1


if __name__ == "__main__":
    sys.exit(main())
