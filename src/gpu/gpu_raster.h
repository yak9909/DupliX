// randamu: the fast mode's rasterizer on the GPU (CUDA). The kernel (src/gpu/fast_raster.cu) is built into PTX
// at build time and embedded; at run time it is loaded with the CUDA driver API from nvcuda.dll, so the user's
// PC needs only the NVIDIA driver. No AE dependency.
#pragma once

#include <functional>

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

// The GPU may be offered for the fast mode: the driver is there and no kernel load / launch has failed in this
// process (a failure is sticky: the frames after it are drawn on the CPU).
bool GpuRasterUsable();

// Loads the kernel for this context now (at AE's GPU device setup), so a driver that cannot run it (e.g. too old
// for the PTX) is known before the first frame. A failure is cached and makes GpuRasterUsable() false.
bool GpuRasterPrepare(void* cuContext);
// Drops the cached kernel of a context AE is about to destroy (GPU device setdown).
void GpuRasterForget(void* cuContext);

enum GpuRasterStatus { kGpuOk, kGpuAborted, kGpuFailed };

// Device memory for the copy lists (e.g. AE's PF_GPUDeviceSuite1, which can purge AE's own caches when memory is
// short). alloc returns null on failure. Without one, cuMemAlloc / cuMemFree are used.
struct GpuAllocator {
    std::function<void*(size_t)> alloc;
    std::function<void(void*)> free;
};

// The same drawing as RasterizeCopies (copies in any order) into dst, on `cuStream` (a CUstream; nullptr = the
// default stream) of `cuContext` (a CUcontext, made current for the call). Waits until the drawing is done.
// Drawn in bands of tile rows (one short launch each: no driver timeout); abort (optional) is asked between them.
// kGpuFailed on any CUDA error or when the copy lists are too large for the GPU (dst is then undefined): draw
// with GpuRasterizeOnCpu instead.
GpuRasterStatus GpuRasterize(void* cuContext, void* cuStream, const GpuImage& src, const GpuImage& dst,
                             const RasterCopy* copies, int n, double dsX, double dsY,
                             const std::function<bool()>& abort = {}, const GpuAllocator* alloc = nullptr);

// The fallback inside AE's GPU render: copies src to host memory, draws with the CPU rasterizer (RasterizeCopies,
// the same result as the GPU within its tolerance) and uploads the result to dst. kGpuFailed if the memory copies
// fail (the context itself is broken).
GpuRasterStatus GpuRasterizeOnCpu(void* cuContext, void* cuStream, const GpuImage& src, const GpuImage& dst,
                                  const RasterCopy* copies, int n, double dsX, double dsY,
                                  const std::function<bool()>& abort = {});

// Where the rasterizer logs failures (printf-style; the effect passes its log).
void GpuRasterSetLog(void (*fn)(const char*, ...));

// For the GPU tests only.
void GpuRasterTestFail(bool fail);  // the kernel step fails as if the launch had failed
void GpuRasterTestReset();          // clears the sticky failure
int GpuRasterLastLaunches();        // launches of the last GpuRasterize on this thread
int GpuRasterCachedContexts();

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
