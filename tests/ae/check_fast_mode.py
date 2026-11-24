"""Checks tests/ae/fast_mode.jsx renders (aerender TIFFs r_fm_<case>_NNNNN.tif / r_nm_<case>_NNNNN.tif).
usage: python check_fast_mode.py <out dir> <label> [case,case,...]
Writes compare_<case>_<label>.png (fast | normal | difference x4). Exit 0 if every check passes."""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

W, H = 400, 300


def load(p: Path) -> np.ndarray:
    return np.asarray(Image.open(p).convert("RGBA"), dtype=np.int16)


def diff(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    return np.abs(a - b).max(axis=2)


def box3(x: np.ndarray) -> np.ndarray:
    p = np.pad(x.astype(np.float32), ((1, 1), (1, 1), (0, 0)), mode="edge")
    h, w = x.shape[:2]
    return sum(p[i:i + h, j:j + w] for i in range(3) for j in range(3)) / 9.0


def stats(d: np.ndarray) -> str:
    return f"max={int(d.max())} mean={d.mean():.3f} >32={100.0 * (d > 32).mean():.2f}%"


def compare_png(a: np.ndarray, b: np.ndarray, out: Path) -> None:
    d = np.clip(diff(a, b) * 4, 0, 255).astype(np.uint8)
    dd = np.stack([d, d, d, np.full_like(d, 255)], axis=2)
    Image.fromarray(np.concatenate([a, b, dd], axis=1).astype(np.uint8)).save(out)


def frame(d: Path, comp: str, f: int) -> Path:
    return d / f"r_{comp}_{f:05d}.tif"


def main() -> int:
    d = Path(sys.argv[1])
    label = sys.argv[2] if len(sys.argv) > 2 else "x"
    only = sys.argv[3].split(",") if len(sys.argv) > 3 and sys.argv[3] else None
    fails = 0

    def report(name: str, ok: bool, what: str) -> None:
        nonlocal fails
        fails += 0 if ok else 1
        print(f"  {name:<14} {'PASS' if ok else 'FAIL'} {what}")

    def near(name: str, a: np.ndarray, b: np.ndarray, max_mean: float, max_frac32: float, tag: str = "") -> bool:
        dd = diff(a, b)
        ok = dd.mean() <= max_mean and (dd > 32).mean() <= max_frac32
        report(name, ok, f"{tag}fast vs normal {stats(dd)}")
        return ok

    cases = ["fast_identity", "fast_scatter", "fast_anim", "fast_half", "fast_nocam", "fast_count0", "fast_toggle",
             "fast_grow", "fast_remove"]
    for c in cases:
        if only and c not in only:
            continue
        if c == "fast_remove":  # PNGs from the AE session only
            on, off, plain = d / "rm_on.png", d / "rm_off.png", d / "rm_plain.png"
            if not (on.exists() and off.exists() and plain.exists()):
                report(c, False, "rm_*.png missing")
                continue
            doff = diff(load(off), load(plain))
            report(c, doff.max() <= 1, f"removed in the session vs never had it {stats(doff)}")
            don = diff(load(on), load(plain))
            report(c + " on", don.max() > 32, f"while on, the copies were shown (vs plain {stats(don)})")
            continue
        fm, nm = frame(d, "fm_" + c, 0), frame(d, "nm_" + c, 0)
        if not fm.exists() or not nm.exists():
            report(c, False, f"MISSING {fm.name if not fm.exists() else nm.name}")
            continue
        a, b = load(fm), load(nm)
        compare_png(a, b, d / f"compare_{c}_{label}.png")
        if c == "fast_identity":
            # Placement: AE's resampling differs from bilinear on fine detail (raw max ~60 on a checker edge),
            # so compare 3x3-averaged images: right placement ~16, a 1 px shift >= 72.
            dd, db = diff(a, b), np.abs(box3(a) - box3(b)).max(axis=2)
            report(c, dd.mean() <= 0.5 and db.max() <= 24,
                   f"fast vs normal {stats(dd)}; 3x3-averaged max={db.max():.1f} (mean<=0.5, averaged max<=24)")
        elif c in ("fast_scatter", "fast_nocam", "fast_grow"):
            near(c, a, b, 2.0, 0.01)
        elif c == "fast_half":
            size_ok = a.shape[:2] == (H // 2, W // 2) and b.shape[:2] == (H // 2, W // 2)
            report(c + " size", size_ok, f"fast {a.shape[1]}x{a.shape[0]} normal {b.shape[1]}x{b.shape[0]} (want half)")
            near(c, a, b, 2.0, 0.01)
        elif c == "fast_count0":
            dd = diff(a, b)
            report(c, dd.max() <= 1, f"fast vs normal (no copies) {stats(dd)}")
        elif c == "fast_toggle":
            near(c, a, b, 2.0, 0.01, "rendered with fast on: ")
            on, off, nmp = d / "tog_on.png", d / "tog_off.png", d / "tog_nm.png"
            if on.exists() and off.exists() and nmp.exists():
                doff = diff(load(off), load(nmp))
                report(c + " off", doff.max() <= 1, f"switched off in the session vs normal {stats(doff)}")
                near(c + " on", load(on), load(nmp), 2.0, 0.01, "in the session: ")
            else:
                report(c + " pngs", False, "tog_*.png missing")
        elif c == "fast_anim":
            worst_mean, worst_frac, bad = 0.0, 0.0, []
            for f in range(30):
                pa, pb = frame(d, "fm_" + c, f), frame(d, "nm_" + c, f)
                if not pa.exists() or not pb.exists():
                    bad.append(f"{f}:missing")
                    continue
                dd = diff(load(pa), load(pb))
                worst_mean, worst_frac = max(worst_mean, dd.mean()), max(worst_frac, (dd > 32).mean())
                if dd.mean() > 2.0 or (dd > 32).mean() > 0.01:
                    bad.append(f"{f}:mean={dd.mean():.2f},>32={100 * (dd > 32).mean():.2f}%")
            report(c, not bad, f"30 frames fast vs normal: worst mean={worst_mean:.3f} >32={100 * worst_frac:.2f}% "
                               f"{' '.join(bad[:6])}")
            for s in sorted(d.glob("single_*.png")):
                f = int(s.stem.split("_")[1])
                pa = frame(d, "fm_" + c, f)
                if not pa.exists():
                    report(f"{c} f{f}", False, "missing sequence frame")
                    continue
                dd = diff(load(pa), load(s))
                report(f"{c} f{f}", dd.max() <= 1, f"sequence vs single render {stats(dd)}")
    print("FAST ALL PASS" if fails == 0 else f"FAST FAILURES: {fails}")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
