"""Checks tests/ae/world_xform.jsx: wx_<case>.png (placement in the World Transform parameters) must equal
wl_<case>.png (the same placement on the layer's own transform), and the copies must be visible.
usage: python check_world_xform.py <out dir>  -> exit 0 if all cases pass (max diff <= 1)."""
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def main() -> int:
    d = Path(sys.argv[1])
    fails = 0
    cases = sorted(d.glob("wx_*.png"))
    for a_path in cases:
        name = a_path.stem[3:]
        a = np.asarray(Image.open(a_path).convert("RGBA"), dtype=np.int16)
        b = np.asarray(Image.open(d / f"wl_{name}.png").convert("RGBA"), dtype=np.int16)
        diff = np.abs(a - b).max(axis=2)
        m = int(diff.max())
        bg = np.array([26, 26, 31])  # BG solid (0.1, 0.1, 0.12)
        drawn = int((np.abs(a[..., :3] - bg).max(axis=2) > 8).sum())
        ok = m <= 1 and drawn > 2000
        fails += 0 if ok else 1
        print(f"  {name:<12} {'PASS' if ok else 'FAIL'} max={m} px>1={int((diff > 1).sum())} drawn px={drawn}")
    if not cases:
        print("  no cases")
        fails += 1
    print("WORLD ALL PASS" if fails == 0 else f"WORLD FAILURES: {fails}")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
