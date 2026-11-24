"""Checks tests/ae/fx_remove.jsx: for every case, c_<name>.png (layer whose effect was removed) must equal
r_<name>.png (same layer that never had the effect).
usage: python check_fx_remove.py <out dir>  -> exit 0 if all cases match (max diff <= 1)."""
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def main() -> int:
    d = Path(sys.argv[1])
    fails = 0
    for c in sorted(d.glob("c_*.png")):
        name = c.stem[2:]
        a = np.asarray(Image.open(c).convert("RGBA"), dtype=np.int16)
        b = np.asarray(Image.open(d / f"r_{name}.png").convert("RGBA"), dtype=np.int16)
        m = int(np.abs(a - b).max())
        ok = m <= 1
        if name == "remove_undo":  # the removal was undone: the copies are back
            ok = int((a[..., :3].min(axis=2) > 200).sum()) > 6400 * 1.5
        fails += 0 if ok else 1
        white_a = int((a[..., :3].min(axis=2) > 200).sum())
        white_b = int((b[..., :3].min(axis=2) > 200).sum())
        print(f"  {name:<18} {'PASS' if ok else 'FAIL'} max={m} white px: removed={white_a} never={white_b}")
        # While the effect was on (mid_<name>_*.png, 3D target) the copies must have been there; a 2D
        # target keeps its plain look.
        for mid in sorted(p for p in d.glob(f"mid_{name}_*.png") if p.stem[len(name) + 5:].isdigit()):
            x = np.asarray(Image.open(mid).convert("RGBA"), dtype=np.int16)
            white_m = int((x[..., :3].min(axis=2) > 200).sum())
            if name == "2d_remove":
                ok2 = white_m == white_b
                what = "2D: effect leaves the layer as is"
            elif name == "remove_undo" and mid.stem.endswith("_4"):
                ok2 = int(np.abs(x - b).max()) <= 1
                what = "after removal == never had it"
            else:
                ok2 = white_m > 6400 * 1.5  # one 80x80 target is at most 6400 px
                what = "3D: copies were shown"
            fails += 0 if ok2 else 1
            print(f"    {mid.stem:<22} {'PASS' if ok2 else 'FAIL'} {what} (white px {white_m})")
    print("FX REMOVE ALL PASS" if fails == 0 else f"FX REMOVE FAILURES: {fails}")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
