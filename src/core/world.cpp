#include "core/world.h"

#include <cmath>

#include "core/fast_plane.h"
#include "core/scatter.h"

namespace rnd {

namespace {
const double kPi = 3.14159265358979323846;
const double kR2D = 180.0 / kPi;
}  // namespace

void WorldM4(const WorldXform& w, double ax, double ay, double out[16])
{
    // detail::MakeM4 with a uniform scale of 1, then the per-axis scale folded into the 3x3 rows
    double m[16];
    detail::MakeM4(w.pos[0], w.pos[1], w.pos[2], w.rotDeg[0], w.rotDeg[1], w.rotDeg[2], 1.0, ax, ay, m);
    // m = T(-A) R T(A + P); want T(-A) S R T(A + P): rows of R scaled, translation recomputed
    double R[9] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
    const double s[3] = {w.scale[0] / 100.0, w.scale[1] / 100.0, w.scale[2] / 100.0};
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) out[r * 4 + c] = s[r] * R[r * 3 + c];
    out[3] = out[7] = out[11] = 0;
    const double a[3] = {-ax, -ay, 0};
    for (int c = 0; c < 3; c++) {
        double t = 0;
        for (int k = 0; k < 3; k++) t += a[k] * out[k * 4 + c];
        out[12 + c] = t + (c == 0 ? ax : c == 1 ? ay : 0) + w.pos[c];
    }
    out[15] = 1;
}

bool DecomposeWorld(const double G[16], double ax, double ay, WorldXform* out)
{
    const double eps = 1e-12;
    if (std::fabs(G[3]) > 1e-9 || std::fabs(G[7]) > 1e-9 || std::fabs(G[11]) > 1e-9 || std::fabs(G[15] - 1) > 1e-9)
        return false;
    double M[9] = {G[0], G[1], G[2], G[4], G[5], G[6], G[8], G[9], G[10]};
    double det = M[0] * (M[4] * M[8] - M[5] * M[7]) - M[1] * (M[3] * M[8] - M[5] * M[6]) +
                 M[2] * (M[3] * M[7] - M[4] * M[6]);
    if (std::fabs(det) < eps) return false;
    double s[3], R[9];
    for (int r = 0; r < 3; r++) {
        s[r] = std::sqrt(M[r * 3] * M[r * 3] + M[r * 3 + 1] * M[r * 3 + 1] + M[r * 3 + 2] * M[r * 3 + 2]);
        if (s[r] < eps) return false;
    }
    if (det < 0) s[2] = -s[2];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) R[r * 3 + c] = M[r * 3 + c] / s[r];
    // R = Rz * Ry * Rx (row vectors): R[2][0] = sin y, R[0][0] = cz cy, R[1][0] = -sz cy,
    // R[2][1] = -cy sx, R[2][2] = cy cx. Shear leaves R slightly off a rotation: the nearest angles result.
    double sy = R[6] < -1 ? -1 : R[6] > 1 ? 1 : R[6];
    double cy = std::sqrt(R[0] * R[0] + R[3] * R[3]);
    double x, y = std::atan2(sy, cy), z;
    if (cy > 1e-9) {
        z = std::atan2(-R[3], R[0]);
        x = std::atan2(-R[7], R[8]);
    } else {  // gimbal lock: z = 0, row 1 = (0, cx, sx)
        z = 0;
        x = std::atan2(R[5], R[4]);
    }
    WorldXform w;
    w.rotDeg[0] = x * kR2D;
    w.rotDeg[1] = y * kR2D;
    w.rotDeg[2] = z * kR2D;
    for (int k = 0; k < 3; k++) w.scale[k] = s[k] * 100.0;
    // G row 3 = -A * M3 + A + P  ->  P = t + A * M3 - A (computed with the exact M3 of the rebuilt matrix)
    double rebuilt[16];
    WorldM4(w, ax, ay, rebuilt);  // its translation is (-A M3' + A) with P = 0
    for (int c = 0; c < 3; c++) w.pos[c] = G[12 + c] - rebuilt[12 + c];
    *out = w;
    return true;
}

void InitialLayerToWorld(double ax, double ay, double compW, double compH, double out[16])
{
    for (int i = 0; i < 16; i++) out[i] = (i % 5 == 0) ? 1.0 : 0.0;
    out[12] = compW / 2 - ax;
    out[13] = compH / 2 - ay;
}

bool TransferToWorld(const WorldXform& cur, const double layerToWorld[16], double ax, double ay, double compW,
                     double compH, WorldXform* out)
{
    double g[16], v[16], vi[16], a[16], b[16];
    WorldM4(cur, ax, ay, g);
    InitialLayerToWorld(ax, ay, compW, compH, v);
    if (!Invert4(v, vi)) return false;
    Mul4(g, layerToWorld, a);
    Mul4(a, vi, b);
    return DecomposeWorld(b, ax, ay, out);
}

}  // namespace rnd
