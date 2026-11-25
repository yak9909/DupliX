#include "gpu/gpu_raster.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <new>
#include <thread>
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
    CUresult(__stdcall* moduleUnload)(CUmodule);
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
        a.moduleUnload = (decltype(a.moduleUnload))get("cuModuleUnload");
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

void (*g_log)(const char*, ...) = nullptr;
#define GpuLog(...) \
    do {                     \
        if (g_log) g_log(__VA_ARGS__); \
    } while (0)

// Must match GCopy in fast_raster.cu.
struct GCopy {
    float h[9];
    float opacity;
    float nearIz;
    int rank;
};
static_assert(sizeof(GCopy) == 48, "GCopy layout is shared with the kernel");

// Tile rows per launch: one launch stays short (well under the driver's ~2 s timeout) and AE's abort is asked
// between launches.
constexpr int kBandTileRows = 8;
// Copy-list entries the GPU path takes (128 MB of indices per frame); past it the CPU draws (host memory).
constexpr size_t kGpuMaxEntries = (size_t)1 << 25;

// A kernel load or launch failed in this process: the fast mode stops offering the GPU (sticky).
std::atomic<bool> g_failed{false};
std::atomic<bool> g_testFail{false};
thread_local int t_launches = 0;

// The kernel, loaded once per context (AE may use several GPUs / contexts). A failed load is cached too (null),
// so a driver that cannot run the PTX does not retry it on every frame under the lock.
struct Loaded {
    CUmodule mod = nullptr;
    CUfunction fn = nullptr;
};
std::mutex g_moduleMutex;
std::map<CUcontext, Loaded> g_functions;

