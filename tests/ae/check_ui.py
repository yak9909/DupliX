"""Checks the PNGs of tests/ae/ui_session.jsx.
usage: python check_ui.py <out dir>  -> prints one line per check; exit 0 if all pass."""
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def load(d: Path, name: str) -> np.ndarray:
    return np.asarray(Image.open(d / f"{name}.png").convert("RGBA"), dtype=np.int16)


def maxdiff(a: np.ndarray, b: np.ndarray) -> int:
    return int(np.abs(a - b).max())


def main() -> int:
    d = Path(sys.argv[1])
    fails = 0

    def check(name: str, cond: bool, info: str) -> None:
        nonlocal fails
        print(f"  {name:<28} {'PASS' if cond else 'FAIL'} {info}")
        if not cond:
            fails += 1

    scrub = [load(d, f"scrub_{i:02d}") for i in range(30)]
    changed = sum(1 for i in range(1, 30) if maxdiff(scrub[i - 1], scrub[i]) > 8)
    check("scrub frames change", changed >= 25, f"{changed}/29 consecutive pairs differ")

    a, b, c, dd, e, f, g = (load(d, n) for n in
                            ("a_base", "b_count20", "c_seed7", "d_rot60", "e_undo", "f_fxoff", "g_fxon"))
    check("count 10 -> 20 changes", maxdiff(a, b) > 8, f"max={maxdiff(a, b)}")
    check("seed 1 -> 7 changes", maxdiff(b, c) > 8, f"max={maxdiff(b, c)}")
    check("rotation 0 -> 60 changes", maxdiff(c, dd) > 8, f"max={maxdiff(c, dd)}")
    check("undo restores seed-7 frame", maxdiff(c, e) <= 1, f"max={maxdiff(c, e)}")

    fref = load(d, "f_ref")  # the target duplicated without the effect
    check("fx off == plain layer", maxdiff(f, fref) <= 1, f"max={maxdiff(f, fref)}")
    check("fx off differs from fx on", maxdiff(f, e) > 8, f"max={maxdiff(f, e)}")
    check("fx on restores frame", maxdiff(e, g) <= 1, f"max={maxdiff(e, g)}")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
