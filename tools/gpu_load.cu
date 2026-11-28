// Loads the GPU like the other processes that sometimes hold it (11/27-28: 87-100% and 11.6 / 12 GB), to reproduce
// AE's behaviour under that contention on purpose. Build and run: pwsh tools/gpu_load.ps1 -Seconds 600 -GB 8
//   gpu_load.exe <seconds> <GB of memory to hold>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <vector>
#include <cuda_runtime.h>

__global__ void Spin(float* p, size_t n, int iters)
{
    size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x;
    if (i >= n) return;
    float v = p[i];
    for (int k = 0; k < iters; k++) v = v * 1.000001f + 0.5f;
    p[i] = v;
}

int main(int argc, char** argv)
{
    const double seconds = argc > 1 ? atof(argv[1]) : 60;
    const double gb = argc > 2 ? atof(argv[2]) : 8;
    std::vector<void*> blocks;
    for (double held = 0; held + 0.25 <= gb; held += 0.25) {  // 256 MB at a time, as much as fits
        void* p = nullptr;
        if (cudaMalloc(&p, 256u << 20) != cudaSuccess) break;
        cudaMemset(p, 0, 256u << 20);
        blocks.push_back(p);
    }
    printf("holding %.2f GB, busy for %.0f s\n", blocks.size() * 0.25, seconds);
    fflush(stdout);
    if (blocks.empty()) return 1;
    const size_t n = (256u << 20) / sizeof(float);
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        Spin<<<(unsigned)((n + 255) / 256), 256>>>((float*)blocks[0], n, 2000);
        cudaDeviceSynchronize();
    }
    for (void* p : blocks) cudaFree(p);
    return 0;
}
