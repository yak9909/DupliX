#include "gpu/gpu_raster.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#include "core/tiles.h"
#include "gpu/ae_span_ptx.h"
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
    // for the host-image path (merge): optional, checked where used
    CUresult(__stdcall* primaryRetain)(CUcontext*, CUdevice);
    CUresult(__stdcall* streamCreate)(CUstream*, unsigned);
    CUresult(__stdcall* memAllocHost)(void**, size_t);
    CUresult(__stdcall* memFreeHost)(void*);
    CUresult(__stdcall* dtohAsync)(void*, CUdeviceptr, size_t, CUstream);
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
        a.primaryRetain = (decltype(a.primaryRetain))get("cuDevicePrimaryCtxRetain");
        a.streamCreate = (decltype(a.streamCreate))get("cuStreamCreate");
        a.memAllocHost = (decltype(a.memAllocHost))get("cuMemAllocHost_v2");
        a.memFreeHost = (decltype(a.memFreeHost))get("cuMemFreeHost");
        a.dtohAsync = (decltype(a.dtohAsync))get("cuMemcpyDtoHAsync_v2");
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
    CUfunction toF = nullptr, fromF = nullptr;  // the merge's pixel conversions (null: that path draws on the CPU)
};
std::mutex g_moduleMutex;
std::map<CUcontext, Loaded> g_functions;

// The calling thread has ctx current.
const Loaded& LoadedFor(CUcontext ctx)
{
    std::lock_guard<std::mutex> lk(g_moduleMutex);
    auto it = g_functions.find(ctx);
    if (it != g_functions.end()) return it->second;
    const Api& cu = Cuda();
    Loaded l;
    CUresult r = cu.moduleLoadData(&l.mod, kFastRasterPtx);
    if (!r) r = cu.moduleGetFunction(&l.fn, l.mod, "FastRaster");
    if (r) {
        if (l.mod && cu.moduleUnload) cu.moduleUnload(l.mod);
        l = Loaded{};
        g_failed = true;
        GpuLog("gpu raster: kernel load failed (CUresult %d): the fast mode draws on the CPU", (int)r);
    } else if (cu.moduleGetFunction(&l.toF, l.mod, "ArgbToBgraF") || cu.moduleGetFunction(&l.fromF, l.mod, "BgraFToArgb")) {
        l.toF = l.fromF = nullptr;
    }
    return g_functions[ctx] = l;  // std::map: the reference stays valid until the entry is erased
}
CUfunction FunctionFor(CUcontext ctx) { return LoadedFor(ctx).fn; }

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
    const GpuAllocator* al = nullptr;
    explicit DevBuf(const GpuAllocator* a) : al(a) {}
    ~DevBuf()
    {
        if (!p) return;
        if (al) al->free((void*)p);
        else Cuda().memFree(p);
    }
    bool Put(const void* host, size_t bytes, CUstream stream)
    {
        // never a zero-sized allocation; nothing to copy then
        const size_t sz = bytes ? bytes : 4;
        if (al) p = (CUdeviceptr)al->alloc(sz);
        else if (Cuda().memAlloc(&p, sz)) p = 0;
        return p && (!host || !bytes || Cuda().htodAsync(p, host, bytes, stream) == 0);
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
                             const RasterCopy* copies, int n, double dsX, double dsY, const std::function<bool()>& abort,
                             const GpuAllocator* alloc)
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
        DevBuf bc(alloc), bs(alloc), bi(alloc);  // freed before the context is popped
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

// ---- host images (the merge's group textures: AE's CPU worlds)
namespace {

// One context (the device's primary one), one non-blocking stream and grow-only device buffers, used by one
// thread at a time (render threads take turns: the GPU part is short). AE's pixels travel as they are (8 bpc is a
// quarter of the float size) and are converted on the GPU.
struct HostPath {
    std::mutex mu;
    bool tried = false;
    CUcontext ctx = nullptr;
    CUstream stream = nullptr;
    CUdeviceptr rawSrc = 0, fSrc = 0, fDst = 0, rawDst = 0;
    size_t rawSrcCap = 0, fSrcCap = 0, fDstCap = 0, rawDstCap = 0;
};
HostPath g_host;

// The calling thread has the context current.
bool GrowDev(CUdeviceptr* p, size_t* cap, size_t need)
{
    if (*cap >= need && *p) return true;
    if (*p) Cuda().memFree(*p);
    *p = 0, *cap = 0;
    if (Cuda().memAlloc(p, need ? need : 4)) return *p = 0, false;
    *cap = need;
    return true;
}

bool LaunchConvert(CUfunction fn, CUstream stream, void** args, int w, int h)
{
    return Cuda().launch(fn, (unsigned)((w + 15) / 16), (unsigned)((h + 15) / 16), 1, 16, 16, 1, 0, stream, args,
                         nullptr) == 0;
}

}  // namespace

