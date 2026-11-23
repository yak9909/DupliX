"""THROWAWAY SPIKE: create test footage for the S2 layer-type variants.

disc.png: 300x300, opaque orange disc (r=140) with a transparent square hole in the middle,
transparent outside. Asymmetric hole offset so a flip/shift would be visible.
"""
from pathlib import Path

import numpy as np
from PIL import Image

out = Path(__file__).resolve().parent.parent / "scenes" / "assets"
out.mkdir(parents=True, exist_ok=True)
n = 300
y, x = np.mgrid[0:n, 0:n] + 0.5
a = ((x - 150) ** 2 + (y - 150) ** 2 <= 140 ** 2).astype(np.uint8) * 255
a[110:170, 150:230] = 0  # hole, right of centre
img = np.zeros((n, n, 4), np.uint8)
img[..., 0] = 240; img[..., 1] = 140; img[..., 2] = 30; img[..., 3] = a
Image.fromarray(img, "RGBA").save(out / "disc.png")
print(out / "disc.png")
