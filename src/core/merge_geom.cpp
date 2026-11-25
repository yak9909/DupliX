#include "core/merge_geom.h"

#include <algorithm>

namespace rnd {

ScreenQuad ProjectRect(const CameraView& cam, const double toWorld[16], const double rect[4])
{
    ScreenQuad q;
    q.valid = false;
    q.zMin = 1e300, q.zMax = -1e300;
    double inv[16], toCam[16];
    if (!(cam.dist > 0) || !Invert4(cam.camToWorld, inv)) return q;
    Mul4(toWorld, inv, toCam);
    const double xs[4] = {rect[0], rect[2], rect[2], rect[0]}, ys[4] = {rect[1], rect[1], rect[3], rect[3]};
    for (int k = 0; k < 4; k++) {
        double c[3];
        for (int j = 0; j < 3; j++) c[j] = xs[k] * toCam[j] + ys[k] * toCam[4 + j] + toCam[12 + j];
        q.zMin = std::min(q.zMin, c[2]);
        q.zMax = std::max(q.zMax, c[2]);
        if (!(c[2] > 1)) return q;
        q.x[k] = c[0] * cam.dist / c[2] + cam.compW / 2;
        q.y[k] = c[1] * cam.dist / c[2] + cam.compH / 2;
    }
    q.valid = true;
    return q;
}

bool GroupPlaneM4(const CameraView& cam, const double layerToWorld[16], double d, const double texRect[4],
                  const double rect[4], double out[16])
{
    const double tw = texRect[2] - texRect[0], th = texRect[3] - texRect[1];
    if (!(tw > 0) || !(th > 0)) return false;
    double P[16];  // shows layer px (x, y) at comp px (x, y) at depth d
    if (!PlaneM4(cam, layerToWorld, d, P)) return false;
    // layer px of the texture rect -> comp px of the screen rect
    const double sx = (rect[2] - rect[0]) / tw, sy = (rect[3] - rect[1]) / th;
    const double A[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, 1, 0, rect[0] - texRect[0] * sx, rect[1] - texRect[1] * sy, 0, 1};
    Mul4(A, P, out);
    return true;
}

void GroupTextureM3(const double texRect[4], int worldW, int worldH, double out[9])
{
    const double sx = worldW > 0 ? (texRect[2] - texRect[0]) / worldW : 1, sy = worldH > 0 ? (texRect[3] - texRect[1]) / worldH : 1;
    const double m[9] = {sx, 0, 0, 0, sy, 0, texRect[0], texRect[1], 1};
    std::copy(m, m + 9, out);
}

void TextureLayerRect(const double m3[9], int w, int h, double out[4])
{
    double mnx = 1e300, mny = 1e300, mxx = -1e300, mxy = -1e300;
    const double xs[2] = {0, (double)w}, ys[2] = {0, (double)h};
    for (double x : xs)
        for (double y : ys) {
            const double hw = x * m3[2] + y * m3[5] + m3[8];
            const double lx = (x * m3[0] + y * m3[3] + m3[6]) / hw, ly = (x * m3[1] + y * m3[4] + m3[7]) / hw;
            mnx = std::min(mnx, lx), mxx = std::max(mxx, lx), mny = std::min(mny, ly), mxy = std::max(mxy, ly);
        }
    out[0] = mnx, out[1] = mny, out[2] = mxx, out[3] = mxy;
}

}  // namespace rnd