GpuRasterStatus GpuRasterizeHost(const RasterImage& src, const RasterCopy* copies, int n, double dsX, double dsY,
                                 RasterImage& dst, double* msOut)
{
    const Api& cu = Cuda();
    if (!cu.ok || g_failed.load() || !cu.primaryRetain || !cu.streamCreate || !cu.dtohAsync) return kGpuFailed;
    if (!dst.data || dst.width <= 0 || dst.height <= 0) return kGpuFailed;
    std::lock_guard<std::mutex> lk(g_host.mu);
    if (!g_host.tried) {
        g_host.tried = true;
        CUdevice dev = 0;
        if (cu.init(0) || cu.deviceGet(&dev, 0) || cu.primaryRetain(&g_host.ctx, dev)) {
            g_host.ctx = nullptr;
            GpuLog("gpu raster: no context for the merge (the merge draws on the CPU)");
        } else {
            CtxScope scope(g_host.ctx);
            if (!scope.pushed || cu.streamCreate(&g_host.stream, 1 /* CU_STREAM_NON_BLOCKING */)) {
                g_host.ctx = nullptr;
                GpuLog("gpu raster: no stream for the merge (the merge draws on the CPU)");
            }
        }
    }
    if (!g_host.ctx) return kGpuFailed;
    auto now = [] {
        LARGE_INTEGER v;
        QueryPerformanceCounter(&v);
        return v.QuadPart;
    };
    long long t[5];
    t[0] = now();
    CtxScope scope(g_host.ctx);
    if (!scope.pushed) return kGpuFailed;
    const Loaded fns = LoadedFor(g_host.ctx);  // a copy: an entry may be erased by AE's setdown of the same context
    if (!fns.fn || !fns.toF || !fns.fromF) return kGpuFailed;
    const bool haveSrc = src.data && src.width > 0 && src.height > 0 && n > 0;
    const size_t srcRaw = haveSrc ? (size_t)src.rowBytes * src.height : 0;
    const size_t srcF = haveSrc ? (size_t)src.width * src.height * 16 : 0;
    const size_t dstF = (size_t)dst.width * dst.height * 16, dstRaw = (size_t)dst.rowBytes * dst.height;
    if ((haveSrc && (!GrowDev(&g_host.rawSrc, &g_host.rawSrcCap, srcRaw) || !GrowDev(&g_host.fSrc, &g_host.fSrcCap, srcF))) ||
        !GrowDev(&g_host.fDst, &g_host.fDstCap, dstF) || !GrowDev(&g_host.rawDst, &g_host.rawDstCap, dstRaw)) {
        GpuLog("gpu raster: merge buffers not allocated (%zu + %zu bytes): this frame on the CPU", srcRaw + srcF, dstF + dstRaw);
        return kGpuFailed;
    }
    CUstream stream = g_host.stream;
    if (haveSrc) {
        if (cu.htodAsync(g_host.rawSrc, src.data, srcRaw, stream)) return kGpuFailed;
        int pitchB = (int)src.rowBytes, w = src.width, h = src.height, bpc = src.bpc, fPitch = src.width;
        CUdeviceptr in = g_host.rawSrc, out = g_host.fSrc;
        void* a[] = {&in, &pitchB, &w, &h, &bpc, &out, &fPitch};
        if (!LaunchConvert(fns.toF, stream, a, w, h)) return g_failed = true, kGpuFailed;
    }
    cu.streamSync(stream);
    t[1] = now();
    GpuImage gs = {haveSrc ? (void*)g_host.fSrc : nullptr, haveSrc ? src.width : 0, haveSrc ? src.height : 0,
                   (long)(haveSrc ? src.width * 16 : 0), src.originX, src.originY};
    GpuImage gd = {(void*)g_host.fDst, dst.width, dst.height, (long)(dst.width * 16), dst.originX, dst.originY};
    gd.screenShiftX = dst.screenShiftX;
    gd.screenShiftY = dst.screenShiftY;
    GpuRasterStatus st = GpuRasterize(g_host.ctx, stream, gs, gd, copies, haveSrc ? n : 0, dsX, dsY);
    if (st != kGpuOk) return st;
    t[2] = now();
    {
        int fPitch = dst.width, w = dst.width, h = dst.height, bpc = dst.bpc, pitchB = (int)dst.rowBytes;
        CUdeviceptr in = g_host.fDst, out = g_host.rawDst;
        void* a[] = {&in, &fPitch, &w, &h, &bpc, &out, &pitchB};
        if (!LaunchConvert(fns.fromF, stream, a, w, h)) return g_failed = true, kGpuFailed;
    }
    cu.streamSync(stream);
    t[3] = now();
    if (cu.dtohAsync(dst.data, g_host.rawDst, dstRaw, stream) || cu.streamSync(stream)) {
        GpuLog("gpu raster: merge download failed: this frame on the CPU");
        return kGpuFailed;
    }
    t[4] = now();
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    auto ms = [&](int a, int b) { return (t[b] - t[a]) * 1000.0 / qf.QuadPart; };
    if (msOut) *msOut = ms(0, 4);
    static std::atomic<int> logged{0};
    if (++logged <= 12)
        GpuLog("gpu raster: merge %dx%d bpc %d: upload+convert %.1f, draw %.1f, convert %.1f, download %.1f ms", dst.width,
               dst.height, dst.bpc, ms(0, 1), ms(1, 2), ms(2, 3), ms(3, 4));
    return kGpuOk;
}

