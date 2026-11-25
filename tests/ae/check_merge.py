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
# 16 / 32 bpc passes (run_merge.ps1 C16, G32, ...): AE dithers them into the 8-bit TIFF, differently in every render
# (two renders of the same comp differ by 1 level at most pixels), so "unchanged" means within 1 level there
DITHER = 1 if re.search(r"(16|32)$", mfr) else 0


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


def load(comp):
    import numpy as np
    from PIL import Image
    p = os.path.join(out_dir, f"{comp}_{mfr}.tif")
    if not os.path.exists(p):
        return None
    return np.asarray(Image.open(p).convert("RGBA")).astype(int)


def box5(img):
    """5x5 average (edges replicated)."""
    import numpy as np
    p = np.pad(img.astype(float), ((2, 2), (2, 2), (0, 0)), mode="edge")
    s = np.zeros(img.shape, float)
    for dy in range(5):
        for dx in range(5):
            s += p[dy:dy + img.shape[0], dx:dx + img.shape[1]]
    return s / 25


def compare(name, min_groups=1, mean_max=1.0, avg_frac_max=0.001, avg_mean_max=0.5, over2_max=None, over32_max=None):
    """merge on vs off. The merged copies are drawn by DupliX's bilinear filter, not AE's: their texture's sharp edges
    differ pixel by pixel (like the fast mode; magnified copies keep a grid of differences along the texels' edges).
    A wrong front/back order differs over whole areas, which a 5x5 average keeps: judged on the share of pixels whose
    5x5 averages differ by more than 16, the averages' mean and the raw mean."""
    import numpy as np
    a, b = load("mg_" + name), load("nm_" + name)
    if a is None or b is None:
        return False, "missing output"
    if a.shape != b.shape:
        return False, f"size {a.shape} vs {b.shape}"
    d = np.abs(a - b).max(axis=2)
    mean, f2, f32 = float(np.abs(a - b).mean()), float((d > 2).mean()), float((d > 32).mean())
    avg = np.abs(box5(a) - box5(b))
    amax, amean, afrac = float(avg.max()), float(avg.mean()), float((avg.max(axis=2) > 16).mean())
    text, off = log_of("mg_" + name), log_of("nm_" + name)
    groups = [int(g) for g in re.findall(r"merge:   group copies=(\d+)", text)]
    swaps = len(re.findall(r"merge: texture swapped", text))
    fails = len(re.findall(r"merge: texture failed", text)) + len(re.findall(r"merge: texture failed", off))
    off_swaps = len(re.findall(r"merge: texture swapped", off))
    ok = (len(groups) >= min_groups and all(g >= 2 for g in groups) and swaps > 0 and fails == 0 and off_swaps == 0 and
          mean <= mean_max and afrac <= avg_frac_max and amean <= avg_mean_max and
          (over2_max is None or f2 <= over2_max) and (over32_max is None or f32 <= over32_max))
    return ok, (f"max={int(d.max())} mean={mean:.3f} >2={100 * f2:.2f}% >32={100 * f32:.3f}% avg5 max={amax:.1f} "
                f">16={100 * afrac:.3f}% mean={amean:.3f} groups={groups} swaps={swaps} failed={fails} off-swaps={off_swaps} "
                f"size={a.shape[1]}x{a.shape[0]}")


def same(name, block):
    """merge on but the layer must not merge (a reason in the log): the frame equals merge off."""
    import numpy as np
    a, b = load("mg_" + name), load("nm_" + name)
    if a is None or b is None:
        return False, "missing output"
    text = log_of("mg_" + name)
    blocks = [int(x, 16) for x in re.findall(r"merge:   layer \S+ count=\d+ block=([0-9a-f]+)", text)]
    d = int(np.abs(a - b).max())
    return d <= DITHER and blocks and all(x & block for x in blocks), f"max={d} blocks={['%x' % x for x in blocks]}"


def seam(name):
    """The group plane crosses the wall only where the group texture is transparent: the wall's pixels (the band
    between the copies) must be unchanged (0), or a seam appears."""
    import numpy as np
    # the copies are small (40%): DupliX's bilinear without prefiltering differs inside them more than usual
    ok, msg = compare(name, over32_max=0.006)
    a, b = load("mg_" + name), load("nm_" + name)
    if a is None or b is None:
        return False, msg
    wall = (b[:, :, 2] > 150) & (b[:, :, 0] < 60) & (b[:, :, 1] < 60)  # the pure blue wall (merge off)
    wall[:, :100] = False  # only between the copies (their checker has blue too)
    wall[:, 250:] = False
    near = wall.copy()
    for dy in (-2, -1, 0, 1, 2):
        for dx in (-2, -1, 0, 1, 2):
            near |= np.roll(np.roll(wall, dy, 0), dx, 1)
    band = int(np.abs(a - b).max(axis=2)[near].max()) if near.any() else -1
    forced = "test: group depth forced" in log_of("mg_" + name)
    return ok and forced and wall.sum() > 500 and band <= DITHER, f"{msg} wall px={int(wall.sum())} wall max={band} forced={forced}"


CHECKS = {
    "seam": lambda: seam("seam"),
    "wall_front_back": lambda: compare("wall_front_back"),
    "wall_tilted": lambda: compare("wall_tilted"),
    "wall_side": lambda: compare("wall_side"),
    "two_layers": lambda: compare("two_layers"),
    "solid_target": lambda: same("solid_target", 0x200),
    "scene_basic": lambda: check_scene("scene_basic", 2, [0, 1], 0),
    "scene_light": lambda: check_scene("scene_light", 1, [0], 4),
    "scene_nocam": lambda: check_scene("scene_nocam", 1, [0], 0),
    "group_pair": lambda: compare("group_pair"),
    "group_scatter": lambda: compare("group_scatter"),
    # half resolution: AE filters its half-size texture differently from DupliX's bilinear (as in the fast mode)
    "group_half": lambda: compare("group_half", over2_max=0.05),
    "group_nocam": lambda: compare("group_nocam"),
    # odd sizes / custom resolution factors (the draft quality of all-group layers must not shift the plane)
    "group_odd": lambda: compare("group_odd"),
    # 1/5: the copies are minified 5x or more; AE filters them, DupliX's bilinear does not (>2 up to 8%; the 5x5
    # averages stay equal: no shift and no order difference)
    "group_res5": lambda: compare("group_res5", over2_max=0.08),
    "group_odd_res3": lambda: compare("group_odd_res3", over2_max=0.05),
}

fails = 0
for name, fn in CHECKS.items():
    if only and name not in only:
        continue
    ok, msg = fn()
    print(f"  {name:<16} {'PASS' if ok else 'FAIL'} {msg}")
    fails += 0 if ok else 1
sys.exit(1 if fails else 0)
