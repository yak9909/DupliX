#include "core/fast_plane.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace rnd {

bool Invert4(const double a[16], double out[16])
{
    double m[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) m[r][c] = c < 4 ? a[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (std::fabs(m[r][c]) > std::fabs(m[p][c])) p = r;
        if (std::fabs(m[p][c]) < 1e-12) return false;
        for (int k = 0; k < 8; k++) std::swap(m[c][k], m[p][k]);
        double d = m[c][c];
        for (int k = 0; k < 8; k++) m[c][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c) {
                double f = m[r][c];
                for (int k = 0; k < 8; k++) m[r][k] -= f * m[c][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[r * 4 + c] = m[r][c + 4];
    return true;
}

void Mul4(const double a[16], const double b[16], double out[16])
{
    double o[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
            o[r * 4 + c] = s;
        }
    for (int k = 0; k < 16; k++) out[k] = o[k];
}

bool Invert3(const double a[9], double out[9])
{
    double c00 = a[4] * a[8] - a[5] * a[7], c01 = a[5] * a[6] - a[3] * a[8], c02 = a[3] * a[7] - a[4] * a[6];
    double det = a[0] * c00 + a[1] * c01 + a[2] * c02;
    if (!(std::fabs(det) > 1e-300)) return false;
    double inv = 1.0 / det;
    out[0] = c00 * inv;
    out[1] = (a[2] * a[7] - a[1] * a[8]) * inv;
    out[2] = (a[1] * a[5] - a[2] * a[4]) * inv;
    out[3] = c01 * inv;
    out[4] = (a[0] * a[8] - a[2] * a[6]) * inv;
    out[5] = (a[2] * a[3] - a[0] * a[5]) * inv;
    out[6] = c02 * inv;
    out[7] = (a[1] * a[6] - a[0] * a[7]) * inv;
    out[8] = (a[0] * a[4] - a[1] * a[3]) * inv;
    return true;
}

namespace {
// Camera-space z of layer point (x, y, 0) under the row-vector matrix R (layer px -> camera space).
double ZOf(const double R[16], double x, double y) { return x * R[2] + y * R[6] + R[14]; }

// layer px -> camera space for a copy: copyM4 * W * C^-1 (copyM4 may be null = identity).
bool LayerToCam(const CameraView& cam, const double W[16], const double* copyM4, double R[16])
{
    double Cinv[16];
    if (!Invert4(cam.camToWorld, Cinv)) return false;
    if (copyM4) {
        double CW[16];
        Mul4(copyM4, W, CW);
        Mul4(CW, Cinv, R);
    } else {
        Mul4(W, Cinv, R);
    }
    return true;
}
}  // namespace

bool PlaneM4(const CameraView& cam, const double layerToWorld[16], double depth, double out[16])
{
    if (!(cam.dist > 0) || !(depth > 0)) return false;
    double Winv[16];
    if (!Invert4(layerToWorld, Winv)) return false;
    double k = depth / cam.dist;
    const double S[16] = {k, 0, 0, 0, 0, k, 0, 0, 0, 0, 1, 0, -k * cam.compW / 2.0, -k * cam.compH / 2.0, depth, 1};
    double SC[16];
    Mul4(S, cam.camToWorld, SC);
    Mul4(SC, Winv, out);
    return true;
}

double AnchorDepth(const CameraView& cam, const double layerToWorld[16], double ax, double ay)
{
    double R[16];
    if (!LayerToCam(cam, layerToWorld, nullptr, R)) return 0;
    return ZOf(R, ax, ay);
}

bool CopyHomography(const CameraView& cam, const double layerToWorld[16], const double copyM4[16],
                    const double srcRect[4], double screenToSrc[9])
{
    double R[16];
    if (!(cam.dist > 0) || !LayerToCam(cam, layerToWorld, copyM4, R)) return false;
    const double xs[2] = {srcRect[0], srcRect[2]}, ys[2] = {srcRect[1], srcRect[3]};
    for (double x : xs)
        for (double y : ys)
            if (!(ZOf(R, x, y) > 1.0)) return false;
    // src (u, v, 1) -> homogeneous screen (sx * z, sy * z, z); rows 0, 1, 3 of R (z = 0 plane).
    double H[9];
    const int rows[3] = {0, 1, 3};
    for (int i = 0; i < 3; i++) {
        const double* r = R + rows[i] * 4;
        H[i * 3 + 0] = r[0] * cam.dist + r[2] * cam.compW / 2.0;
        H[i * 3 + 1] = r[1] * cam.dist + r[2] * cam.compH / 2.0;
        H[i * 3 + 2] = r[2];
    }
    return Invert3(H, screenToSrc);
}

bool CopyScreenBounds(const CameraView& cam, const double layerToWorld[16], const double copyM4[16],
                      const double srcRect[4], double out[4])
{
    double S[9], H[9];
    if (!CopyHomography(cam, layerToWorld, copyM4, srcRect, S) || !Invert3(S, H)) return false;
    out[0] = out[1] = 1e300;
    out[2] = out[3] = -1e300;
    const double xs[2] = {srcRect[0], srcRect[2]}, ys[2] = {srcRect[1], srcRect[3]};
    for (double u : xs)
        for (double v : ys) {
            // [u v 1] * H = w * [sx sy 1]
            const double X = u * H[0] + v * H[3] + H[6], Y = u * H[1] + v * H[4] + H[7], W = u * H[2] + v * H[5] + H[8];
            if (!(W > 0)) return false;
            out[0] = std::min(out[0], X / W), out[1] = std::min(out[1], Y / W);
            out[2] = std::max(out[2], X / W), out[3] = std::max(out[3], Y / W);
        }
    return true;
}

double CopyDepth(const CameraView& cam, const double layerToWorld[16], const double copyM4[16], double cx, double cy)
{
    double R[16];
    if (!LayerToCam(cam, layerToWorld, copyM4, R)) return 0;
    return ZOf(R, cx, cy);
}

}  // namespace rnd