// ---- AE's span drawing (classic 3D copies drawn by DupliX with AE's arithmetic)
namespace {

struct AeHost {
    std::mutex mu;
    bool tried = false;
    CUcontext ctx = nullptr;
    CUstream stream = nullptr;
    CUmodule mod = nullptr;
    CUfunction fn = nullptr;
    CUdeviceptr tex = 0, rcp = 0, region = 0, rows = 0;
    size_t texCap = 0, regionCap = 0, rowsCap = 0;
    unsigned long long texKey = ~0ull;
    std::vector<unsigned char> shadow;  // the pixels of the texture on the GPU (host copy)
    int shadowW = 0, shadowH = 0, shadowRb = 0;
    float rcpHost[256] = {};
    bool rcpSet = false;
    unsigned char* stage = nullptr;  // pinned: the region on its way to and from the GPU
    size_t stageCap = 0;
};
AeHost g_ae;

bool GrowHost(unsigned char** p, size_t* cap, size_t need)
{
    if (*cap >= need && *p) return true;
    if (*p) Cuda().memFreeHost(*p);
    *p = nullptr, *cap = 0;
    void* q = nullptr;
    if (Cuda().memAllocHost(&q, need ? need : 4)) return false;
    *p = (unsigned char*)q, *cap = need;
    return true;
}

}  // namespace

