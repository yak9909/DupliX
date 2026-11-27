// DupliX core: the reference CPU rasterizer (unit tests only). Same contract as RasterizeCopies.
#pragma once

#include "core/raster.h"

namespace rnd {

bool RasterizeCopiesReference(const RasterImage& src, RasterCopy* copies, int n, double dsX, double dsY,
                              RasterImage& dst, int threads, const std::function<bool()>& abort = {});

}  // namespace rnd
