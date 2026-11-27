"""Checks tests/ae/count_keys.jsx: the keyed comp (in one session, and rendered by aerender with MFR on) against a
comp with that frame's count fixed. Usage: check_count_keys.py <dir>"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

FRAMES = [0, 5, 10, 20, 29, 30, 31, 35, 40]


def load(p: Path):
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.int16)


def main() -> int:
    d = Path(sys.argv[1])
    ok = True
    for o in (0, 1):
        for f in FRAMES:
            ref = d / f"ref_o{o}_{f}.png"
            if not ref.exists():
                print(f"o{o} f{f}: ref MISSING")
                ok = False
                continue
            r = load(ref)
            row = [f"o{o} f{f:2d}: lit {int((r.max(axis=2) > 120).sum()):5d}"]
            for kind, p in (("session", d / f"key_o{o}_{f}.png"), ("mfr", d / f"mfr_o{o}_{f:05d}.tif")):
                if not p.exists():
                    row.append(f"{kind} MISSING")
                    ok = False
                    continue
                diff = np.abs(load(p) - r).max(axis=2)
                n = int((diff > 0).sum())
                row.append(f"{kind} {'same' if n == 0 else f'DIFF {n} px max {int(diff.max())}'}")
                ok &= n == 0
            print("  ".join(row))
    print("COUNT KEYS PASS" if ok else "COUNT KEYS FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
