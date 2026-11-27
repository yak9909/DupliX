#include "effect/gpu_copy.h"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <mutex>

#include "effect/duplix.h"

namespace rnd {

namespace {

// ---- CUDA driver API (subset of cuda.h)
typedef int CUresult;
typedef unsigned long long CUdeviceptr;
enum { CU_MEMORYTYPE_DEVICE = 2 };
struct CUDA_MEMCPY2D {
    size_t srcXInBytes, srcY;
    int srcMemoryType;
    const void* srcHost;
    CUdeviceptr srcDevice;
    void* srcArray;
    size_t srcPitch;
    size_t dstXInBytes, dstY;
    int dstMemoryType;
    void* dstHost;
    CUdeviceptr dstDevice;
    void* dstArray;
    size_t dstPitch;
    size_t WidthInBytes, Height;
};
typedef CUresult(__stdcall* CuCtxPush)(void* ctx);
typedef CUresult(__stdcall* CuCtxPop)(void** ctx);
typedef CUresult(__stdcall* CuMemcpy2DAsync)(const CUDA_MEMCPY2D* p, void* stream);

struct CudaApi {
    CuCtxPush push = nullptr;
    CuCtxPop pop = nullptr;
    CuMemcpy2DAsync copy = nullptr;
    bool ok = false;
};
const CudaApi& Cuda()
{
    static CudaApi api = [] {
        CudaApi a;
        HMODULE m = LoadLibraryA("nvcuda.dll");
        if (!m) return a;
        a.push = (CuCtxPush)GetProcAddress(m, "cuCtxPushCurrent_v2");
        a.pop = (CuCtxPop)GetProcAddress(m, "cuCtxPopCurrent_v2");
        a.copy = (CuMemcpy2DAsync)GetProcAddress(m, "cuMemcpy2DAsync_v2");
        a.ok = a.push && a.pop && a.copy;
        return a;
    }();
    return api;
}

// ---- OpenCL (subset of cl.h)
typedef int cl_int;
typedef cl_int(__stdcall* ClEnqueueCopyBufferRect)(void* queue, void* src, void* dst, const size_t* srcOrigin,
                                                   const size_t* dstOrigin, const size_t* region, size_t srcRowPitch,
                                                   size_t srcSlicePitch, size_t dstRowPitch, size_t dstSlicePitch,
                                                   unsigned numEvents, const void* events, void* event);
ClEnqueueCopyBufferRect OpenCl()
{
    static ClEnqueueCopyBufferRect fn = [] {
        HMODULE m = LoadLibraryA("OpenCL.dll");
        return m ? (ClEnqueueCopyBufferRect)GetProcAddress(m, "clEnqueueCopyBufferRect") : nullptr;
    }();
    return fn;
}

// ---- DirectX 12: one command list + fence per device, serialised (MFR may render concurrently)
struct DxState {
    std::mutex mu;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = nullptr;
    UINT64 value = 0;
};

void DxRelease(DxState* s)
{
    if (!s) return;
    if (s->list) s->list->Release();
    if (s->alloc) s->alloc->Release();
    if (s->fence) s->fence->Release();
    if (s->event) CloseHandle(s->event);
    delete s;
}

DxState* DxCreate(const PF_GPUDeviceInfo& info)
{
    auto* s = new DxState;
    s->device = (ID3D12Device*)info.devicePV;
    s->queue = (ID3D12CommandQueue*)info.command_queuePV;
    if (!s->device || !s->queue) {
        DxRelease(s);
        return nullptr;
    }
    D3D12_COMMAND_LIST_TYPE type = s->queue->GetDesc().Type;
    if (FAILED(s->device->CreateCommandAllocator(type, IID_PPV_ARGS(&s->alloc))) ||
        FAILED(s->device->CreateCommandList(0, type, s->alloc, nullptr, IID_PPV_ARGS(&s->list))) ||
        FAILED(s->list->Close()) || FAILED(s->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s->fence))) ||
        !(s->event = CreateEventA(nullptr, FALSE, FALSE, nullptr))) {
        DxRelease(s);
        return nullptr;
    }
    return s;
}

