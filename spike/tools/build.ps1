# THROWAWAY SPIKE build script.
# Usage: pwsh spike\tools\build.ps1 [-Configuration Debug|Release]
param([string]$Configuration = "Debug")
$ErrorActionPreference = "Stop"

$vsPath = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
$devShell = Join-Path $vsPath "Common7\Tools\Launch-VsDevShell.ps1"
& $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

$proj = Join-Path $PSScriptRoot "..\DupliXSpike\DupliXSpike.vcxproj"
# AE sees this folder through the junction MediaCore\duplix_dev -> dev_plugins
if (-not $env:AE_PLUGIN_BUILD_DIR) {
    $env:AE_PLUGIN_BUILD_DIR = (Resolve-Path (Join-Path $PSScriptRoot "..\..\dev_plugins")).Path
}
msbuild $proj /nologo /v:minimal /m /p:Configuration=$Configuration /p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw "msbuild failed ($LASTEXITCODE)" }

$out = Join-Path $PSScriptRoot "..\build\out\DupliXSpike.aex"
if ($env:AE_PLUGIN_BUILD_DIR) { $out = Join-Path $env:AE_PLUGIN_BUILD_DIR "DupliXSpike.aex" }
Get-Item $out | Select-Object FullName, Length, LastWriteTime
