"""THROWAWAY SPIKE: automatic checks for the S1/S2 renders.

For each variant there are two renders:
  <name>_00000.tif      effect enabled
  <name>_ref_00000.tif  effect + its layer disabled (= what AE itself draws)

S1: red marker at comp (100..300, 100..300) and green 8px border on the comp edge,
    whatever the effect layer's own transform is (+-1px for AE's resampling).
S2: our opaque-magenta overlay of the slot-1 layer (checked-out pixels + toComp() corners,
    perspective-correct) must match the silhouette AE draws for that layer (IoU).
Colour values are never compared, so the project's colour management does not matter.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

out_dir = Path(sys.argv[1])


def load(p):
    return np.array(Image.open(p).convert("RGBA")).astype(int)


EDGE_TOL = 2


def dilate(m, n):
    """4-neighbour dilation, n steps."""
    o = m.copy()
    for _ in range(n):
        p = o.copy()
        p[1:] |= o[:-1]; p[:-1] |= o[1:]; p[:, 1:] |= o[:, :-1]; p[:, :-1] |= o[:, 1:]
        o = p
    return o


# S5: the overlay now uses AE's own render of the target layer (AEGP), so masks, effects,
# text and shapes are graded.
# S5c: with the widened ROI a collapsed precomp also renders through AEGP, so it is graded
# too (and must come from "aegp", not the slot fallback).
INFO_ONLY = set()
# Thin shapes (text strokes): the 2px edge band is a large share of the area, so IoU is low
# even when every mismatch is on the edge. Grade them by the edge rule with a lower IoU bar.
THIN_IOU = {"t_text": 0.80}
# S4: target hidden (video switch off) / outside its in-out range: nothing may be drawn.
EXPECT_EMPTY = {"t_hidden", "t_outrange", "t_null", "t_guide", "t_adjust"}

# Which comps actually reached SmartRender, and from which pixel source (plugin.log).
import re
render_src = {}
cur = None
log_path = out_dir / "plugin.log"
if log_path.exists():
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.search(r"SmartRender: bpc=.* comp='([^']*)'", line)
        if m:
            cur = m.group(1)
            render_src.setdefault(cur, [])
            continue
        m = re.search(r"overlay source = (\w+)", line)
        if m and cur is not None:
            render_src[cur].append(m.group(1))
            cur = None

ok_all = True
for tif in sorted(out_dir.glob("*_00000.tif")):
    if "_ref_" in tif.name:
        continue
    name = tif.name.replace("_00000.tif", "")
    img = load(tif)
    ref = load(out_dir / f"{name}_ref_00000.tif")
    h, w = img.shape[:2]
    Image.fromarray(img.astype(np.uint8)).resize((w // 2, h // 2)).save(out_dir / f"{name}.preview.png")

    # --- S1
    red = (img[..., 0] > 200) & (img[..., 1] < 60) & (img[..., 2] < 60)
    ys, xs = np.nonzero(red)
    box = (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1) if len(xs) else None
    box_ok = box is not None and all(abs(a - b) <= 1 for a, b in zip(box, (100, 100, 300, 300)))
    green = (img[..., 1] > 200) & (img[..., 0] < 60) & (img[..., 2] < 60)
    border = np.zeros((h, w), bool)
    border[:8, :] = border[-8:, :] = border[:, :8] = border[:, -8:] = True
    inner = np.zeros((h, w), bool)
    inner[10:-10, 10:-10] = True
    border_cov = green[border].mean()
    s1 = box_ok and border_cov > 0.7 and green[inner].sum() == 0

    # --- S2
    bg = ref[h - 20, w // 2, :3]
    mask_ref = np.abs(ref[..., :3] - bg).max(axis=-1) > 8
    mask_ours = (img[..., 0] > 200) & (img[..., 1] < 60) & (img[..., 2] > 200)
    inter = (mask_ref & mask_ours).sum()
    union = (mask_ref | mask_ours).sum()
    iou = inter / union if union else 0.0
    # Edge pixels differ by AA and by AE resampling a scaled/rotated effect layer (up to ~2px
    # when the layer is scaled 150%). A real misplacement shows up as mismatches farther away,
    # so require every mismatch to lie within EDGE_TOL px of the other mask (both directions).
    far_ref = int((mask_ref & ~dilate(mask_ours, EDGE_TOL)).sum())
    far_ours = int((mask_ours & ~dilate(mask_ref, EDGE_TOL)).sum())
    s2 = mask_ours.sum() > 0 and far_ref == 0 and far_ours == 0 and iou > THIN_IOU.get(name, 0.97)
    if name in EXPECT_EMPTY:
        # AE does not draw the target at all: our overlay must be empty too
        s2 = mask_ref.sum() == 0 and mask_ours.sum() == 0
    info = name in INFO_ONLY
    srcs = render_src.get(name, [])
    rendered = len(srcs) > 0 and all(s == "aegp" for s in srcs)
    # S7: comps with zero targets and the same self transform share one cache entry (same
    # output), so only the first of them reaches SmartRender. That is correct, not stale.
    if name in EXPECT_EMPTY and not srcs:
        rendered = True
    if not info:
        ok_all &= s1 and s2 and rendered
    print(f"{'(info) ' if info else ''}{name}: render={'aegp' if rendered else (srcs or 'NONE(cache?)')} | "
          f"S1 {'OK' if s1 else 'NG'} red_box={box} border_green={border_cov:.3f} | "
          f"S2 {'OK' if s2 else 'NG'} IoU={iou:.4f} far(>{EDGE_TOL}px) ref={far_ref} ours={far_ours} "
          f"ref_px={int(mask_ref.sum())} ours_px={int(mask_ours.sum())} "
          f"only_ref={int((mask_ref & ~mask_ours).sum())} only_ours={int((mask_ours & ~mask_ref).sum())}")

print("ALL OK" if ok_all else "SOME NG")
