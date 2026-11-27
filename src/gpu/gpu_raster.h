// randamu: the fast mode's rasterizer on the GPU (CUDA). The kernel (src/gpu/fast_raster.cu) is built into PTX
// at build time and embedded; at run time it is loaded with the CUDA driver API from nvcuda.dll, so the user's
// PC needs only the NVIDIA driver. No AE dependency.
#pragma once

#include <functional>

#include "core/ae_span_impl.h"
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

// Host images (the merge's group textures: AE's CPU worlds, ARGB 8 / 16 / 32 bpc, straight alpha): the same drawing
// as RasterizeCopies into dst, on the GPU in the device's primary context with a stream of its own (callers take
// turns). src is uploaded, dst downloaded and rounded like the CPU rasterizer. kGpuFailed (dst undefined): draw with
// RasterizeCopies instead. *msOut (optional): the whole call's time.
GpuRasterStatus GpuRasterizeHost(const RasterImage& src, const RasterCopy* copies, int n, double dsX, double dsY,
                                 RasterImage& dst, double* msOut = nullptr);

// AE's classic 3D drawing of one surface's row spans on the GPU (core/ae_span_impl.h, the same pixels as AE).
// target: AE's 8 bpc ARGB buffer (drawn in place); rows: (y, x0, x1) per row; texKey: changes whenever the texture's
// pixels may have changed (the texture is uploaded again then). False: nothing drawn (the caller draws on the CPU).
struct GpuAeSpanJob {
    AeSpanCore core;
    AeTex8 tex;
    unsigned long long texKey;
    const float* rcp;  // 256 entries
    unsigned char* target;
    int targetRowbytes;
    const int* rows;
    int nRows;
};
// times of one call (diagnostics): texture upload (0 when the texture was on the GPU already), copying the region
// into and out of pinned memory on the CPU, the GPU part (upload, kernel, download), region size in pixels
struct GpuAeSpanTimes {
    double texMs = 0, stageMs = 0, gpuMs = 0;
    long long regionPx = 0;
};
bool GpuAeSpans(const GpuAeSpanJob& job, double* msOut = nullptr, GpuAeSpanTimes* times = nullptr);
// Several jobs drawn in order with one upload and one download (one kernel per job on one stream): they must share
// the target, the texture's pixels and the table. False: nothing drawn (the caller draws them one by one).
bool GpuAeSpansBatch(const GpuAeSpanJob* jobs, int n, double* msOut = nullptr);
// The fragments AE's M buffer row (sub_1800477F0) makes of the job's rows (core/ae_span's AeMbufSpan8; target and
// rcp unused): per pixel, the rows' pixels one after another, the sample col[4 i ..] and the depth depth[i].
// False: nothing computed (the caller computes them on the CPU).
bool GpuAeMbufSpans(const GpuAeSpanJob& job, unsigned char* col, float* depth, double* msOut = nullptr);

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
