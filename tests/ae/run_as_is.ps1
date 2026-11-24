# Renders a COPY of the user's project as it is, every DupliX in fast vs normal mode (tests/ae/as_is.jsx),
# with aerender and through the viewer path (saveFrameToPng, full and half resolution), and compares.
#   pwsh tests/ae/run_as_is.ps1 [-Aep "tests/ae/user/DupliX Test.aep"]
param([string]$Aep = "tests/ae/user/DupliX Test.aep")
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$outDir = Join-Path $root "build\test\as_is"
$renderDir = Join-Path $env:TEMP "randamu_as_is"
New-Item -ItemType Directory -Force $renderDir | Out-Null
Get-ChildItem $renderDir -Filter "r_*.tif" -ErrorAction SilentlyContinue | Remove-Item -Confirm:$false
$env:RANDAMU_RENDER_DIR = $renderDir -replace '\\', '/'
# Never open the user's file itself: AE saves a modified project when it closes.
$inDir = Join-Path $root "build\test\as_is_in"
New-Item -ItemType Directory -Force $inDir | Out-Null
$copy = Join-Path $inDir "user_copy.aep"
Copy-Item (Resolve-Path (Join-Path $root $Aep)).Path $copy -Force
$env:RANDAMU_USER_AEP = $copy -replace '\\', '/'
$env:RANDAMU_CACHE_SALT = [guid]::NewGuid().ToString()
& (Join-Path $PSScriptRoot "run_jsx.ps1") -Jsx tests/ae/as_is.jsx -Name as_is -TimeoutSec 400 -Expect "^done" | Out-Null
if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $outDir "jsx.log"); exit 1 }
Get-Content (Join-Path $outDir "jsx.log")
. (Join-Path $root "spike\tools\ae_guard.ps1")
$null = Repair-AeCrashState
$sf = "C:\Program Files\Adobe\Adobe After Effects 2025\Support Files"
$env:RANDAMU_LOG = Join-Path $outDir "plugin_aer.log"
if (Test-Path $env:RANDAMU_LOG) { Remove-Item $env:RANDAMU_LOG }
$p = Start-Process -FilePath (Join-Path $sf "aerender.exe") -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $outDir "aerender.log") `
    -ArgumentList @("-project", "`"$(Join-Path $outDir 'as_is.aep')`"", "-mfr", "OFF", "100", "-v", "ERRORS_AND_PROGRESS")
if (-not $p.WaitForExit(900 * 1000)) { $p.Kill(); Write-Host "TIMEOUT"; exit 1 }
Remove-Item env:RANDAMU_LOG
& (Join-Path $root "spike\.venv\Scripts\python.exe") -I (Join-Path $PSScriptRoot "check_as_is.py") $outDir $renderDir
exit $LASTEXITCODE
