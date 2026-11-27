# P0 graph evaluation benchmark (spike/P0Bench/graph_bench.cpp): builds with cl /O2 and runs it.
#   pwsh tools/bench_p0.ps1
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$vsPath = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
& (Join-Path $vsPath "Common7\Tools\Launch-VsDevShell.ps1") -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$out = Join-Path $root "build\p0bench"
New-Item -ItemType Directory -Force $out | Out-Null
Push-Location $out
try {
    cl /nologo /O2 /EHsc /std:c++17 /fp:fast (Join-Path $root "spike\P0Bench\graph_bench.cpp") /Fe:graph_bench.exe | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "cl failed" }
    & .\graph_bench.exe
} finally { Pop-Location }
