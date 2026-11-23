"""Checks the smoke run: many.png has clearly more covered pixels than one.png, and the hook keyed
the layer with the same comp/layer IDs the script reported.
usage: python check_smoke.py <out dir>   (exit 0 = pass)"""
import re
import sys
from pathlib import Path

from PIL import Image


def covered(path: Path) -> int:
    im = Image.open(path).convert("RGBA")
    return sum(im.getchannel("A").histogram()[9:])


def main() -> int:
    out = Path(sys.argv[1])
    one, many = covered(out / "one.png"), covered(out / "many.png")
    print(f"covered one={one} many={many}")
    ok = one > 0 and many > 3 * one
    jsx = (out / "jsx.log").read_text(encoding="utf-8")
    m = re.search(r"TARGET_ID=(\d+) COMP_ID=(\d+)", jsx)
    plugin = (out / "plugin.log").read_text(encoding="utf-8", errors="replace") if (out / "plugin.log").exists() else ""
    if m:
        want = f"comp {m.group(2)} layer {m.group(1)}"
        keyed = want in plugin
        print(f"hook key '{want}' in plugin.log: {keyed}")
        ok = ok and keyed
    print("SMOKE CHECK", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
