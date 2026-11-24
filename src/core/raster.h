// randamu core: CPU rasterizer of the fast mode. Draws perspective copies of one source image into one
// output image (bilinear, premultiplied "over", per-copy opacity, far copies first). No AE dependency.
#pragma once

namespace rnd {

// ARGB, premultiplied, the channel order of PF_Pixel8 / PF_Pixel16 / PF_PixelFloat.
// bpc 8: 0..255, 16: 0..32768, 32: float. Buffer pixel (i, j) is (downsampled) px (i + originX, j + originY).
struct RasterImage {
    void* data;
    int width, height;
    long rowBytes;
    int bpc;
    int originX, originY;
};

// screenToSrc: full-resolution screen px -> full-resolution source layer px (CopyHomography).
// opacity 0..1; depth: camera-space z (larger = farther, drawn first).
struct RasterCopy {
    double screenToSrc[9];
    double opacity;
    double depth;
};

// Clears dst to transparent and draws the copies far to near (sorts `copies` in place).
// dsX/dsY: the downsample factor of both src and dst (1 = full, 0.5 = half): buffer px p of either image
// covers full-resolution px [(p + origin) / ds, (p + origin + 1) / ds). Pixel centres are sampled.
// Source px outside src are transparent. `threads` bands of dst rows are drawn in parallel.
void RasterizeCopies(const RasterImage& src, RasterCopy* copies, int n, double dsX, double dsY, RasterImage& dst,
                     int threads);

}  // namespace rnd
