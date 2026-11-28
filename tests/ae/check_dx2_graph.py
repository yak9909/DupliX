"""Checks tests/ae/dx2_graph.jsx (run by run_dx2_graph.ps1).

1. The elements the effect evaluated (DUPLIX2_DUMP, one block per PreRender: "t <s> layer <w> <h>") are the ones
   graph_test.exe eval gives for the same graph and macro values (the macros' keys are linear).
2. Pixels of the saved frames: the copies are where the graph puts them, turned and faded as it says.
Usage: check_dx2_graph.py <dir>"""
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
GRAPH_TEST = ROOT / "build" / "graph" / "graph_test.exe"
SCRIPT = Path(__file__).resolve().parent / "dx2_graph_script.txt"


def blocks(dump: Path):
    """(w, h, t) -> list of the element lines of each PreRender, in order"""
    out = {}
    key, cur = None, None
    for line in dump.read_text(encoding="utf-8").splitlines():
        if line.startswith("t "):
            p = line.split()
            key = (int(p[3]), int(p[4]), round(float(p[1]), 3))
            cur = []
            out.setdefault(key, []).append(cur)
        elif cur is not None:
            cur.append(line)
    return out


def graph_of(script: str, tmp: Path) -> Path:
    """the graph an editor script makes from a new instance's graph"""
    s = tmp / "script.txt"
    s.write_text(script, encoding="utf-8")
    g = tmp / f"graph_{abs(hash(script))}.txt"
    r = subprocess.run([str(GRAPH_TEST), "script", str(s), str(g)], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"graph_test script: {r.stderr}")
    return g


def expected(graph: Path, t: float, macros: dict) -> list:
    args = [str(GRAPH_TEST), "eval", str(graph), f"{t}"] + [f"m{k}={v}" for k, v in macros.items()]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"graph_test eval: {r.stderr}")
    return r.stdout.splitlines()


def px(d: Path, name: str, x: int, y: int):
    p = d / f"{name}.png"
    if not p.exists():
        return None
    return Image.open(p).convert("RGBA").getpixel((x, y))


def near(got, want, tol=3):
    return got is not None and all(abs(a - b) <= tol for a, b in zip(got, want))


def main() -> int:
    d = Path(sys.argv[1])
    ok = True

    def check(name, good, detail=""):
        nonlocal ok
        ok &= bool(good)
        print(f"{name}: {'ok' if good else 'WRONG'} {detail}")

    dump = d / "dump.txt"
    if not dump.exists():
        print("dump.txt: MISSING")
        return 1
    b = blocks(dump)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        default = graph_of("", tmp)
        circle = graph_of(SCRIPT.read_text(encoding="utf-8"), tmp)
        # A: Count 2 -> 4, Spacing 60 -> 100 over 0..2 s
        for t in (0, 1, 2):
            want = expected(default, t, {1: 2 + t, 2: 60 + 20 * t, 3: 100})
            got = b.get((40, 40, float(t)))
            check(f"A t={t} elements", got and got[-1] == want,
                  f"({want[0]}; got {got[-1][0] if got else 'no dump'})")
        # B before the keys: Macro 1 = 300 (the value Expose wrote with the graph)
        want = expected(circle, 0, {1: 300})
        got = b.get((30, 30, 0.0)) or []
        check("B before keys: some PreRender with Radius 300", any(g == want for g in got),
              f"({len(got)} blocks at t=0)")
        # B: Radius 50 -> 150 over 0..2 s
        for t in (0, 1, 2):
            want = expected(circle, t, {1: 50 + 50 * t})
            got = b.get((30, 30, float(t)))
            hit = got and any(g == want for g in got)
            check(f"B t={t} elements", hit, f"({want[0]})")

    # pixels (comp 400 x 300, the layers at its centre (200, 150); outside the copies: transparent)
    red, green_half, clear = (255, 0, 0, 255), (0, 255, 0, 128), (0, 0, 0, 0)
    # A t=0: 2 x 2 at spacing 60: copies centred at (170,120) (230,120) (170,180) (230,180), 40 px
    check("a_0 copy", near(px(d, "a_0", 170, 120), red), str(px(d, "a_0", 170, 120)))
    check("a_0 gap", near(px(d, "a_0", 200, 150), clear), str(px(d, "a_0", 200, 150)))
    # A t=2: 4 x 4 at spacing 100: x 50 150 250 350, y 0 100 200 300
    check("a_2 copy", near(px(d, "a_2", 150, 100), red), str(px(d, "a_2", 150, 100)))
    check("a_2 gap", near(px(d, "a_2", 200, 150), clear), str(px(d, "a_2", 200, 150)))
    # B t=0: 6 on a circle of 50, the first at the top (200, 100), turned 45 degrees, 50% opacity: a diamond
    # |x| + |y| <= 21.2 around it: (218, 100) is inside it (not inside the square unturned), (214, 86) is outside
    v, ref = px(d, "b_0", 200, 100), px(d, "ref_half", 200, 150)
    check("b_0 copy centre half opacity (as AE's 50% solid)", ref is not None and near(v, ref, 1), f"{v} want {ref}")
    v = px(d, "b_0", 218, 100)
    check("b_0 turned (a corner on the axis)", v is not None and v[3] > 60, str(v))
    check("b_0 turned (no square corner)", near(px(d, "b_0", 214, 86), clear), str(px(d, "b_0", 214, 86)))
    # B t=2: radius 150, the first at (200, 0); the 4th (180 degrees) at (200, 300): neither centre at (200, 150)
    check("b_2 centre clear", near(px(d, "b_2", 200, 150), clear), str(px(d, "b_2", 200, 150)))
    # after the undos: the default 5 x 5 grid (spacing 100, Count 5): a copy at the centre, opaque
    v = px(d, "b_undo", 200, 150)
    check("b_undo default grid", v is not None and v[1] > 250 and v[3] > 250, str(v))
    # the editor (plugin.log): opened on B, reloaded after AE's undo, closed when B's effect went away
    log = (d / "plugin.log").read_text(encoding="utf-8", errors="replace") if (d / "plugin.log").exists() else ""
    check("editor opened", "dx2: editor: opened on instance" in log)
    check("editor reloaded after the undo", "dx2: editor: the graph changed outside the editor" in log)
    check("editor closed with its instance", "dx2: editor: session closed" in log and ", gone)" in log)
    print("DX2 GRAPH PASS" if ok else "DX2 GRAPH FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
