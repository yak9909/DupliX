# THROWAWAY SPIKE: build scenes/make_approx.jsx (hidden desktop), render with aerender,
# and write logs/approx/approx_compare.png. The plugin is not involved.
$ErrorActionPreference = "Stop"
$root   = Resolve-Path (Join-Path $PSScriptRoot "..")
$outDir = Join-Path $root "logs\approx"
New-Item -ItemType Directory -Force $outDir | Out-Null
Get-ChildItem "$outDir\*" -Include *.tif, *.png, *.log, *.aep -ErrorAction SilentlyContinue | Remove-Item -Confirm:$false
$env:DUPLIX_SPIKE_DIR = ($outDir -replace '\\', '/')
$sf = "C:\Program Files\Adobe\Adobe After Effects 2025\Support Files"

. (Join-Path $PSScriptRoot "ae_bg.ps1")
. (Join-Path $PSScriptRoot "ae_guard.ps1")
$running = @(Get-Process AfterFX, aerender -ErrorAction SilentlyContinue)
if ($running) { throw "AfterFX/aerender is already running (pid $($running.Id -join ',')). Close it first." }
$null = Repair-AeCrashState

$jsx = Join-Path $env:TEMP "duplix_make_approx.jsx"
Copy-Item (Join-Path $root "scenes\make_approx.jsx") $jsx -Force
$p = Start-AeHidden -Exe (Join-Path $sf "AfterFX.exe") -Arguments "-r $jsx"
$jsxLog = Join-Path $outDir "jsx.log"
$deadline = (Get-Date).AddSeconds(300)
while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
    if ((Test-Path $jsxLog) -and (Select-String -Path $jsxLog -Pattern "^saved |EXCEPTION" -Quiet)) { break }
    Start-Sleep -Seconds 2
}
if (-not $p.HasExited -and -not $p.WaitForExit(20000)) {
    if (-not (Stop-AeHidden -Process $p)) { $null = Repair-AeCrashState }
}
Get-Content $jsxLog
$aep = Join-Path $outDir "approx.aep"
if (-not (Test-Path $aep)) { throw "scene not created" }

$a = Start-Process -FilePath (Join-Path $sf "aerender.exe") -ArgumentList @("-project", "`"$aep`"") -PassThru -NoNewWindow `
        -RedirectStandardOutput (Join-Path $outDir "aerender.out.log")
if (-not $a.WaitForExit(300000)) { Stop-Process -Id $a.Id -Force; throw "aerender timeout" }
& (Join-Path $root ".venv\Scripts\python.exe") (Join-Path $root "tools\compare_approx.py") $outDir
