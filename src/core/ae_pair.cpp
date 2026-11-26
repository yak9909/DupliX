#include "core/ae_pair.h"

namespace rnd {

namespace {

// minsd / maxsd as AE uses them: the second operand unless the first is smaller (greater)
inline double Mn(double a, double b) { return a < b ? a : b; }
inline double Mx(double a, double b) { return a > b ? a : b; }
constexpr double kEps = 1e-12, kNeg = -1e-12;  // qword_180080548, qword_1800805B0
thread_local long long t_cov[AE_PAIR_COV_N];

struct Ctx {
    double kx, ky, k0;  // the depth difference a - b: (ky y + kx x) + k0
    unsigned char *f1, *f2;
    void Flag(double d) const
    {
        if (d > kNeg) *f1 = 1;
        if (kEps > d) *f2 = 1;
    }
    double Diff(double x, double y) const { return (ky * y + kx * x) + k0; }
};

inline double X(const double* v, int k) { return v[4 * k]; }
inline double Y(const double* v, int k) { return v[4 * k + 1]; }

// sub_18002B1B0: edges P1 -> P2 (a's) and Q1 -> Q2 (b's) on one line
void Collinear(const Ctx& c, const double* P1, const double* P2, const double* Q1, const double* Q2)
{
    t_cov[0]++;
    double dy = P1[1] - P2[1], dx = P1[0] - P2[0];
    dy = dy < 0 ? -dy : dy;  // vandpd with the sign mask
    dx = dx < 0 ? -dx : dx;
    const int ax = dx > dy ? 0 : 1;
    {
        const double q2 = Q2[ax], q1 = Q1[ax], p1 = P1[ax];
        bool in = false;
        if (q2 > q1) {
            if (p1 > q1 - kEps && q2 + kEps > p1) in = true;
        } else {
            if (p1 > q2 - kEps && q1 + kEps > p1) in = true;
        }
        const double* p = in ? P1 : P2;
        c.Flag((p[1] * c.ky + p[0] * c.kx) + c.k0);
    }
    {
        const double p2 = P2[ax], p1 = P1[ax], q1 = Q1[ax];
        bool in = false;
        if (p2 > p1) {
            if (q1 > p1 - kEps && p2 + kEps > q1) in = true;
        } else {
            if (q1 > p2 - kEps && p1 + kEps > q1) in = true;
        }
        const double* q = in ? Q1 : Q2;
        c.Flag((q[1] * c.ky + q[0] * c.kx) + c.k0);
    }
}

// sub_18002BC80: does the edge Q1 -> Q2 cross the ray from P towards +x (counted into *cnt)
void Count(const double* P, const double* Q1, const double* Q2, unsigned short* cnt)
{
    const double d1 = Q1[1] - P[1], d2 = Q2[1] - P[1], px = P[0];
    if ((d2 >= 0) == (d1 >= 0)) return;
    const double e1 = Q1[0] - px, e2 = Q2[0] - px;
    const bool a = e1 >= 0, b = e2 >= 0;
    if (a && b) {
        ++*cnt;
        return;
    }
    if (!a && !b) return;
    const double dd = d2 - d1;
    if (kEps > dd && dd > kNeg) return;
    const double xq = e1 - ((e2 - e1) * d1) / dd;
    if (xq >= 0) ++*cnt;
}

}  // namespace

bool AePairBoxes(const double a[4], const double b[4])
{
    if (a[0] >= a[2] || a[1] >= a[3] || b[0] >= b[2] || b[1] >= b[3]) return false;
    if (!(Mn(a[3], b[3]) > Mx(a[1], b[1]))) return false;
    if (!(Mn(a[2], b[2]) > Mx(a[0], b[0]))) return false;
    return true;
}

bool AePairSamples(const AePairSample& A, const AePairSample& B, unsigned char* f1, unsigned char* f2)
{
    const int nA = A.n, nB = B.n;
    constexpr int kMax = 64;
    if (nA > kMax || nB > kMax) return false;
    Ctx c;
    c.k0 = A.d / A.c - B.d / B.c;
    c.ky = A.b / A.c - B.b / B.c;
    c.kx = A.a / A.c - B.a / B.c;
    c.f1 = f1, c.f2 = f2;
    unsigned short ca[kMax] = {}, cb[kMax] = {};
    const double* va = A.v;
    const double* vb = B.v;
    int p = nA - 1;
    for (int i = 0; i < nA; i++) {
        int q = nB - 1;
        for (int j = 0; j < nB; j++) {
            const double xp = X(va, p), yp = Y(va, p), xi = X(va, i), yi = Y(va, i);
            const double xq = X(vb, q), yq = Y(vb, q), xj = X(vb, j), yj = Y(vb, j);
            const bool apart = Mn(xp, xi) > Mx(xq, xj) || Mn(xq, xj) > Mx(xp, xi) || Mn(yp, yi) > Mx(yq, yj) ||
                               Mn(yq, yj) > Mx(yp, yi);
            if (!apart) {
                const double dBx = xj - xq;
                const double sp = (xp - xq) * (yj - yq) - (yp - yq) * dBx;
                const bool onP = kEps > sp && sp > kNeg;
                const double si = (xi - xq) * (yj - yq) - (yi - yq) * dBx;
                const bool onI = kEps > si && si > kNeg;
                if (onP && onI) {
                    Collinear(c, va + 4 * p, va + 4 * i, vb + 4 * q, vb + 4 * j);
                } else if (onP || onI) {
                    t_cov[onP ? 1 : 2]++;
                    c.Flag(c.Diff(xp, yp));  // AE takes p in both cases
                } else if ((si > 0) != (sp > 0)) {
                    const double dAx = xi - xp, dAy = yi - yp;
                    const double tq = (xq - xp) * dAy - (yq - yp) * dAx;
                    const bool onQ = kEps > tq && tq > kNeg;
                    const double tj = dAy * (xj - xp) - dAx * (yj - yp);
                    const bool onJ = kEps > tj && tj > kNeg;
                    if (onQ && onJ) {
                        Collinear(c, va + 4 * p, va + 4 * i, vb + 4 * q, vb + 4 * j);
                    } else if (onQ) {
                        t_cov[3]++;
                        c.Flag(c.Diff(xq, yq));
                    } else if (onJ) {
                        t_cov[4]++;
                        c.Flag(c.Diff(xj, yj));
                    } else if ((tj > 0) != (tq > 0)) {
                        const double dBy = yj - yq;
                        const double den = dAx * dBy - dAy * dBx;
                        if (!(den == 0)) {
                            t_cov[5]++;
                            const double e = yq * dBx - xq * dBy;
                            const double g = dBy * xp - dBx * yp;
                            const double h = g + e;
                            const double t = (-h) / den;
                            const double y = t * dAy + yp, x = t * dAx + xp;
                            c.Flag((y * c.ky + x * c.kx) + c.k0);
                        }
                    }
                }
            }
            if (*f1 && *f2) return true;  // AE returns here (the answer cannot change)
            Count(va + 4 * p, vb + 4 * q, vb + 4 * j, &ca[p]);
            Count(vb + 4 * q, va + 4 * p, va + 4 * i, &cb[q]);
            q = j;
        }
        p = i;
    }
    for (int k = 0; k < nA; k++)
        if (ca[k] & 1) t_cov[6]++, c.Flag(c.Diff(X(va, k), Y(va, k)));
    for (int k = 0; k < nB; k++)
        if (cb[k] & 1) t_cov[7]++, c.Flag(c.Diff(X(vb, k), Y(vb, k)));
    return true;
}

const long long* AePairCoverage() { return t_cov; }

}  // namespace rnd
