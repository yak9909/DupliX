# Reproduces the orbit-null drawing-order report on a COPY of the user's project: renders orb_fm (fast) and
# orb_nm (normal) over 120 frames and prints fast-vs-normal per frame (check_orbit_order.py).
#   pwsh tests/ae/run_orbit_order.ps1 [-Aep "tests/ae/user/DupliX Test.aep"] [-SkipScene] [-Mfr OFF]
param([string]$Aep = "tests/ae/user/DupliX Test.aep", [switch]$SkipScene, [string]$Mfr = "OFF",
      [string]$Deg = "")  # a fixed orbit angle (one frame), e.g. -Deg 26
$env:RANDAMU_ORBIT_DEG = $Deg
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$outDir = Join-Path $root "build\test\orbit"
# The renders (~2 GB) go to %TEMP% (A: has little free space); the check reads them there.
$renderDir = Join-Path $env:TEMP "randamu_orbit"
New-Item -ItemType Directory -Force $renderDir | Out-Null
$env:RANDAMU_RENDER_DIR = $renderDir -replace '\\', '/'
if (-not $SkipScene) {
    # Never open the user's file itself: AE saves a modified project when it closes.
    $inDir = Join-Path $root "build\test\orbit_in"
    New-Item -ItemType Directory -Force $inDir | Out-Null
    $copy = Join-Path $inDir "user_copy.aep"
    Copy-Item (Resolve-Path (Join-Path $root $Aep)).Path $copy -Force
    $env:RANDAMU_USER_AEP = $copy -replace '\\', '/'
    & (Join-Path $PSScriptRoot "run_jsx.ps1") -Jsx tests/ae/orbit_order.jsx -Name orbit -TimeoutSec 400 -Expect "^done" |
        Select-String "orb_|EXCEPTION|TIMEOUT|EXPECT"
    Remove-Item env:RANDAMU_USER_AEP
    if ($LASTEXITCODE -ne 0) { exit 1 }
}
. (Join-Path $root "spike\tools\ae_guard.ps1")
$sf = "C:\Program Files\Adobe\Adobe After Effects 2025\Support Files"
Get-ChildItem $renderDir -Filter "r_*.tif" -ErrorAction SilentlyContinue | Remove-Item -Confirm:$false
$null = Repair-AeCrashState
$env:RANDAMU_LOG = Join-Path $outDir "plugin_aer.log"
if (Test-Path $env:RANDAMU_LOG) { Remove-Item $env:RANDAMU_LOG }
$env:RANDAMU_CACHE_SALT = [guid]::NewGuid().ToString()
$p = Start-Process -FilePath (Join-Path $sf "aerender.exe") -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $outDir "aerender.log") `
    -ArgumentList @("-project", "`"$(Join-Path $outDir 'orbit.aep')`"", "-mfr", $Mfr, "100", "-v", "ERRORS_AND_PROGRESS")
if (-not $p.WaitForExit(1200 * 1000)) { $p.Kill(); Write-Host "TIMEOUT"; exit 1 }
Remove-Item env:RANDAMU_LOG
& (Join-Path $root "spike\.venv\Scripts\python.exe") -I (Join-Path $PSScriptRoot "check_orbit_order.py") $renderDir
$rc = $LASTEXITCODE
Get-ChildItem $renderDir -Filter "cmp_*.png" | Copy-Item -Destination $outDir -Force
exit $rc
