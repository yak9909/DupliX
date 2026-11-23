"""THROWAWAY SPIKE: put the correct and the A' (cut-out) renders side by side.

Row 1: correct (AE composites the occluder X over the copy)
Row 2: A' (copy on top with X's alpha cut out)
Row 3: |difference| x4 (black = identical)
Also prints the mean / max difference (0-255) inside each column.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

d = Path(sys.argv[1])
a = np.array(Image.open(d / "correct_00000.tif").convert("RGB")).astype(int)
b = np.array(Image.open(d / "approx_00000.tif").convert("RGB")).astype(int)
diff = np.abs(a - b)
h, w = a.shape[:2]
labels = ["100%", "75%", "50%", "25%", "feather", "Add", "Multiply"]
colw = 260
for i, lab in enumerate(labels):
    region = diff[80:, i * colw:(i + 1) * colw]
    print(f"{lab:9s} mean={region.mean():6.2f} max={region.max():4d} "
          f"px>8={int((region.max(axis=-1) > 8).sum())}")

try:
    font = ImageFont.truetype("C:/Windows/Fonts/meiryo.ttc", 28)
except OSError:
    font = ImageFont.load_default()
pad = 50
out = Image.new("RGB", (w, (h + pad) * 3), (20, 20, 20))
rows = [("正しい見た目（AE がそのまま描いた絵）", a),
        ("くり抜き方式の見た目", b),
        ("差（4 倍に強調。黒 = 同じ）", np.clip(diff * 4, 0, 255))]
dr = ImageDraw.Draw(out)
for k, (title, img) in enumerate(rows):
    y = k * (h + pad)
    dr.text((12, y + 8), title, fill=(255, 255, 255), font=font)
    out.paste(Image.fromarray(img.astype(np.uint8)), (0, y + pad))
out.save(d / "approx_compare.png")
print("saved", d / "approx_compare.png")
