"""Checks tests/ae/vector_probe.jsx: one copy with no ranges must look like the plain layer.
For every case with plain_<case>.png, compares fm1_<case> (Fast) and nm1_<case> (3D Layers) with it.
The placement matters, not the edge resampling (Fast's bilinear differs from AE's on edges by up to ~26 after a
3x3 average, more at half resolution): the brightness centroid must agree within 0.1 px, the brightness mass
within 1%, and the mean difference must be <= 0.5. The 3x3-averaged max difference is printed for reference
(check_fast_mode.py's fast_identity uses <= 24 on AV layers).
Also prints the white pixel count of the 10-copy pictures (fm_ / nm_; 10 copies cover more).
Cases without plain_<case>.png (shape_rot) compare nm1 with fm1.
usage: python check_vector.py <out dir> [fm|nm|both]   (exit 0 = pass)"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def load(p):
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.float32)


def box3(a):
    p = np.pad(a, ((1, 1), (1, 1), (0, 0)), mode="edge")
    h, w = a.shape[:2]
    return sum(p[y:y + h, x:x + w] for y in range(3) for x in range(3)) / 9.0


def centroid(a):
    m = np.clip(a.mean(axis=2) - 30, 0, None)
    ys, xs = np.indices(m.shape)
    s = m.sum()
    return ((xs * m).sum() / s, (ys * m).sum() / s, s) if s > 0 else (0.0, 0.0, 0.0)


def white(a):
    return int((a.mean(axis=2) > 200).sum())


def main() -> int:
    out = Path(sys.argv[1])
    which = sys.argv[2] if len(sys.argv) > 2 else "both"
    modes = ["fm1", "nm1"] if which == "both" else [which + "1"]
    ok = True
    for fm1 in sorted(out.glob("fm1_*.png")):
        case = fm1.stem[len("fm1_"):]
        ref = out / f"plain_{case}.png"
        # without a plain reference (shape_rot), 3D Layers is compared with Fast
        r = load(ref) if ref.exists() else load(fm1)
        line = f"  {case:13} white {white(r):6}{'' if ref.exists() else ' (vs fm1)'}"
        for m in modes:
            if m == "fm1" and not ref.exists():
                continue
            p = out / f"{m}_{case}.png"
            if not p.exists():
                line += f"  {m} MISSING"
                ok = False
                continue
            a = load(p)
            if a.shape != r.shape:
                line += f"  {m} size {a.shape[1]}x{a.shape[0]} != {r.shape[1]}x{r.shape[0]} FAIL"
                ok = False
                continue
            mean = float(np.abs(a - r).max(axis=2).mean())
            avg = float(np.abs(box3(a) - box3(r)).max())
            (xa, ya, sa), (xr, yr, sr) = centroid(a), centroid(r)
            shift = float(np.hypot(xa - xr, ya - yr))
            mass = sa / sr if sr > 0 else 0.0
            good = mean <= 0.5 and shift <= 0.1 and abs(mass - 1) <= 0.01
            ok = ok and good
            line += (f"  {m} shift {shift:.3f} mass {mass:.4f} mean {mean:.2f} avgmax {avg:5.1f} "
                     f"{'PASS' if good else 'FAIL'}")
        for m in ("fm", "nm"):
            p = out / f"{m}_{case}.png"
            if p.exists():
                line += f"  {m}10 white {white(load(p)):6}"
        print(line)
    print("VECTOR CHECK", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
