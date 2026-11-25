#include "core/tiles.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <new>
#include <numeric>

#include "core/fast_plane.h"

namespace rnd {

bool CopyBox(const RasterImage& src, const RasterCopy& c, double dsX, double dsY, const RasterImage& dst, int* x0,
             int* y0, int* x1, int* y1, double* nearIz)
{
    double F[9];
    bool whole = !Invert3(c.screenToSrc, F);
    double mnx = 1e300, mny = 1e300, mxx = -1e300, mxy = -1e300, near = 0;
    const double us[2] = {src.originX / dsX, (src.originX + src.width) / dsX};
    const double vs[2] = {src.originY / dsY, (src.originY + src.height) / dsY};
    for (double u : us)
        for (double v : vs) {
            if (whole) break;
            double a = u * F[0] + v * F[3] + F[6], b = u * F[1] + v * F[4] + F[7], w = u * F[2] + v * F[5] + F[8];
            if (!(w > 0)) {
                whole = true;
                break;
            }
            near = std::max(near, 1.0 / w);  // (sx, sy, 1) * H = (u, v, 1) / w: 1 / z at this corner is 1 / w
            double sx = (a / w - dst.screenShiftX) * dsX - dst.originX - 0.5,
                   sy = (b / w - dst.screenShiftY) * dsY - dst.originY - 0.5;
            mnx = std::min(mnx, sx), mxx = std::max(mxx, sx), mny = std::min(mny, sy), mxy = std::max(mxy, sy);
        }
    if (nearIz) *nearIz = whole ? HUGE_VAL : near;
    if (whole) {
        *x0 = 0, *y0 = 0, *x1 = dst.width - 1, *y1 = dst.height - 1;
        return dst.width > 0 && dst.height > 0;
    }
    double lx = std::floor(mnx) - 1, hx = std::ceil(mxx) + 1, ly = std::floor(mny) - 1, hy = std::ceil(mxy) + 1;
    if (hx < 0 || hy < 0 || lx > dst.width - 1 || ly > dst.height - 1) return false;
    *x0 = (int)std::max(lx, 0.0);
    *y0 = (int)std::max(ly, 0.0);
    *x1 = (int)std::min(hx, (double)dst.width - 1);
    *y1 = (int)std::min(hy, (double)dst.height - 1);
    return true;
}

void BuildTiles(const RasterImage& src, const RasterCopy* copies, int n, double dsX, double dsY,
                const RasterImage& dst, int tileSize, TileLists* out)
{
    out->tileSize = tileSize;
    out->tilesX = (std::max(dst.width, 0) + tileSize - 1) / tileSize;
    out->tilesY = (std::max(dst.height, 0) + tileSize - 1) / tileSize;
    const int nt = out->tilesX * out->tilesY;
    std::vector<int> x0(n), y0(n), x1(n), y1(n);
    std::vector<char> hit(n);
    // The total in 64 bits before any per-tile counting: past the limit the lists cannot be built (an int index,
    // and memory): refuse like a failed allocation instead of wrapping around.
    long long total = 0;
    for (int i = 0; i < n; i++) {
        hit[i] = copies[i].opacity > 0 && CopyBox(src, copies[i], dsX, dsY, dst, &x0[i], &y0[i], &x1[i], &y1[i]);
        if (!hit[i]) continue;
        total += (long long)(x1[i] / tileSize - x0[i] / tileSize + 1) * (y1[i] / tileSize - y0[i] / tileSize + 1);
        if (total > kMaxTileEntries) throw std::bad_alloc();
    }
    std::vector<int> count((size_t)nt + 1, 0);
    for (int i = 0; i < n; i++) {
        if (!hit[i]) continue;
        for (int ty = y0[i] / tileSize; ty <= y1[i] / tileSize; ty++)
            for (int tx = x0[i] / tileSize; tx <= x1[i] / tileSize; tx++) count[(size_t)ty * out->tilesX + tx]++;
    }
    out->start.assign((size_t)nt + 1, 0);
    for (int t = 0; t < nt; t++) out->start[t + 1] = out->start[t] + count[t];
    out->index.assign((size_t)out->start[nt], 0);
    std::vector<int> fill(out->start.begin(), out->start.end() - 1);
    for (int i = 0; i < n; i++) {
        if (!hit[i]) continue;
        for (int ty = y0[i] / tileSize; ty <= y1[i] / tileSize; ty++)
            for (int tx = x0[i] / tileSize; tx <= x1[i] / tileSize; tx++)
                out->index[fill[(size_t)ty * out->tilesX + tx]++] = i;
    }
}

void PrepareCopies(const RasterImage& src, const RasterCopy* copies, int n, double dsX, double dsY,
                   const RasterImage& dst, int tileSize, PreparedCopies* out)
{
    std::vector<int> order(std::max(n, 0));
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return copies[a].depth > copies[b].depth; });
    struct Item {
        int copy, rank;
        double nearIz;
    };
    std::vector<Item> items;
    items.reserve(order.size());
    for (int r = 0; r < (int)order.size(); r++) {
        const RasterCopy& c = copies[order[r]];
        int x0, y0, x1, y1;
        double nz = 0;
        if (c.opacity > 0 && CopyBox(src, c, dsX, dsY, dst, &x0, &y0, &x1, &y1, &nz)) items.push_back({order[r], r, nz});
    }
    // nearest first; equal nearest points: the higher rank (drawn on top) first
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.nearIz != b.nearIz ? a.nearIz > b.nearIz : a.rank > b.rank;
    });
    out->copies.resize(items.size());
    out->rank.resize(items.size());
    out->nearIz.resize(items.size());
    for (size_t i = 0; i < items.size(); i++) {
        out->copies[i] = copies[items[i].copy];
        out->rank[i] = items[i].rank;
        out->nearIz[i] = items[i].nearIz > FLT_MAX ? FLT_MAX : (float)items[i].nearIz;
    }
    BuildTiles(src, out->copies.data(), (int)out->copies.size(), dsX, dsY, dst, tileSize, &out->tiles);
}

}  // namespace rnd
