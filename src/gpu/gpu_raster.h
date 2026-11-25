// randamu: the fast mode's rasterizer on the GPU (CUDA). The kernel (src/gpu/fast_raster.cu) is built into PTX
// at build time and embedded; at run time it is loaded with the CUDA driver API from nvcuda.dll, so the user's
// PC needs only the NVIDIA driver. No AE dependency.
#pragma once

#include "core/raster.h"

namespace rnd {

// A device buffer of AE's GPU pixels: BGRA, 32-bit float per channel, straight alpha (like the CPU worlds).
// Buffer pixel (i, j) is (downsampled) px (i + originX, j + originY); screenShift as RasterImage's.
struct GpuImage {
    void* devPtr;
    int width, height;
    long rowBytes;
    int originX, originY;
    double screenShiftX = 0, screenShiftY = 0;
};

// nvcuda.dll and the driver functions are there.
bool GpuRasterAvailable();

// The same drawing as RasterizeCopies (copies in any order) into dst, on `cuStream` (a CUstream; nullptr = the
// default stream) of `cuContext` (a CUcontext, made current for the call). Waits until the drawing is done.
// False on any CUDA error (dst is then undefined).
bool GpuRasterize(void* cuContext, void* cuStream, const GpuImage& src, const GpuImage& dst, const RasterCopy* copies,
                  int n, double dsX, double dsY);

// For the GPU tests only: a context of our own and plain device memory (current context).
namespace cu {
bool Init();
void* CreateContext();
void DestroyContext(void* ctx);
void* Alloc(size_t bytes);
void Free(void* p);
bool Upload(void* dev, const void* host, size_t bytes);
bool Download(void* host, const void* dev, size_t bytes);
}  // namespace cu

}  // namespace rnd
