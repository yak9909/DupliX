"""Pixel comparison of two renders (RGBA, 8 bpc).
usage: python cmp.py <a> <b> [<diff.png>]  -> prints "max=.. mean=.. >2: n >8: n >32: n of N"; exit 0 if max <= 1."""
import sys

import numpy as np
from PIL import Image


def main() -> int:
    a = np.asarray(Image.open(sys.argv[1]).convert("RGBA"), dtype=np.int16)
    b = np.asarray(Image.open(sys.argv[2]).convert("RGBA"), dtype=np.int16)
    if a.shape != b.shape:
        print(f"size mismatch {a.shape} vs {b.shape}")
        return 2
    d = np.abs(a - b).max(axis=2)
    n = d.size
    print(f"max={int(d.max())} mean={d.mean():.4f} >2: {int((d > 2).sum())} >8: {int((d > 8).sum())} "
          f">32: {int((d > 32).sum())} of {n}")
    if len(sys.argv) > 3:
        Image.fromarray(np.clip(d * 8, 0, 255).astype(np.uint8)).save(sys.argv[3])
    return 0 if d.max() <= 1 else 1


if __name__ == "__main__":
    sys.exit(main())
