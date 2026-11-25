"""Checks tests/ae/transfer.jsx (fast mode switched on by script: the layer's transform moves into the World
Transform). usage: python check_transfer.py <out dir>  -> exit 0 if every check passes.
Writes compare_<case>.png (before | after | difference x4)."""
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def load(p: Path) -> np.ndarray:
    return np.asarray(Image.open(p).convert("RGBA"), dtype=np.int16)


def diff(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    return np.abs(a - b).max(axis=2)


def stats(d: np.ndarray) -> str:
    return f"max={int(d.max())} mean={d.mean():.3f} >32={100.0 * (d > 32).mean():.2f}%"


def main() -> int:
    d = Path(sys.argv[1])
    fails = 0

    def report(name: str, ok: bool, what: str) -> None:
        nonlocal fails
        fails += 0 if ok else 1
        print(f"  {name:<22} {'PASS' if ok else 'FAIL'} {what}")

    states = {}
    for line in (d / "jsx.log").read_text(encoding="utf-8").splitlines():
        m = re.match(r"STATE (\S+) (.*)", line)
        if m:
            states[m.group(1)] = dict(kv.split("=", 1) for kv in m.group(2).split())

    def png(name: str) -> np.ndarray:
        return load(d / f"{name}.png")

    for case in ("toggle_plain", "toggle_parent", "toggle_anim"):
        a, b = png(f"{case}_before"), png(f"{case}_after")
        dd = diff(a, b)
        dm = np.clip(dd * 4, 0, 255).astype(np.uint8)
        Image.fromarray(np.concatenate([a, b, np.stack([dm, dm, dm, np.full_like(dm, 255)], 2)], 1).astype(np.uint8)
                        ).save(d / f"compare_{case}.png")
        drawn = int((dd >= 0).sum())
        # fast (own bilinear drawing) vs normal (AE's 3D): the same limits as run_fast_mode's fast_scatter
        report(case, dd.mean() <= 2.0 and (dd > 32).mean() <= 0.01 and drawn > 0, f"before vs after {stats(dd)}")
        s = states.get(f"{case}_after", {})
        # a 2D layer keeps 3 components in scripting (z unused)
        report(case + " layer", s.get("3d") == "false" and s.get("parent") == "none" and s.get("posKeys") == "0"
               and s.get("fast") == "1" and s.get("pos", "").startswith("200,150")
               and s.get("scale", "").startswith("100,100"), f"after: {s}")
    s = states.get("toggle_parent_after", {})
    report("toggle_parent world", abs(float(s.get("wrotY", "nan")) - 26) < 1e-3,
           f"the null's Y 26 deg moved into the World Transform: wrotY={s.get('wrotY')}")
    s = states.get("toggle_plain_back", {})
    report("toggle_plain back", s.get("3d") == "true" and s.get("fast") == "0", f"fast off: {s}")
    dd = diff(png("toggle_plain_before"), png("toggle_plain_back"))
    report("toggle_plain back img", dd.max() <= 1, f"normal mode again vs before {stats(dd)}")
    s, s0 = states.get("toggle_parent_undo", {}), states.get("toggle_parent_after", {})
    report("toggle_parent undo", s.get("3d") == "true" and s.get("parent") == "yes" and s.get("fast") == "0"
           and s.get("wposX") == "0", f"after one Undo: {s}")
    dd = diff(png("toggle_parent_before"), png("toggle_parent_undo"))
    report("toggle_parent undo img", dd.max() <= 1, f"after one Undo vs before {stats(dd)}")
    # refused cases: the layer is left exactly as it was (still 3D, parent/rotation kept), World untouched
    s = states.get("toggle_separated_after", {})
    report("toggle_separated", s.get("3d") == "true" and s.get("rotY") == "20" and s.get("wposX") == "0"
           and s.get("fast") == "1", f"refused, layer unchanged: {s}")
    s = states.get("toggle_world_keys_after", {})
    k = states.get("toggle_world_keys_keys", {})
    report("toggle_world_keys", s.get("3d") == "true" and s.get("rotY") == "26" and k.get("n") == "2"
           and k.get("k2") == "40", f"refused, layer and keys unchanged: {s} {k}")
    s = states.get("toggle_initial_keys_after", {})
    k = states.get("toggle_initial_keys_keys", {})
    report("toggle_initial_keys", s.get("3d") == "false" and k.get("n") == "2" and k.get("k2") == "40",
           f"layer to 2D, World keys untouched: {s} {k}")
    if (d / "toggle_initial_keys_before.png").exists() and (d / "toggle_initial_keys_after.png").exists():
        dd = diff(png("toggle_initial_keys_before"), png("toggle_initial_keys_after"))
        report("toggle_initial_keys img", dd.mean() <= 2.0 and (dd > 32).mean() <= 0.01, f"before vs after {stats(dd)}")
    else:
        report("toggle_initial_keys img", False, "png missing")
    print("TRANSFER ALL PASS" if fails == 0 else f"TRANSFER FAILURES: {fails}")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
