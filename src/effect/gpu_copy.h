// GPU pass-through for the effect's own render: the output is a copy of the input (the copies are
// drawn by AE's classic 3D renderer, not here). Supports CUDA, OpenCL and DirectX 12 without SDKs:
// the CUDA driver API and OpenCL are loaded from nvcuda.dll / OpenCL.dll at run time.
#pragma once

#include "AE_Effect.h"
#include "AE_EffectPixelFormat.h"
#include "AE_EffectGPUSuites.h"

namespace rnd {

// PF_Cmd_GPU_DEVICE_SETUP: returns true if this framework is supported (then set
// PF_OutFlag2_SUPPORTS_GPU_RENDER_F32). *gpuData receives the per-device state (may stay null).
bool GpuDeviceSetup(PF_GPU_Framework fw, const PF_GPUDeviceInfo& info, void** gpuData);
// PF_Cmd_GPU_DEVICE_SETDOWN.
void GpuDeviceSetdown(PF_GPU_Framework fw, void* gpuData);
// Copies rows of the input GPU buffer to the output GPU buffer. Returns false on failure.
bool GpuCopy(PF_GPU_Framework fw, const PF_GPUDeviceInfo& info, const void* gpuData, void* src, void* dst,
             long srcRowBytes, long dstRowBytes, long height);

}  // namespace rnd
