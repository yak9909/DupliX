// randamu core: CPU rasterizer of the fast mode. Draws perspective copies of one source image into one
// output image (bilinear, "over", per-copy opacity, far copies first). No AE dependency.
#pragma once

#include <functional>

namespace rnd {

// ARGB, straight (unpremultiplied) alpha like AE's effect worlds, the channel order of PF_Pixel8 /
// PF_Pixel16 / PF_PixelFloat. Filtering and "over" are done premultiplied internally.
// bpc 8: 0..255, 16: 0..32768, 32: float. Buffer pixel (i, j) is (downsampled) px (i + originX, j + originY).
struct RasterImage {
    void* data;
    int width, height;
    long rowBytes;
    int bpc;
    int originX, originY;
    // dst only: full-resolution screen px = full-resolution buffer px + shift (fast mode on a 2D layer:
    // the output is in layer px, screen = layer px - anchor + comp centre). src ignores it.
    double screenShiftX = 0, screenShiftY = 0;
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
// abort (optional) is polled on the calling thread between chunks of rows; when it returns true the drawing
// stops (dst is left incomplete) and the function returns false.
bool RasterizeCopies(const RasterImage& src, RasterCopy* copies, int n, double dsX, double dsY, RasterImage& dst,
                     int threads, const std::function<bool()>& abort = {});

// For the unit tests only: worker threads (not the calling thread) fail to get their scratch memory.
void RasterTestFailWorkerScratch(bool fail);

}  // namespace rnd
