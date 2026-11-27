"""Checks tests/ae/v2_sources.jsx: the colour at each grid cell's centre and that the space between cells is empty.
usage: python check_v2_sources.py <out dir>   (exit 0 = pass)"""
import sys
from pathlib import Path

from PIL import Image

CELLS = [(100, 150), (200, 150), (300, 150)]
GAPS = [(150, 150), (250, 150), (200, 60)]  # between the copies, and above them (outside every cell)
RED, GREEN, BLUE, GREY = (255, 0, 0), (0, 255, 0), (0, 0, 255), (128, 128, 128)
CASES = {
    "self": [GREY, GREY, GREY],
    "seq": [RED, GREEN, BLUE],
    "bya_rev": [BLUE, GREEN, RED],
    "gap": [RED, BLUE, RED],
}


def near(a, b, tol=3):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def main() -> int:
    out = Path(sys.argv[1])
    ok = True
    # saveFrameToPng writes 32 bpc projects darker (a red solid without any effect comes out as 25, 0, 0):
    # the expected colours are scaled by what it wrote for ref_red.png
    scale = 1.0
    ref = out / "ref_red.png"
    if ref.exists():
        scale = Image.open(ref).convert("RGBA").getpixel((200, 150))[0] / 255.0
        print(f"  ref_red    scale {scale:.3f}")
    cases = dict(CASES)
    for p in out.glob("seq_*.png"):
        cases[p.stem] = CASES["seq"]
    for name, want in cases.items():
        path = out / f"{name}.png"
        if not path.exists():
            print(f"  {name:10} MISSING")
            ok = False
            continue
        im = Image.open(path).convert("RGBA")
        got = [im.getpixel(c) for c in CELLS]
        gaps = [im.getpixel(g) for g in GAPS]
        want = [tuple(round(v * scale) for v in w) for w in want]
        cells_ok = all(near(g[:3], w) and g[3] >= 250 for g, w in zip(got, want))
        gaps_ok = all(g[3] <= 2 or near(g[:3], (0, 0, 0)) for g in gaps)
        print(f"  {name:10} cells {[g[:3] for g in got]} {'PASS' if cells_ok else 'FAIL'}, "
              f"gaps {'empty' if gaps_ok else gaps} {'PASS' if gaps_ok else 'FAIL'}")
        ok = ok and cells_ok and gaps_ok
    # the copies' size does not follow the Grid Size: the red copy is as wide in grid_big.png as in seq.png
    widths = {}
    for name, cx in (("seq", 100), ("grid_big", 80)):
        path = out / f"{name}.png"
        if not path.exists():
            ok = False
            continue
        im = Image.open(path).convert("RGBA")
        def red(p):  # the red copy only (not its green neighbour)
            return p[3] >= 128 and p[0] > 4 and p[0] > 4 * max(p[1], p[2])
        widths[name] = sum(1 for x in range(cx - 60, cx + 61) if red(im.getpixel((x, 150))))
    same = len(widths) == 2 and abs(widths["seq"] - widths["grid_big"]) <= 1 and abs(widths["seq"] - 90) <= 2
    print(f"  copy width seq {widths.get('seq')} grid_big {widths.get('grid_big')} (want 90) {'PASS' if same else 'FAIL'}")
    ok = ok and same
    print("V2 SOURCES", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
