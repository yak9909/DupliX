# Builds tools/gpu_load.cu (once) and runs it in the background: the GPU busy and most of its memory held, as when
# another process holds it. Returns the process (stop it with Stop-Process, or let -Seconds run out).
#   $p = pwsh tools/gpu_load.ps1 -Seconds 600 -GB 8   (or dot-source and call Start-GpuLoad)
param([double]$Seconds = 600, [double]$GB = 8)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$exe = Join-Path $root "build\gpu_load\gpu_load.exe"
if (-not (Test-Path $exe) -or (Get-Item $exe).LastWriteTime -lt (Get-Item (Join-Path $PSScriptRoot "gpu_load.cu")).LastWriteTime) {
    $vsPath = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
    & (Join-Path $vsPath "Common7\Tools\Launch-VsDevShell.ps1") -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
    New-Item -ItemType Directory -Force (Split-Path $exe) | Out-Null
    # the same switches as build_ptx.ps1 (the installed VS is newer than CUDA 12.2 knows)
    & "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.2\bin\nvcc.exe" -O2 -allow-unsupported-compiler `
        -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH -o $exe (Join-Path $PSScriptRoot "gpu_load.cu") | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "nvcc failed" }
}
Start-Process -FilePath $exe -ArgumentList "$Seconds $GB" -WindowStyle Hidden -PassThru
