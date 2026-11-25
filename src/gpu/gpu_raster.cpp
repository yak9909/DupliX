#include "gpu/gpu_raster.h"

#include <windows.h>

#include <map>
#include <mutex>
#include <vector>

#include "core/tiles.h"
#include "gpu/fast_raster_ptx.h"

namespace rnd {

namespace {

// ---- CUDA driver API (subset of cuda.h), loaded from nvcuda.dll
typedef int CUresult;
typedef unsigned long long CUdeviceptr;
typedef void* CUcontext;
typedef void* CUmodule;
typedef void* CUfunction;
typedef void* CUstream;
typedef int CUdevice;

struct Api {
    CUresult(__stdcall* init)(unsigned);
    CUresult(__stdcall* deviceGet)(CUdevice*, int);
    CUresult(__stdcall* ctxCreate)(CUcontext*, unsigned, CUdevice);
    CUresult(__stdcall* ctxDestroy)(CUcontext);
    CUresult(__stdcall* ctxPush)(CUcontext);
    CUresult(__stdcall* ctxPop)(CUcontext*);
    CUresult(__stdcall* moduleLoadData)(CUmodule*, const void*);
    CUresult(__stdcall* moduleGetFunction)(CUfunction*, CUmodule, const char*);
    CUresult(__stdcall* memAlloc)(CUdeviceptr*, size_t);
    CUresult(__stdcall* memFree)(CUdeviceptr);
    CUresult(__stdcall* htod)(CUdeviceptr, const void*, size_t);
    CUresult(__stdcall* dtoh)(void*, CUdeviceptr, size_t);
    CUresult(__stdcall* htodAsync)(CUdeviceptr, const void*, size_t, CUstream);
    CUresult(__stdcall* launch)(CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                                CUstream, void**, void**);
    CUresult(__stdcall* streamSync)(CUstream);
    bool ok = false;
};

const Api& Cuda()
{
    static Api api = [] {
        Api a = {};
        HMODULE m = LoadLibraryA("nvcuda.dll");
        if (!m) return a;
        auto get = [m](const char* n) { return GetProcAddress(m, n); };
        a.init = (decltype(a.init))get("cuInit");
        a.deviceGet = (decltype(a.deviceGet))get("cuDeviceGet");
        a.ctxCreate = (decltype(a.ctxCreate))get("cuCtxCreate_v2");
        a.ctxDestroy = (decltype(a.ctxDestroy))get("cuCtxDestroy_v2");
        a.ctxPush = (decltype(a.ctxPush))get("cuCtxPushCurrent_v2");
        a.ctxPop = (decltype(a.ctxPop))get("cuCtxPopCurrent_v2");
        a.moduleLoadData = (decltype(a.moduleLoadData))get("cuModuleLoadData");
        a.moduleGetFunction = (decltype(a.moduleGetFunction))get("cuModuleGetFunction");
        a.memAlloc = (decltype(a.memAlloc))get("cuMemAlloc_v2");
        a.memFree = (decltype(a.memFree))get("cuMemFree_v2");
        a.htod = (decltype(a.htod))get("cuMemcpyHtoD_v2");
        a.dtoh = (decltype(a.dtoh))get("cuMemcpyDtoH_v2");
        a.htodAsync = (decltype(a.htodAsync))get("cuMemcpyHtoDAsync_v2");
        a.launch = (decltype(a.launch))get("cuLaunchKernel");
        a.streamSync = (decltype(a.streamSync))get("cuStreamSynchronize");
        a.ok = a.init && a.deviceGet && a.ctxCreate && a.ctxDestroy && a.ctxPush && a.ctxPop && a.moduleLoadData &&
               a.moduleGetFunction && a.memAlloc && a.memFree && a.htod && a.dtoh && a.htodAsync && a.launch &&
               a.streamSync;
        return a;
    }();
    return api;
}

// Must match GCopy in fast_raster.cu.
struct GCopy {
    float h[9];
    float opacity;
    float nearIz;
    int rank;
};
static_assert(sizeof(GCopy) == 48, "GCopy layout is shared with the kernel");

// The kernel, loaded once per context (AE may use several GPUs / contexts).
std::mutex g_moduleMutex;
std::map<CUcontext, CUfunction> g_functions;

CUfunction FunctionFor(CUcontext ctx)
{
    std::lock_guard<std::mutex> lk(g_moduleMutex);
    auto it = g_functions.find(ctx);
    if (it != g_functions.end()) return it->second;
    const Api& cu = Cuda();
    CUmodule mod = nullptr;
    CUfunction fn = nullptr;
    if (cu.moduleLoadData(&mod, kFastRasterPtx) || cu.moduleGetFunction(&fn, mod, "FastRaster")) fn = nullptr;
    if (fn) g_functions[ctx] = fn;
    return fn;
}

// The context of the calling thread for the duration of a scope.
struct CtxScope {
    bool pushed = false;
    explicit CtxScope(CUcontext c) { pushed = c && Cuda().ctxPush(c) == 0; }
    ~CtxScope()
    {
        CUcontext old = nullptr;
        if (pushed) Cuda().ctxPop(&old);
    }
};

struct DevBuf {
    CUdeviceptr p = 0;
    ~DevBuf()
    {
        if (p) Cuda().memFree(p);
    }
    bool Put(const void* host, size_t bytes, CUstream stream)
    {
        // never a zero-sized allocation; nothing to copy then
        return Cuda().memAlloc(&p, bytes ? bytes : 4) == 0 &&
               (!host || !bytes || Cuda().htodAsync(p, host, bytes, stream) == 0);
    }
};

}  // namespace

bool GpuRasterAvailable() { return Cuda().ok; }

bool GpuRasterize(void* cuContext, void* cuStream, const GpuImage& src, const GpuImage& dst, const RasterCopy* copies,
                  int n, double dsX, double dsY)
{
    const Api& cu = Cuda();
    if (!cu.ok || !dst.devPtr || dst.width <= 0 || dst.height <= 0) return cu.ok;
    if (!(dsX > 0) || !(dsY > 0) || !src.devPtr) n = 0;
    // The same copy lists as the CPU rasterizer (only the geometry of src / dst is used).
    RasterImage s = {nullptr, src.width, src.height, src.rowBytes, 32, src.originX, src.originY};
    RasterImage d = {nullptr, dst.width, dst.height, dst.rowBytes, 32, dst.originX, dst.originY};
    d.screenShiftX = dst.screenShiftX;
    d.screenShiftY = dst.screenShiftY;
    PreparedCopies pc;
    PrepareCopies(s, copies, n, dsX, dsY, d, 16, &pc);
    // Fold the buffer <-> full-resolution px mappings into each homography (in double, then float):
    // screen = (buffer + origin + 0.5) / ds + shift, src buffer = src px * ds - 0.5 - src origin.
    std::vector<GCopy> g(pc.copies.size());
    const double M[9] = {1 / dsX, 0, 0, 0, 1 / dsY, 0, (dst.originX + 0.5) / dsX + dst.screenShiftX,
                         (dst.originY + 0.5) / dsY + dst.screenShiftY, 1};
    const double N[9] = {dsX, 0, 0, 0, dsY, 0, -0.5 - src.originX, -0.5 - src.originY, 1};
    for (size_t i = 0; i < g.size(); i++) {
        const double* H = pc.copies[i].screenToSrc;
        double MH[9], F[9];
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) MH[r * 3 + c] = M[r * 3] * H[c] + M[r * 3 + 1] * H[3 + c] + M[r * 3 + 2] * H[6 + c];
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) F[r * 3 + c] = MH[r * 3] * N[c] + MH[r * 3 + 1] * N[3 + c] + MH[r * 3 + 2] * N[6 + c];
        for (int k = 0; k < 9; k++) g[i].h[k] = (float)F[k];
        g[i].opacity = (float)pc.copies[i].opacity;
        g[i].nearIz = pc.nearIz[i];
        g[i].rank = pc.rank[i];
    }
    CtxScope scope((CUcontext)cuContext);
    if (cuContext && !scope.pushed) return false;
    CUfunction fn = FunctionFor((CUcontext)cuContext);
    if (!fn) return false;
    CUstream stream = (CUstream)cuStream;
    DevBuf bc, bs, bi;
    if (!bc.Put(g.data(), g.size() * sizeof(GCopy), stream) ||
        !bs.Put(pc.tiles.start.data(), pc.tiles.start.size() * sizeof(int), stream) ||
        !bi.Put(pc.tiles.index.empty() ? nullptr : pc.tiles.index.data(), pc.tiles.index.size() * sizeof(int), stream))
        return false;
    CUdeviceptr srcPtr = (CUdeviceptr)src.devPtr, dstPtr = (CUdeviceptr)dst.devPtr;
    int srcPitch = (int)(src.rowBytes / 16), srcW = n ? src.width : 0, srcH = n ? src.height : 0;
    int dstPitch = (int)(dst.rowBytes / 16), dstW = dst.width, dstH = dst.height, tilesX = pc.tiles.tilesX;
    CUdeviceptr pCopies = bc.p, pStart = bs.p, pIndex = bi.p;
    void* args[] = {&srcPtr, &srcPitch, &srcW, &srcH, &dstPtr, &dstPitch, &dstW, &dstH, &pCopies, &pStart, &pIndex, &tilesX};
    if (cu.launch(fn, (unsigned)pc.tiles.tilesX, (unsigned)pc.tiles.tilesY, 1, 16, 16, 1, 0, stream, args, nullptr))
        return false;
    // the copy lists are freed when this returns: wait for the kernel (AE reads dst after the command anyway)
    return cu.streamSync(stream) == 0;
}

namespace cu {
bool Init() { return Cuda().ok && Cuda().init(0) == 0; }
void* CreateContext()
{
    CUdevice dev = 0;
    CUcontext ctx = nullptr;
    if (Cuda().deviceGet(&dev, 0) || Cuda().ctxCreate(&ctx, 0, dev)) return nullptr;
    return ctx;  // current on this thread
}
void DestroyContext(void* ctx)
{
    if (ctx) Cuda().ctxDestroy((CUcontext)ctx);
}
void* Alloc(size_t bytes)
{
    CUdeviceptr p = 0;
    return Cuda().memAlloc(&p, bytes) ? nullptr : (void*)p;
}
void Free(void* p)
{
    if (p) Cuda().memFree((CUdeviceptr)p);
}
bool Upload(void* dev, const void* host, size_t bytes) { return Cuda().htod((CUdeviceptr)dev, host, bytes) == 0; }
bool Download(void* host, const void* dev, size_t bytes) { return Cuda().dtoh(host, (CUdeviceptr)dev, bytes) == 0; }
}  // namespace cu

}  // namespace rnd