bool GpuAeSpans(const GpuAeSpanJob& j, double* msOut, GpuAeSpanTimes* times)
{
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    auto nowMs = [&]() {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return (double)t.QuadPart * 1000.0 / (double)qf.QuadPart;
    };
    const Api& cu = Cuda();
    if (!cu.ok || g_failed.load() || !cu.primaryRetain || !cu.streamCreate || !cu.dtohAsync || !cu.memAllocHost) return false;
    if (j.nRows <= 0 || !j.target || !j.tex.data) return false;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    std::lock_guard<std::mutex> lk(g_ae.mu);
    if (!g_ae.tried) {
        g_ae.tried = true;
        CUdevice dev = 0;
        if (cu.init(0) || cu.deviceGet(&dev, 0) || cu.primaryRetain(&g_ae.ctx, dev)) {
            g_ae.ctx = nullptr;
        } else {
            CtxScope scope(g_ae.ctx);
            CUresult r = !scope.pushed ? 1 : cu.streamCreate(&g_ae.stream, 1 /* CU_STREAM_NON_BLOCKING */);
            if (!r) r = cu.moduleLoadData(&g_ae.mod, kAeSpanPtx);
            if (!r) r = cu.moduleGetFunction(&g_ae.fn, g_ae.mod, "AeSpanRows");
            if (r) {
                GpuLog("gpu ae span: not available (CUresult %d): copies drawn on the CPU", (int)r);
                g_ae.ctx = nullptr;
            }
        }
    }
    if (!g_ae.ctx) return false;
    CtxScope scope(g_ae.ctx);
    if (!scope.pushed) return false;
    CUstream st = g_ae.stream;
    // the rows' bounding box
    int y0 = 1 << 30, y1 = -(1 << 30), x0 = 1 << 30, x1 = -(1 << 30);
    for (int i = 0; i < j.nRows; i++) {
        const int* r = j.rows + 3 * i;
        if (r[2] <= r[1]) continue;
        y0 = std::min(y0, r[0]), y1 = std::max(y1, r[0] + 1), x0 = std::min(x0, r[1]), x1 = std::max(x1, r[2]);
    }
    if (y1 <= y0 || x1 <= x0) return true;  // nothing to draw
    const int pitch = (x1 - x0) * 4;
    const size_t regionBytes = (size_t)pitch * (y1 - y0), rowsBytes = (size_t)j.nRows * 12;
    const size_t texBytes = (size_t)j.tex.rowbytes * j.tex.height;
    if (!GrowDev(&g_ae.region, &g_ae.regionCap, regionBytes) || !GrowDev(&g_ae.rows, &g_ae.rowsCap, rowsBytes) ||
        !GrowHost(&g_ae.stage, &g_ae.stageCap, regionBytes + rowsBytes)) {
        GpuLog("gpu ae span: buffers not allocated (%zu bytes): this copy on the CPU", regionBytes);
        return false;
    }
    if (times) times->regionPx = (long long)(x1 - x0) * (y1 - y0);
    if (g_ae.texKey != j.texKey || !g_ae.tex) {
        const double ta = nowMs();
        // the same pixels as the texture on the GPU (the copies of one layer share one texture): no upload
        // (each row's pixels only: the padding after them differs between textures and is never read)
        bool same = g_ae.tex && g_ae.shadow.size() == texBytes && g_ae.shadowW == j.tex.width &&
                    g_ae.shadowH == j.tex.height && g_ae.shadowRb == j.tex.rowbytes;
        for (int y = 0; same && y < j.tex.height; y++)
            same = !std::memcmp(g_ae.shadow.data() + (size_t)y * j.tex.rowbytes, j.tex.data + (size_t)y * j.tex.rowbytes,
                                (size_t)j.tex.width * 4);
        if (!same) {
            if (!GrowDev(&g_ae.tex, &g_ae.texCap, texBytes) || cu.htod(g_ae.tex, j.tex.data, texBytes)) {
                g_ae.texKey = ~0ull;
                g_ae.shadow.clear();
                return false;
            }
            g_ae.shadow.assign(j.tex.data, j.tex.data + texBytes);
            g_ae.shadowW = j.tex.width, g_ae.shadowH = j.tex.height, g_ae.shadowRb = j.tex.rowbytes;
        }
        g_ae.texKey = j.texKey;
        if (times) times->texMs = nowMs() - ta;
    }
    if (!g_ae.rcpSet || std::memcmp(g_ae.rcpHost, j.rcp, sizeof(g_ae.rcpHost))) {
        if (!g_ae.rcp && cu.memAlloc(&g_ae.rcp, sizeof(g_ae.rcpHost))) return false;
        if (cu.htod(g_ae.rcp, j.rcp, sizeof(g_ae.rcpHost))) return false;
        std::memcpy(g_ae.rcpHost, j.rcp, sizeof(g_ae.rcpHost));
        g_ae.rcpSet = true;
    }
    // the region and the rows into pinned memory, then one copy each way
    const double tsA = nowMs();
    for (int y = y0; y < y1; y++)
        std::memcpy(g_ae.stage + (size_t)(y - y0) * pitch, j.target + (ptrdiff_t)y * j.targetRowbytes + 4 * (ptrdiff_t)x0, pitch);
    std::memcpy(g_ae.stage + regionBytes, j.rows, rowsBytes);
    const double tg = nowMs();
    if (cu.htodAsync(g_ae.region, g_ae.stage, regionBytes, st) ||
        cu.htodAsync(g_ae.rows, g_ae.stage + regionBytes, rowsBytes, st))
        return false;
    AeSpanCore core = j.core;
    CUdeviceptr texD = g_ae.tex, rcpD = g_ae.rcp, regD = g_ae.region, rowsD = g_ae.rows;
    int texRb = j.tex.rowbytes, texW = j.tex.width, texH = j.tex.height, n = j.nRows, ox = x0, oy = y0, pt = pitch;
    void* args[] = {&core, &texD, &texRb, &texW, &texH, &rcpD, &regD, &pt, &ox, &oy, &rowsD, &n};
    if (cu.launch(g_ae.fn, (unsigned)n, 1, 1, 64, 1, 1, 0, st, args, nullptr)) {
        g_failed = true;
        GpuLog("gpu ae span: launch failed: copies drawn on the CPU from now on");
        return false;
    }
    if (cu.dtohAsync(g_ae.stage, g_ae.region, regionBytes, st) || cu.streamSync(st)) {
        g_failed = true;
        GpuLog("gpu ae span: the kernel failed: copies drawn on the CPU from now on");
        return false;
    }
    const double tsB = nowMs();
    for (int y = y0; y < y1; y++)
        std::memcpy(j.target + (ptrdiff_t)y * j.targetRowbytes + 4 * (ptrdiff_t)x0, g_ae.stage + (size_t)(y - y0) * pitch, pitch);
    if (times) times->gpuMs = tsB - tg, times->stageMs = (tg - tsA) + (nowMs() - tsB);
    if (msOut) {
        LARGE_INTEGER t1, f;
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        *msOut = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    }
    return true;
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
