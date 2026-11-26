# A large opaque test image (like a photo: every pixel different) for run_preview.ps1's pv_bigS / pv_bigL variants.
#   python tests/ae/make_big_png.py build/test/big/big1920.png [1920 1080 [opaque share]]
# opaque share (e.g. 0.5): a cut-out, opaque inside a centred ellipse of that share of the image, clear outside (a
# hard edge, like a photo cut out of its background)
import math
import os
import struct
import sys
import zlib


def main():
    out = sys.argv[1]
    w = int(sys.argv[2]) if len(sys.argv) > 2 else 1920
    h = int(sys.argv[3]) if len(sys.argv) > 3 else 1080
    share = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0
    # ellipse area = pi/4 * w * h * s^2
    s = math.sqrt(share * 4 / math.pi) if share < 1 else None
    rows = bytearray()
    for y in range(h):
        rows.append(0)  # filter: none
        for x in range(w):
            r = int(127.5 + 127.5 * math.sin(x * 0.013 + y * 0.004))
            g = int(127.5 + 127.5 * math.sin(y * 0.017 - x * 0.006 + 1.0))
            b = (x * 7 + y * 13) & 255
            a = 255
            if s is not None:
                ex, ey = (x + 0.5 - w / 2) / (w / 2 * s), (y + 0.5 - h / 2) / (h / 2 * s)
                a = 255 if ex * ex + ey * ey <= 1 else 0
            rows += bytes((r, g, b, a))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(rows), 6)))
        f.write(chunk(b"IEND", b""))


if __name__ == "__main__":
    main()
