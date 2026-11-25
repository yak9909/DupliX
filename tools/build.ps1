# Build randamu.
#   pwsh tools/build.ps1                 # effect -> dev_plugins\randamu.aex (AE loads it via the junction)
#   pwsh tools/build.ps1 -Target unit    # tests\unit -> build\unit\unit_tests.exe
#   pwsh tools/build.ps1 -Target dump    # tests\tools -> build\dump\randamu_dump.exe
#   pwsh tools/build.ps1 -Target gpu     # tests\gpu -> build\gpu\gpu_test.exe (runs on an NVIDIA GPU)
#   pwsh tools/build.ps1 -Target all
param([ValidateSet("effect", "unit", "dump", "gpu", "all")][string]$Target = "effect",
      [string]$Configuration = "Release")  # the user's AE loads dev_plugins\randamu.aex: Release unless debugging
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")

$vsPath = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
& (Join-Path $vsPath "Common7\Tools\Launch-VsDevShell.ps1") -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

function Invoke-Build([string]$proj, [string]$outDir) {
    $env:AE_PLUGIN_BUILD_DIR = $outDir
    msbuild $proj /nologo /v:minimal /m /p:Configuration=$Configuration /p:Platform=x64
    if ($LASTEXITCODE -ne 0) { throw "msbuild failed: $proj ($LASTEXITCODE)" }
}

$targets = if ($Target -eq "all") { @("effect", "unit", "dump", "gpu") } else { @($Target) }
# The CUDA kernel as PTX embedded in a header (src/gpu/fast_raster_ptx.h), for the effect and the GPU test.
if ($targets -contains "effect" -or $targets -contains "gpu") { & (Join-Path $PSScriptRoot "build_ptx.ps1") }
foreach ($t in $targets) {
    switch ($t) {
        "effect" {
            $dev = Join-Path $root "dev_plugins"
            if (-not (Test-Path $dev)) { New-Item -ItemType Directory $dev | Out-Null }
            Invoke-Build (Join-Path $root "src\effect\randamu.vcxproj") $dev
            Get-Item (Join-Path $dev "randamu.aex") | Select-Object FullName, Length, LastWriteTime
        }
        "unit" { Invoke-Build (Join-Path $root "tests\unit\unit_tests.vcxproj") (Join-Path $root "build\unit") }
        "dump" { Invoke-Build (Join-Path $root "tests\tools\randamu_dump.vcxproj") (Join-Path $root "build\dump") }
        "gpu" { Invoke-Build (Join-Path $root "tests\gpu\gpu_test.vcxproj") (Join-Path $root "build\gpu") }
    }
}
Remove-Item env:AE_PLUGIN_BUILD_DIR -ErrorAction SilentlyContinue
