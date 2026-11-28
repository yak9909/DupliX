# Build DupliX.
#   pwsh tools/build.ps1                 # effect -> dev_plugins\DupliX.aex (AE loads it via the junction)
#   pwsh tools/build.ps1 -Target unit    # tests\unit -> build\unit\unit_tests.exe
#   pwsh tools/build.ps1 -Target dump    # tests\tools -> build\dump\duplix_dump.exe
#   pwsh tools/build.ps1 -Target gpu     # tests\gpu -> build\gpu\gpu_test.exe (runs on an NVIDIA GPU)
#   pwsh tools/build.ps1 -Target v2      # src\v2 -> dev_plugins\DupliX2.aex (DupliX 2, the prototype)
#   pwsh tools/build.ps1 -Target all
#   pwsh tools/build.ps1 -Target p0      # spike\P0Graph -> dev_plugins\DupliXP0.aex (design v4 P0 spike)
#   pwsh tools/build.ps1 -Target graph   # tests\graph -> build\graph\graph_test.exe (design v4's graph core)
#   pwsh tools/build.ps1 -Target dxg5    # tests\dxg5 -> build\dxg5\dxg5_test.exe (design v5's dx5 core)
param([ValidateSet("effect", "v2", "p0", "unit", "dump", "gpu", "graph", "dxg5", "all")][string]$Target = "effect",
      [string]$Configuration = "Release")  # the user's AE loads dev_plugins\DupliX.aex: Release unless debugging
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")

$vsPath = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
& (Join-Path $vsPath "Common7\Tools\Launch-VsDevShell.ps1") -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

function Invoke-Build([string]$proj, [string]$outDir) {
    $env:AE_PLUGIN_BUILD_DIR = $outDir
    msbuild $proj /nologo /v:minimal /m /p:Configuration=$Configuration /p:Platform=x64
    if ($LASTEXITCODE -ne 0) { throw "msbuild failed: $proj ($LASTEXITCODE)" }
}

$targets = if ($Target -eq "all") { @("effect", "v2", "unit", "dump", "gpu", "graph", "dxg5") } else { @($Target) }
# The CUDA kernel as PTX embedded in a header (src/gpu/fast_raster_ptx.h), for the effect and the GPU test.
if ($targets -contains "effect" -or $targets -contains "gpu") { & (Join-Path $PSScriptRoot "build_ptx.ps1") }
foreach ($t in $targets) {
    switch ($t) {
        "effect" {
            $dev = Join-Path $root "dev_plugins"
            if (-not (Test-Path $dev)) { New-Item -ItemType Directory $dev | Out-Null }
            Invoke-Build (Join-Path $root "src\effect\duplix.vcxproj") $dev
            Get-Item (Join-Path $dev "DupliX.aex") | Select-Object FullName, Length, LastWriteTime
        }
        "v2" {
            $dev = Join-Path $root "dev_plugins"
            if (-not (Test-Path $dev)) { New-Item -ItemType Directory $dev | Out-Null }
            Invoke-Build (Join-Path $root "src\v2\duplix2.vcxproj") $dev
            Get-Item (Join-Path $dev "DupliX2.aex") | Select-Object FullName, Length, LastWriteTime
        }
        "p0" {
            $dev = Join-Path $root "dev_plugins"
            Invoke-Build (Join-Path $root "spike\P0Graph\p0graph.vcxproj") $dev
            Get-Item (Join-Path $dev "DupliXP0.aex") | Select-Object FullName, Length, LastWriteTime
        }
        "unit" { Invoke-Build (Join-Path $root "tests\unit\unit_tests.vcxproj") (Join-Path $root "build\unit") }
        "dump" { Invoke-Build (Join-Path $root "tests\tools\duplix_dump.vcxproj") (Join-Path $root "build\dump") }
        "gpu" { Invoke-Build (Join-Path $root "tests\gpu\gpu_test.vcxproj") (Join-Path $root "build\gpu") }
        "graph" { Invoke-Build (Join-Path $root "tests\graph\graph_test.vcxproj") (Join-Path $root "build\graph") }
        "dxg5" { Invoke-Build (Join-Path $root "tests\dxg5\dxg5_test.vcxproj") (Join-Path $root "build\dxg5") }
    }
}
Remove-Item env:AE_PLUGIN_BUILD_DIR -ErrorAction SilentlyContinue
