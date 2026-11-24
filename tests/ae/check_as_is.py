"""Compares fast vs normal mode for tests/ae/as_is.jsx (aerender TIFFs and viewer-path PNGs).
usage: python check_as_is.py <out dir> <render dir>  -> writes pair_<what>.png (fast | normal | diff x4)."""
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def load(p: Path) -> np.ndarray:
    return np.asarray(Image.open(p).convert("RGBA"), dtype=np.int16)


def main() -> int:
    out, ren = Path(sys.argv[1]), Path(sys.argv[2])
    pairs = []
    fm = sorted(ren.glob("r_as_fm_*.tif"))
    nm = sorted(ren.glob("r_as_nm_*.tif"))
    if fm and nm:
        pairs.append(("aerender", fm[0], nm[0]))
    for res in ("full", "half"):
        a, b = out / f"ui_{res}_fm.png", out / f"ui_{res}_nm.png"
        if a.exists() and b.exists():
            pairs.append((f"viewer_{res}", a, b))
    bad = 0
    for what, a, b in pairs:
        x, y = load(a), load(b)
        d = np.abs(x - y).max(axis=2)
        ok = d.mean() <= 2.0 and (d > 32).mean() <= 0.02
        bad += 0 if ok else 1
        print(f"  {what:<12} {'PASS' if ok else 'FAIL'} fast vs normal mean={d.mean():.3f} >32={100 * (d > 32).mean():.2f}%")
        dd = np.clip(d * 4, 0, 255).astype(np.uint8)
        img = np.concatenate([x, y, np.stack([dd, dd, dd, np.full_like(dd, 255)], axis=2)], axis=1).astype(np.uint8)
        Image.fromarray(img).save(out / f"pair_{what}.png")
    print(f"compared {len(pairs)}; {'AS-IS ALL PASS' if pairs and not bad else 'AS-IS FAILURES'}")
    return 0 if pairs and not bad else 1


if __name__ == "__main__":
    sys.exit(main())
