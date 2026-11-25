#include "core/ae_order.h"

#include <algorithm>
#include <cmath>

namespace rnd {

namespace {

struct Pt {
    double x, y;
};

double Cross(const Pt& o, const Pt& a, const Pt& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

// Convex polygon `subject` clipped by convex polygon `clip` (Sutherland-Hodgman; any winding).
std::vector<Pt> Clip(const std::vector<Pt>& subject, const std::vector<Pt>& clip)
{
    double area = 0;
    for (size_t k = 0; k < clip.size(); k++) {
        const Pt& a = clip[k];
        const Pt& b = clip[(k + 1) % clip.size()];
        area += a.x * b.y - b.x * a.y;
    }
    const double s = area >= 0 ? 1 : -1;
    std::vector<Pt> out = subject;
    for (size_t k = 0; k < clip.size() && !out.empty(); k++) {
        const Pt& a = clip[k];
        const Pt& b = clip[(k + 1) % clip.size()];
        std::vector<Pt> in;
        in.swap(out);
        Pt prev = in.back();
        bool prevIn = s * Cross(a, b, prev) >= 0;
        for (const Pt& cur : in) {
            const bool curIn = s * Cross(a, b, cur) >= 0;
            if (curIn != prevIn) {
                const double d1x = cur.x - prev.x, d1y = cur.y - prev.y, d2x = b.x - a.x, d2y = b.y - a.y;
                const double den = d1x * d2y - d1y * d2x;
                const double t = den != 0 ? ((a.x - prev.x) * d2y - (a.y - prev.y) * d2x) / den : 0;
                out.push_back({prev.x + t * d1x, prev.y + t * d1y});
            }
            if (curIn) out.push_back(cur);
            prev = cur;
            prevIn = curIn;
        }
    }
    return out;
}

double PolyArea(const std::vector<Pt>& p)
{
    double a = 0;
    for (size_t k = 0; k < p.size(); k++) a += p[k].x * p[(k + 1) % p.size()].y - p[(k + 1) % p.size()].x * p[k].y;
    return std::fabs(a) / 2;
}

// A surface's plane in camera space (the camera's own units, like ProjectRect): normal and a point.
struct CamPlane {
    double n[3], p[3];
};

CamPlane PlaneOf(const double inv[16], const double w[4][3])
{
    double c[3][3];
    const int idx[3] = {0, 1, 3};
    for (int k = 0; k < 3; k++)
        for (int j = 0; j < 3; j++)
            c[k][j] = w[idx[k]][0] * inv[j] + w[idx[k]][1] * inv[4 + j] + w[idx[k]][2] * inv[8 + j] + inv[12 + j];
    double u[3], v[3];
    for (int j = 0; j < 3; j++) u[j] = c[1][j] - c[0][j], v[j] = c[2][j] - c[0][j];
    CamPlane pl;
    pl.n[0] = u[1] * v[2] - u[2] * v[1], pl.n[1] = u[2] * v[0] - u[0] * v[2], pl.n[2] = u[0] * v[1] - u[1] * v[0];
    for (int j = 0; j < 3; j++) pl.p[j] = c[0][j];
    return pl;
}

// Camera depth where the ray through screen (x, y) meets the plane (huge if parallel).
double DepthAt(const CameraView& cam, const CamPlane& pl, double x, double y)
{
    const double d[3] = {x - cam.compW / 2, y - cam.compH / 2, cam.dist};
    const double den = pl.n[0] * d[0] + pl.n[1] * d[1] + pl.n[2] * d[2];
    if (den == 0) return 1e300;
    const double t = (pl.n[0] * pl.p[0] + pl.n[1] * pl.p[1] + pl.n[2] * pl.p[2]) / den;
    return t * cam.dist;
}

}  // namespace

AeOrderResult AeOrderGraph(const CameraView& cam, const std::vector<AeSurface>& s, double eps)
{
    AeOrderResult r;
    const int n = (int)s.size();
    r.inCycle.assign(n, 0);
    double inv[16];
    if (!Invert4(cam.camToWorld, inv)) return r;
    std::vector<CamPlane> planes(n);
    std::vector<std::vector<Pt>> polys(n);
    std::vector<double> bb(4 * (size_t)n);
    std::vector<int> order(n);
    for (int i = 0; i < n; i++) {
        planes[i] = PlaneOf(inv, s[i].w);
        double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
        for (int k = 0; k < 4; k++) {
            polys[i].push_back({s[i].sx[k], s[i].sy[k]});
            x0 = std::min(x0, s[i].sx[k]), x1 = std::max(x1, s[i].sx[k]);
            y0 = std::min(y0, s[i].sy[k]), y1 = std::max(y1, s[i].sy[k]);
        }
        bb[4 * i] = x0, bb[4 * i + 1] = y0, bb[4 * i + 2] = x1, bb[4 * i + 3] = y1;
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) { return bb[4 * a] < bb[4 * b]; });
    // edges: before[a] holds b when a must be painted before b (a is behind b somewhere)
    std::vector<std::vector<int>> before(n);
    for (int ii = 0; ii < n; ii++) {
        const int a = order[ii];
        for (int jj = ii + 1; jj < n; jj++) {
            const int b = order[jj];
            if (bb[4 * b] >= bb[4 * a + 2]) break;
            if (bb[4 * b + 1] >= bb[4 * a + 3] || bb[4 * a + 1] >= bb[4 * b + 3]) continue;
            const std::vector<Pt> ov = Clip(polys[a], polys[b]);
            if (ov.size() < 3 || PolyArea(ov) < 1e-6) continue;
            bool aFront = false, bFront = false;
            for (const Pt& p : ov) {
                const double za = DepthAt(cam, planes[a], p.x, p.y), zb = DepthAt(cam, planes[b], p.x, p.y);
                const double tol = eps * std::max(std::fabs(za), std::fabs(zb));
                if (za < zb - tol) aFront = true;
                if (zb < za - tol) bFront = true;
            }
            if (aFront) before[b].push_back(a);
            if (bFront) before[a].push_back(b);
            if (!aFront && !bFront) r.ties++;
        }
    }
    // Tarjan (iterative)
    std::vector<int> index(n, -1), low(n, 0), stack;
    std::vector<char> onStack(n, 0);
    int counter = 0;
    struct Frame {
        int v;
        size_t next;
    };
    for (int root = 0; root < n; root++) {
        if (index[root] >= 0) continue;
        std::vector<Frame> call{{root, 0}};
        index[root] = low[root] = counter++;
        stack.push_back(root);
        onStack[root] = 1;
        while (!call.empty()) {
            Frame& f = call.back();
            if (f.next < before[f.v].size()) {
                const int w = before[f.v][f.next++];
                if (index[w] < 0) {
                    index[w] = low[w] = counter++;
                    stack.push_back(w);
                    onStack[w] = 1;
                    call.push_back({w, 0});
                } else if (onStack[w]) {
                    low[f.v] = std::min(low[f.v], index[w]);
                }
                continue;
            }
            const int v = f.v;
            if (low[v] == index[v]) {
                std::vector<int> comp;
                int w;
                do {
                    w = stack.back();
                    stack.pop_back();
                    onStack[w] = 0;
                    comp.push_back(w);
                } while (w != v);
                if (comp.size() > 1)
                    for (int c : comp) r.inCycle[c] = 1, r.cycleSurfaces++;
            }
            call.pop_back();
            if (!call.empty()) low[call.back().v] = std::min(low[call.back().v], low[v]);
        }
    }
    return r;
}

AeSurface CameraFacingSurface(const CameraView& cam, const double rect[4], double d)
{
    AeSurface s;
    const double xs[4] = {rect[0], rect[2], rect[2], rect[0]}, ys[4] = {rect[1], rect[1], rect[3], rect[3]};
    for (int k = 0; k < 4; k++) {
        s.sx[k] = xs[k], s.sy[k] = ys[k];
        const double c[3] = {(xs[k] - cam.compW / 2) * d / cam.dist, (ys[k] - cam.compH / 2) * d / cam.dist, d};
        for (int j = 0; j < 3; j++)
            s.w[k][j] = c[0] * cam.camToWorld[j] + c[1] * cam.camToWorld[4 + j] + c[2] * cam.camToWorld[8 + j] +
                        cam.camToWorld[12 + j];
    }
    return s;
}

}  // namespace rnd