// The calling thread has ctx current.
CUfunction FunctionFor(CUcontext ctx)
{
    std::lock_guard<std::mutex> lk(g_moduleMutex);
    auto it = g_functions.find(ctx);
    if (it != g_functions.end()) return it->second.fn;
    const Api& cu = Cuda();
    Loaded l;
    CUresult r = cu.moduleLoadData(&l.mod, kFastRasterPtx);
    if (!r) r = cu.moduleGetFunction(&l.fn, l.mod, "FastRaster");
    if (r) {
        if (l.mod && cu.moduleUnload) cu.moduleUnload(l.mod);
        l = Loaded{};
        g_failed = true;
        GpuLog("gpu raster: kernel load failed (CUresult %d): the fast mode draws on the CPU", (int)r);
    }
    g_functions[ctx] = l;
    return l.fn;
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
bool GpuRasterUsable() { return Cuda().ok && !g_failed.load(); }
void GpuRasterSetLog(void (*fn)(const char*, ...)) { g_log = fn; }

bool GpuRasterPrepare(void* cuContext)
{
    if (!Cuda().ok || !cuContext) return false;
    CtxScope scope((CUcontext)cuContext);
    if (!scope.pushed) return false;
    return FunctionFor((CUcontext)cuContext) != nullptr;
}

void GpuRasterForget(void* cuContext)
{
    std::lock_guard<std::mutex> lk(g_moduleMutex);
    auto it = g_functions.find((CUcontext)cuContext);
    if (it == g_functions.end()) return;
    // the context is still alive at setdown: unload its module (best effort)
    if (it->second.mod && Cuda().moduleUnload) {
        CtxScope scope((CUcontext)cuContext);
        if (scope.pushed) Cuda().moduleUnload(it->second.mod);
    }
    g_functions.erase(it);
}

void GpuRasterTestFail(bool fail) { g_testFail = fail; }
void GpuRasterTestReset() { g_failed = false; }
int GpuRasterLastLaunches() { return t_launches; }
int GpuRasterCachedContexts()
{
    std::lock_guard<std::mutex> lk(g_moduleMutex);
    return (int)g_functions.size();
}

GpuRasterStatus GpuRasterize(void* cuContext, void* cuStream, const GpuImage& src, const GpuImage& dst,
                             const RasterCopy* copies, int n, double dsX, double dsY, const std::function<bool()>& abort)
{
    t_launches = 0;
    const Api& cu = Cuda();
    if (!cu.ok) return kGpuFailed;
    if (!dst.devPtr || dst.width <= 0 || dst.height <= 0) return kGpuOk;
    if (!(dsX > 0) || !(dsY > 0) || !src.devPtr) n = 0;
    // The same copy lists as the CPU rasterizer (only the geometry of src / dst is used).
    RasterImage s = {nullptr, src.width, src.height, src.rowBytes, 32, src.originX, src.originY};
    RasterImage d = {nullptr, dst.width, dst.height, dst.rowBytes, 32, dst.originX, dst.originY};
    d.screenShiftX = dst.screenShiftX;
    d.screenShiftY = dst.screenShiftY;
    PreparedCopies pc;
    try {
        PrepareCopies(s, copies, n, dsX, dsY, d, 16, &pc);
    } catch (const std::bad_alloc&) {
        return kGpuFailed;  // too many entries: the CPU fallback decides (it refuses the same way)
    }
    if (pc.tiles.index.size() > kGpuMaxEntries) return kGpuFailed;
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
    if (cuContext && !scope.pushed) return kGpuFailed;
    CUfunction fn = FunctionFor((CUcontext)cuContext);
    if (!fn) return kGpuFailed;
    CUstream stream = (CUstream)cuStream;
    {
        DevBuf bc, bs, bi;  // freed before the context is popped
        if (!bc.Put(g.data(), g.size() * sizeof(GCopy), stream) ||
            !bs.Put(pc.tiles.start.data(), pc.tiles.start.size() * sizeof(int), stream) ||
            !bi.Put(pc.tiles.index.empty() ? nullptr : pc.tiles.index.data(), pc.tiles.index.size() * sizeof(int),
                    stream)) {
            // out of device memory (not a broken driver): this frame only
            GpuLog("gpu raster: device allocation failed (%zu entries): this frame on the CPU", pc.tiles.index.size());
            return kGpuFailed;
        }
        CUdeviceptr srcPtr = (CUdeviceptr)src.devPtr, dstPtr = (CUdeviceptr)dst.devPtr;
        int srcPitch = (int)(src.rowBytes / 16), srcW = n ? src.width : 0, srcH = n ? src.height : 0;
        int dstPitch = (int)(dst.rowBytes / 16), dstW = dst.width, dstH = dst.height, tilesX = pc.tiles.tilesX;
        CUdeviceptr pCopies = bc.p, pStart = bs.p, pIndex = bi.p;
        int tileY0 = 0;
        void* args[] = {&srcPtr, &srcPitch, &srcW, &srcH, &dstPtr, &dstPitch, &dstW, &dstH, &pCopies, &pStart, &pIndex,
                        &tilesX, &tileY0};
        for (; tileY0 < pc.tiles.tilesY; tileY0 += kBandTileRows) {
            if (tileY0 > 0 && abort && abort()) {
                cu.streamSync(stream);  // the launched bands still read the lists
                return kGpuAborted;
            }
            const int rows = std::min(kBandTileRows, pc.tiles.tilesY - tileY0);
            CUresult r = g_testFail ? 1 : cu.launch(fn, (unsigned)tilesX, (unsigned)rows, 1, 16, 16, 1, 0, stream, args, nullptr);
            // one band at a time: the next abort check comes after this band has run
            if (!r) r = cu.streamSync(stream);
            t_launches++;
            if (r) {
                g_failed = true;
                GpuLog("gpu raster: launch failed (CUresult %d): the fast mode draws on the CPU from now on", (int)r);
                cu.streamSync(stream);
                return kGpuFailed;
            }
        }
    }
    return kGpuOk;
}

GpuRasterStatus GpuRasterizeOnCpu(void* cuContext, void* cuStream, const GpuImage& src, const GpuImage& dst,
                                  const RasterCopy* copies, int n, double dsX, double dsY,
                                  const std::function<bool()>& abort)
{
    const Api& cu = Cuda();
    if (!cu.ok) return kGpuFailed;
    if (!dst.devPtr || dst.width <= 0 || dst.height <= 0) return kGpuOk;
    CtxScope scope((CUcontext)cuContext);
    if (cuContext && !scope.pushed) return kGpuFailed;
    CUstream stream = (CUstream)cuStream;
    if (cu.streamSync(stream)) return kGpuFailed;  // AE's work on src is done
    struct F4 {
        float c[4];
    };
    // src: BGRA (device) -> ARGB (the CPU's 32 bpc pixel)
    std::vector<F4> s;
    const bool haveSrc = src.devPtr && src.width > 0 && src.height > 0;
    if (haveSrc) {
        std::vector<char> raw((size_t)src.rowBytes * src.height);
        if (cu.dtoh(raw.data(), (CUdeviceptr)src.devPtr, raw.size())) return kGpuFailed;
        s.resize((size_t)src.width * src.height);
        for (int y = 0; y < src.height; y++) {
            const F4* row = (const F4*)(raw.data() + (size_t)y * src.rowBytes);
            for (int x = 0; x < src.width; x++) {
                const float* p = row[x].c;
                s[(size_t)y * src.width + x] = {{p[3], p[2], p[1], p[0]}};
            }
        }
    }
    std::vector<F4> d((size_t)dst.width * dst.height);
    RasterImage si = {haveSrc ? s.data() : nullptr, haveSrc ? src.width : 0, haveSrc ? src.height : 0,
                      (long)(src.width * sizeof(F4)), 32, src.originX, src.originY};
    RasterImage di = {d.data(), dst.width, dst.height, (long)(dst.width * sizeof(F4)), 32, dst.originX, dst.originY};
    di.screenShiftX = dst.screenShiftX;
    di.screenShiftY = dst.screenShiftY;
    std::vector<RasterCopy> cp(copies, copies + (n > 0 ? n : 0));
    unsigned hw = std::thread::hardware_concurrency();
    if (!RasterizeCopies(si, cp.data(), (int)cp.size(), dsX, dsY, di, hw ? (int)hw : 4, abort)) return kGpuAborted;
    // dst: ARGB -> BGRA rows at the device pitch
    std::vector<char> raw((size_t)dst.rowBytes * dst.height, 0);
    for (int y = 0; y < dst.height; y++) {
        F4* row = (F4*)(raw.data() + (size_t)y * dst.rowBytes);
        for (int x = 0; x < dst.width; x++) {
            const float* p = d[(size_t)y * dst.width + x].c;
            row[x] = {{p[3], p[2], p[1], p[0]}};
        }
    }
    return cu.htod((CUdeviceptr)dst.devPtr, raw.data(), raw.size()) ? kGpuFailed : kGpuOk;
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