bool DxCopy(DxState* s, ID3D12Resource* src, ID3D12Resource* dst, long srcRow, long dstRow, long height)
{
    if (!s || !src || !dst) return false;
    std::lock_guard<std::mutex> lk(s->mu);
    if (FAILED(s->alloc->Reset()) || FAILED(s->list->Reset(s->alloc, nullptr))) return false;
    // AE's GPU worlds are plain buffers in the COMMON state (the SDK sample binds them without
    // barriers); copies promote them implicitly and they decay back when the list completes.
    long w = std::min(srcRow, dstRow);
    if (srcRow == dstRow)
        s->list->CopyBufferRegion(dst, 0, src, 0, (UINT64)srcRow * height);
    else
        for (long y = 0; y < height; y++) s->list->CopyBufferRegion(dst, (UINT64)y * dstRow, src, (UINT64)y * srcRow, w);
    if (FAILED(s->list->Close())) return false;
    ID3D12CommandList* lists[] = {s->list};
    s->queue->ExecuteCommandLists(1, lists);
    // Wait: AE reads the output after this command returns and the next render resets the list.
    if (FAILED(s->queue->Signal(s->fence, ++s->value))) return false;
    if (s->fence->GetCompletedValue() < s->value) {
        if (FAILED(s->fence->SetEventOnCompletion(s->value, s->event))) return false;
        WaitForSingleObject(s->event, INFINITE);
    }
    return true;
}

}  // namespace

bool GpuDeviceSetup(PF_GPU_Framework fw, const PF_GPUDeviceInfo& info, void** gpuData)
{
    *gpuData = nullptr;
    switch (fw) {
    case PF_GPU_Framework_CUDA:
        return Cuda().ok;
    case PF_GPU_Framework_OPENCL:
        return OpenCl() != nullptr;
    case PF_GPU_Framework_DIRECTX:
        *gpuData = DxCreate(info);
        return *gpuData != nullptr;
    default:
        return false;
    }
}

void GpuDeviceSetdown(PF_GPU_Framework fw, void* gpuData)
{
    if (fw == PF_GPU_Framework_DIRECTX) DxRelease((DxState*)gpuData);
}

bool GpuCopy(PF_GPU_Framework fw, const PF_GPUDeviceInfo& info, const void* gpuData, void* src, void* dst,
             long srcRowBytes, long dstRowBytes, long height)
{
    long w = std::min(srcRowBytes, dstRowBytes);
    if (!src || !dst || w <= 0 || height <= 0) return false;
    switch (fw) {
    case PF_GPU_Framework_CUDA: {
        const CudaApi& cu = Cuda();
        if (!cu.ok) return false;
        CUDA_MEMCPY2D p = {};
        p.srcMemoryType = p.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        p.srcDevice = (CUdeviceptr)src;
        p.dstDevice = (CUdeviceptr)dst;
        p.srcPitch = (size_t)srcRowBytes;
        p.dstPitch = (size_t)dstRowBytes;
        p.WidthInBytes = (size_t)w;
        p.Height = (size_t)height;
        if (cu.push(info.contextPV)) return false;
        CUresult r = cu.copy(&p, info.command_queuePV);  // on AE's stream: ordered before AE's next use
        void* popped = nullptr;
        cu.pop(&popped);
        return r == 0;
    }
    case PF_GPU_Framework_OPENCL: {
        ClEnqueueCopyBufferRect fn = OpenCl();
        if (!fn) return false;
        size_t origin[3] = {0, 0, 0};
        size_t region[3] = {(size_t)w, (size_t)height, 1};
        return fn(info.command_queuePV, src, dst, origin, origin, region, (size_t)srcRowBytes, 0,
                  (size_t)dstRowBytes, 0, 0, nullptr, nullptr) == 0;
    }
    case PF_GPU_Framework_DIRECTX:
        return DxCopy((DxState*)gpuData, (ID3D12Resource*)src, (ID3D12Resource*)dst, srcRowBytes, dstRowBytes,
                      height);
    default:
        return false;
    }
}

}  // namespace rnd
