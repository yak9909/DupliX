// randamu core: the per-tile copy lists shared by the CPU and the GPU rasterizers. No AE dependency.
#pragma once

#include <vector>

#include "core/raster.h"

namespace rnd {

// Tile t = (tx, ty) covers dst buffer px [tx * tileSize, ...) x [ty * tileSize, ...). Its copies are
// index[start[t] .. start[t + 1]), in the order of the copies given to BuildTiles.
struct TileLists {
    int tileSize = 16, tilesX = 0, tilesY = 0;
    std::vector<int> start;
    std::vector<int> index;
};

// Bounding box (dst buffer px, inclusive) of a copy; false if it misses dst. *nearIz (optional): the largest
// 1 / camera z over the copy (its nearest point; +inf when part of it is at or behind the camera).
bool CopyBox(const RasterImage& src, const RasterCopy& c, double dsX, double dsY, const RasterImage& dst, int* x0,
             int* y0, int* x1, int* y1, double* nearIz = nullptr);

// The most (copy, tile) entries the lists may hold (1 GB of indices). BuildTiles / PrepareCopies throw
// std::bad_alloc past it (e.g. 100000 copies each covering a whole 8K frame).
constexpr long long kMaxTileEntries = 1LL << 28;

void BuildTiles(const RasterImage& src, const RasterCopy* copies, int n, double dsX, double dsY,
                const RasterImage& dst, int tileSize, TileLists* out);

// The copies as both rasterizers draw them: sorted far to near by `depth` (stable) to get each copy's rank (the
// tie-break of equal per-pixel depths: higher rank on top), copies with no opacity or outside dst dropped, then
// ordered nearest first by their nearest point (so opaque near copies hide far ones early), and tiled.
struct PreparedCopies {
    std::vector<RasterCopy> copies;  // nearest first
    std::vector<int> rank;           // far-to-near rank of copies[i]
    std::vector<float> nearIz;       // largest 1 / z of copies[i] (float max when unbounded)
    TileLists tiles;
};
void PrepareCopies(const RasterImage& src, const RasterCopy* copies, int n, double dsX, double dsY,
                   const RasterImage& dst, int tileSize, PreparedCopies* out);

// Per-pixel fragment limit of both rasterizers. Past it the farthest fragment is composited under a back
// accumulator (approximate only where more than kMaxFrags fragments are visible at one pixel).
constexpr int kMaxFrags = 64;
// A fragment at least this opaque hides everything behind it at its pixel.
constexpr float kOpaqueAlpha = 1.f - 1.f / 1024.f;

}  // namespace rnd
