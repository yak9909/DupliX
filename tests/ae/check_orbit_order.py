"""Per-frame fast-vs-normal difference for tests/ae/orbit_order.jsx (r_orb_fm_NNNNN.tif vs r_orb_nm_NNNNN.tif).
usage: python check_orbit_order.py <out dir>  -> one line per frame; writes cmp_NNNNN.png for the worst frames.
Exit 0 if every frame passes (mean <= 2, >32 on <= 2% of px). A wrong drawing order gave mean 2.3-13;
copies seen almost edge-on (orbit at 90/270 deg) differ only by resampling (mean < 1, >32 on ~1.1%)."""
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def main() -> int:
    d = Path(sys.argv[1])
    rows = []
    for a in sorted(d.glob("r_orb_fm_*.tif")):
        f = a.stem.split("_")[-1]
        b = d / f"r_orb_nm_{f}.tif"
        if not b.exists():
            continue
        x = np.asarray(Image.open(a).convert("RGBA"), dtype=np.int16)
        y = np.asarray(Image.open(b).convert("RGBA"), dtype=np.int16)
        dd = np.abs(x - y).max(axis=2)
        rows.append((int(f), dd.mean(), (dd > 32).mean(), x, y, dd))
    bad = [r for r in rows if r[1] > 2.0 or r[2] > 0.02]
    for f, mean, frac, *_ in rows:
        print(f"  frame {f:3d} mean={mean:6.3f} >32={100 * frac:5.2f}%{'  <-- FAIL' if mean > 2.0 or frac > 0.02 else ''}")
    for f, mean, frac, x, y, dd in sorted(rows, key=lambda r: -r[1])[:4]:
        img = np.concatenate([x, y, np.stack([np.clip(dd * 4, 0, 255)] * 3 + [np.full_like(dd, 255)], axis=2)], axis=1)
        Image.fromarray(img.astype(np.uint8)).resize((img.shape[1] // 2, img.shape[0] // 2)).save(d / f"cmp_{f:05d}.png")
    print(f"frames {len(rows)}, failing {len(bad)}")
    print("ORBIT ALL PASS" if rows and not bad else "ORBIT FAILURES")
    return 0 if rows and not bad else 1


if __name__ == "__main__":
    sys.exit(main())
