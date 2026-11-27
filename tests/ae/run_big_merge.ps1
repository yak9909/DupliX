# Large images as the copied layer: the merge on (DupliX on the GPU, groups, clusters in AE's M buffer) against the
# merge off (AE draws every copy), viewer frames compared pixel for pixel (cmp_dirs.py: exit 1 if any differs).
#   pwsh tests/ae/run_big_merge.ps1 [-Variants "pv_bigS380,pv_bigS3271"] [-Counts "380,3271"] [-Image build/test/big/big1920.png]
# The image is made by make_big_png.py if missing. The merge off draws thousands of large copies slowly: -IntervalMs
# gives the viewer time for each frame (a frame not drawn in time is missing and reported).
param([string]$Variants = "pv_bigS380,pv_bigL380,pv_bigS3271", [string]$Counts = "380,3271",
      [string]$Image = "build/test/big/big1920.png", [string]$Aep = "tests/ae/user/DupliX Test C.aep",
      [string]$Layer = "flower_blue.png", [int]$Frames = 4, [int]$IntervalMs = 15000, [switch]$Nest)
# -Nest: the pv_nest<n> variants too (the copied layer a precomp with classic 3D layers inside: a frame drawn inside a
# frame), e.g. -Nest -Variants pv_nest380 -Counts 380
$env:DUPLIX_PV_NEST = if ($Nest) { "1" } else { "" }
# (11/27: at 8000 ms the merge off of pv_bigS3271 drew only 2 of 4 frames; cmp_dirs.py counts a missing frame as differing)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
Set-Location $root
if (-not (Test-Path $Image)) { python (Join-Path $PSScriptRoot "make_big_png.py") $Image }
$env:DUPLIX_PV_COUNTS = $Counts
$env:DUPLIX_PV_BIG = (Resolve-Path $Image).Path -replace '\\', '/'
$cmp = Join-Path $root "build\test\big_merge"
Remove-Item -Recurse -Force $cmp -ErrorAction SilentlyContinue
foreach ($m in "1", "0") {
    $env:DUPLIX_PV_BIG_MERGE = $m
    pwsh (Join-Path $PSScriptRoot "run_preview.ps1") -Aep $Aep -Layer $Layer -Frames $Frames -IntervalMs $IntervalMs -Variants $Variants *>&1 | Select-Object -Last 6
    $d = Join-Path $cmp "merge$m"
    New-Item -ItemType Directory -Force $d | Out-Null
    Copy-Item (Join-Path $root "build\test\preview\frame_*.png") $d
    Copy-Item (Join-Path $root "build\test\preview\plugin.log") (Join-Path $d "plugin.log")
}
Remove-Item env:DUPLIX_PV_BIG_MERGE
& (Join-Path $root "spike\.venv\Scripts\python.exe") -I (Join-Path $PSScriptRoot "cmp_dirs.py") (Join-Path $cmp "merge1") (Join-Path $cmp "merge0")
$code = $LASTEXITCODE
if ($code -eq 0) { "BIG MERGE ALL PASS" } else { "BIG MERGE FAIL" }
exit $code
