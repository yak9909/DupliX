"""THROWAWAY SPIKE: checks for S4 (re-render inside one AE session), see scenes/make_s4.jsx.

Per step tag (from "== step <tag>" markers that the jsx writes into plugin.log):
  smart   = SmartRender calls of our effect during that step (0 = served from AE's cache)
  pre     = PreRender calls and their total time
Per "<case>_b": our overlay silhouette vs the _ref render (same rule as check_s1.py), and
whether ours changed at all from "<case>_a" (a stale cache gives an unchanged picture).
"""
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image

out_dir = Path(sys.argv[1])
EDGE_TOL = 2


def load(p):
    return np.array(Image.open(p).convert("RGBA")).astype(int)


def dilate(m, n):
    o = m.copy()
    for _ in range(n):
        p = o.copy()
        p[1:] |= o[:-1]; p[:-1] |= o[1:]; p[:, 1:] |= o[:, :-1]; p[:, :-1] |= o[:, 1:]
        o = p
    return o


def masks(tag):
    img = load(out_dir / f"{tag}_00000.tif")
    ref = load(out_dir / f"{tag}_ref_00000.tif")
    h, w = img.shape[:2]
    bg = ref[h - 20, w // 2, :3]
    mref = np.abs(ref[..., :3] - bg).max(axis=-1) > 8
    mours = (img[..., 0] > 200) & (img[..., 1] < 60) & (img[..., 2] > 200)
    return mours, mref


# --- per-step counts from plugin.log
steps = {}
order = []
cur = None
for line in (out_dir / "plugin.log").read_text(encoding="utf-8", errors="replace").splitlines():
    m = re.search(r"== step (\S+)", line)
    if m:
        cur = m.group(1)
        order.append(cur)
        steps[cur] = {"smart": 0, "pre": 0, "pre_ms": 0.0, "noguid": 0, "s7_ms": 0.0, "s7_targets": 0}
        continue
    m = re.search(r"S7 comp=.* targets=(\d+) .*targets\+mattes render ([\d.]+)ms", line)
    if m and cur is not None:
        steps[cur]["s7_targets"] = max(steps[cur]["s7_targets"], int(m.group(1)))
        steps[cur]["s7_ms"] += float(m.group(2))
    if cur is None:
        continue
    if "SmartRender: bpc" in line:
        steps[cur]["smart"] += 1
    m = re.search(r"S4 PreRender total ([\d.]+)ms", line)
    if m:
        steps[cur]["pre"] += 1
        steps[cur]["pre_ms"] += float(m.group(1))
    if "S4 NOGUID" in line:
        steps[cur]["noguid"] += 1

for tag in order:
    s = steps[tag]
    print(f"{tag:14s} smart={s['smart']} pre={s['pre']} pre_ms={s['pre_ms']:.1f}"
          + (f" s7_targets={s['s7_targets']} s7_layer_render_ms={s['s7_ms']:.1f}" if s["s7_targets"] else "")
          + (" (noguid)" if s["noguid"] else ""))

print()
if "s0_b" in steps:
    s0 = steps["s0_b"]["smart"]
    print(f"step0: second render of the unchanged comp -> smart={s0} "
          + ("(cache hit: OK, later steps are meaningful)" if s0 == 0 else "(NOT cached: later steps prove nothing)"))

# --- "before" pictures: a wrong one means a cache entry was shared with another comp
for tag in order:
    if not tag.endswith("_a") or tag.startswith("s0"):
        continue
    try:
        o, r = masks(tag)
    except FileNotFoundError:
        continue
    u = (o | r).sum()
    iou = (o & r).sum() / u if u else 0.0
    far = int((o & ~dilate(r, EDGE_TOL)).sum() + (r & ~dilate(o, EDGE_TOL)).sum())
    ok = iou > (0.80 if tag.startswith("many") else 0.97) and far == 0
    print(f"{tag}: {'OK' if ok else 'NG (wrong before-change picture)'} IoU={iou:.4f} far={far} "
          f"smart={steps[tag]['smart']}")
print()

# --- per-case picture checks
for tag in order:
    if not tag.endswith("_b") or tag == "s0_b":
        continue
    case = tag[:-2]
    try:
        ours_b, ref_b = masks(tag)
        ours_a, _ = masks(case + "_a")
    except FileNotFoundError as e:
        print(f"{case}: missing output ({e.filename})")
        continue
    changed = int((ours_a ^ ours_b).sum())
    if case in ("opacity0", "hide"):
        # AE draws nothing for the target any more; our overlay should disappear too if the
        # AEGP render includes the layer's opacity.
        ok = ref_b.sum() == 0 and ours_b.sum() == 0
        print(f"{case}: {'OK' if ok else 'NG'} ref_px={int(ref_b.sum())} ours_px={int(ours_b.sum())} "
              f"ours_changed_px={changed} smart={steps[tag]['smart']}")
        continue
    inter = (ref_b & ours_b).sum()
    union = (ref_b | ours_b).sum()
    iou = inter / union if union else 0.0
    far_ref = int((ref_b & ~dilate(ours_b, EDGE_TOL)).sum())
    far_ours = int((ours_b & ~dilate(ref_b, EDGE_TOL)).sum())
    # "many" is built on t_text (thin glyphs): same lower IoU bar as check_s1.py, edge rule unchanged
    ok = ours_b.sum() > 0 and far_ref == 0 and far_ours == 0 and iou > (0.80 if case == "many" else 0.97)
    print(f"{case}: {'OK' if ok else 'NG (stale?)'} IoU={iou:.4f} far ref={far_ref} ours={far_ours} "
          f"ours_changed_px={changed} smart={steps[tag]['smart']}")
