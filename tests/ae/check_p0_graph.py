"""Checks tests/ae/p0_graph.jsx: the color of each saved frame (the graph data's color).
Usage: check_p0_graph.py <dir>"""
import sys
from pathlib import Path

from PIL import Image

EXPECT = [("before", (128, 128, 128)), ("written", (0, 255, 0)), ("undo1", (255, 0, 0)), ("undo2", (128, 128, 128)),
          ("redo", (255, 0, 0))]


def main() -> int:
    d = Path(sys.argv[1])
    ok = True
    for name, want in EXPECT:
        p = d / f"{name}.png"
        if not p.exists():
            print(f"{name}: MISSING")
            ok = False
            continue
        got = Image.open(p).convert("RGB").getpixel((32, 32))
        good = all(abs(a - b) <= 1 for a, b in zip(got, want))
        ok &= good
        print(f"{name}: {got} want {want} {'ok' if good else 'WRONG'}")
    print("P0 GRAPH PASS" if ok else "P0 GRAPH FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
