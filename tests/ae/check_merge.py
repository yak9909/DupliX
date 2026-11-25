"""Judges the merge optimisation runs (tests/ae/run_merge.ps1).

usage: check_merge.py <out dir> <mfr> [cases comma list]
Each case below has a check reading the plugin log (plugin_<comp>_<mfr>.log) and/or the rendered frames
(<comp>_<mfr>.tif). Prints one line per case and exits 1 if any fails.
"""
import os
import re
import sys

out_dir, mfr = sys.argv[1], sys.argv[2]
only = [c for c in (sys.argv[3] if len(sys.argv) > 3 else "").split(",") if c]


def log_of(comp):
    p = os.path.join(out_dir, f"plugin_{comp}_{mfr}.log")
    if not os.path.exists(p):
        return ""
    with open(p, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def scene_lines(text):
    m = re.search(r"merge: scene cam dist=([\d.]+) comp \S+ camPos=\(([-\d.]+),([-\d.]+),([-\d.]+)\) layers=(\d+) planes=(\d+) boxes=(\d+)", text)
    if not m:
        return None
    d = {"dist": float(m.group(1)), "pos": tuple(float(m.group(i)) for i in (2, 3, 4)),
         "layers": int(m.group(5)), "planes": int(m.group(6)), "boxes": int(m.group(7))}
    d["blocks"] = [int(b, 16) for b in re.findall(r"merge:   layer \S+ count=\d+ block=([0-9a-f]+)", text)]
    d["unbounded"] = [int(u) for u in re.findall(r"merge:   plane .* unbounded=(\d)", text)]
    e = re.search(r"merge: effect cam dist=([\d.]+) camPos=\(([-\d.]+),([-\d.]+),([-\d.]+)\)", text)
    d["effect"] = (float(e.group(1)), tuple(float(e.group(i)) for i in (2, 3, 4))) if e else None
    return d


def check_scene(name, planes, unbounded, block, boxes=0):
    text = log_of("mg_" + name)
    if "merge: read scene ok=1" not in text:
        return False, "no 'read scene ok=1' in the log"
    s = scene_lines(text)
    if not s:
        return False, "no scene line"
    msgs = []
    ok = s["layers"] == 1 and s["planes"] == planes and s["boxes"] == boxes
    msgs.append(f"layers={s['layers']} planes={s['planes']} boxes={s['boxes']}")
    if sorted(s["unbounded"]) != sorted(unbounded):
        ok = False
    msgs.append(f"unbounded={s['unbounded']}")
    if s["blocks"] != [block]:
        ok = False
    msgs.append(f"block={['%x' % b for b in s['blocks']]}")
    if s["effect"] is None:
        ok = False
        msgs.append("no effect camera line")
    else:
        dd = abs(s["effect"][0] - s["dist"])
        dp = max(abs(a - b) for a, b in zip(s["effect"][1], s["pos"]))
        if dd > 1e-3 or dp > 1e-3:
            ok = False
        msgs.append(f"camera vs AE's: dist {dd:.4f} pos {dp:.4f}")
    return ok, " ".join(msgs)


CHECKS = {
    "scene_basic": lambda: check_scene("scene_basic", 2, [0, 1], 0),
    "scene_light": lambda: check_scene("scene_light", 1, [0], 4),
    "scene_nocam": lambda: check_scene("scene_nocam", 1, [0], 0),
}

fails = 0
for name, fn in CHECKS.items():
    if only and name not in only:
        continue
    ok, msg = fn()
    print(f"  {name:<16} {'PASS' if ok else 'FAIL'} {msg}")
    fails += 0 if ok else 1
sys.exit(1 if fails else 0)
