#include "core/merge_geom.h"

#include <algorithm>
#include <cmath>

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

bool QuadsOverlap(const ScreenQuad& a, const ScreenQuad& b, double marginPx)
{
    // separating axis test of two convex quads grown by marginPx each
    const ScreenQuad* q[2] = {&a, &b};
    for (int s = 0; s < 2; s++)
        for (int e = 0; e < 4; e++) {
            const ScreenQuad& p = *q[s];
            double nx = -(p.y[(e + 1) % 4] - p.y[e]), ny = p.x[(e + 1) % 4] - p.x[e];
            const double len = std::sqrt(nx * nx + ny * ny);
            if (!(len > 1e-12)) continue;
            nx /= len, ny /= len;
            double mn[2] = {1e300, 1e300}, mx[2] = {-1e300, -1e300};
            for (int t = 0; t < 2; t++)
                for (int k = 0; k < 4; k++) {
                    const double v = q[t]->x[k] * nx + q[t]->y[k] * ny;
                    mn[t] = std::min(mn[t], v), mx[t] = std::max(mx[t], v);
                }
            if (mn[1] > mx[0] + 2 * marginPx || mn[0] > mx[1] + 2 * marginPx) return false;
        }
    // degenerate (edge-on) quads: their bounding boxes decide
    double ab[4] = {1e300, 1e300, -1e300, -1e300}, bb[4] = {1e300, 1e300, -1e300, -1e300};
    for (int k = 0; k < 4; k++) {
        ab[0] = std::min(ab[0], a.x[k]), ab[1] = std::min(ab[1], a.y[k]), ab[2] = std::max(ab[2], a.x[k]), ab[3] = std::max(ab[3], a.y[k]);
        bb[0] = std::min(bb[0], b.x[k]), bb[1] = std::min(bb[1], b.y[k]), bb[2] = std::max(bb[2], b.x[k]), bb[3] = std::max(bb[3], b.y[k]);
    }
    return !(bb[0] > ab[2] + 2 * marginPx || ab[0] > bb[2] + 2 * marginPx || bb[1] > ab[3] + 2 * marginPx ||
             ab[1] > bb[3] + 2 * marginPx);
}

void RectCorners(const double toWorld[16], const double rect[4], double out[4][3])
{
    const double xs[4] = {rect[0], rect[2], rect[2], rect[0]}, ys[4] = {rect[1], rect[1], rect[3], rect[3]};
    for (int k = 0; k < 4; k++)
        for (int j = 0; j < 3; j++) out[k][j] = xs[k] * toWorld[j] + ys[k] * toWorld[4 + j] + toWorld[12 + j];
}

namespace {
void PlaneOf(const double m[16], double n[3], double p0[3])
{
    n[0] = m[1] * m[6] - m[2] * m[5];
    n[1] = m[2] * m[4] - m[0] * m[6];
    n[2] = m[0] * m[5] - m[1] * m[4];
    p0[0] = m[12], p0[1] = m[13], p0[2] = m[14];
}
double Dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
}  // namespace

int SideOfPlane(const double planeToWorld[16], const double camPos[3], const double pts[4][3])
{
    double n[3], p0[3];
    PlaneOf(planeToWorld, n, p0);
    const double nl = std::sqrt(Dot3(n, n));
    if (!(nl > 0)) return 0;
    const double c[3] = {camPos[0] - p0[0], camPos[1] - p0[1], camPos[2] - p0[2]};
    const double cs = Dot3(n, c) / nl;
    if (std::fabs(cs) < 1e-6) return 0;
    int same = 0, other = 0;
    for (int k = 0; k < 4; k++) {
        const double d[3] = {pts[k][0] - p0[0], pts[k][1] - p0[1], pts[k][2] - p0[2]};
        const double s = Dot3(n, d) / nl;
        if (std::fabs(s) < 1e-6 * (1 + std::sqrt(Dot3(d, d)))) return 0;  // touches the plane
        ((s > 0) == (cs > 0) ? same : other)++;
    }
    return same == 4 ? 1 : other == 4 ? -1 : 0;
}

bool PlaneDepthAt(const CameraView& cam, const double planeToWorld[16], double sx, double sy, double* z)
{
    double n[3], p0[3];
    PlaneOf(planeToWorld, n, p0);
    const double* C = cam.camToWorld;
    const double x = (sx - cam.compW / 2) / cam.dist, y = (sy - cam.compH / 2) / cam.dist;
    const double dir[3] = {x * C[0] + y * C[4] + C[8], x * C[1] + y * C[5] + C[9], x * C[2] + y * C[6] + C[10]};
    const double nd = Dot3(n, dir);
    if (std::fabs(nd) < 1e-12) return false;
    const double w[3] = {p0[0] - C[12], p0[1] - C[13], p0[2] - C[14]};
    const double t = Dot3(n, w) / nd;  // camera z: dir has camera-space z 1
    if (!(t > 0)) return false;
    *z = t;
    return true;
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
