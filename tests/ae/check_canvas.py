"""Checks Canvas in tests/ae/vector_probe.jsx: 10 shape copies spread past the comp on a Fast (2D) layer scaled to
50%. With Comp Size the drawing ends at the comp-sized frame (shown at 50%: the middle quarter of the comp), with
Fit Copies the copies outside it are drawn too. Passes when fm_canvas_fit has white pixels outside the middle
quarter, fm_canvas_comp has none there, and inside the middle quarter the two agree.
usage: python check_canvas.py <out dir>   (exit 0 = pass)"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def main() -> int:
    out = Path(sys.argv[1])
    comp = np.asarray(Image.open(out / "fm_canvas_comp.png").convert("L"), dtype=np.float32)
    fit = np.asarray(Image.open(out / "fm_canvas_fit.png").convert("L"), dtype=np.float32)
    h, w = comp.shape
    inner = np.zeros_like(comp, dtype=bool)
    inner[h // 4 + 1:h - h // 4 - 1, w // 4 + 1:w - w // 4 - 1] = True
    outer = ~np.pad(inner[2:-2, 2:-2], 2, constant_values=False) & ~inner
    comp_out = int((comp[outer] > 128).sum())
    fit_out = int((fit[outer] > 128).sum())
    same = float(np.abs(comp[inner] - fit[inner]).max())
    ok = comp_out == 0 and fit_out > 100 and same <= 2
    print(f"  outside the middle quarter: comp {comp_out} px, fit {fit_out} px; inside max diff {same:.0f}")
    print("CANVAS CHECK", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
