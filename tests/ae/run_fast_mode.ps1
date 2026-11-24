# Fast mode checks: builds fast.aep with tests/ae/fast_mode.jsx (hidden AE), renders it with aerender
# (MFR off and on) and compares fast vs normal mode with check_fast_mode.py. Exit 1 on any failure.
#   pwsh tests/ae/run_fast_mode.ps1 [-Cases fast_identity,fast_scatter] [-Mfrs OFF,ON] [-SkipScene]
param([string]$Cases = "", [string[]]$Mfrs = @("OFF", "ON"), [switch]$SkipScene, [int]$RenderTimeoutSec = 600)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$outDir = Join-Path $root "build\test\fast"
$py = Join-Path $root "spike\.venv\Scripts\python.exe"
$check = Join-Path $PSScriptRoot "check_fast_mode.py"

if (-not $SkipScene) {
    $env:RANDAMU_FAST_IMAGE = Join-Path $root "spike\assets\probe_img.png"
    $env:RANDAMU_FAST_ONLY = $Cases
    & (Join-Path $PSScriptRoot "run_jsx.ps1") -Jsx tests/ae/fast_mode.jsx -Name fast -TimeoutSec 900 -Expect "^done" |
        Select-String "EXCEPTION|TIMEOUT|EXPECT|window class|half:"
    if ($LASTEXITCODE -ne 0) { Write-Host "scene build failed"; Get-Content (Join-Path $outDir "jsx.log") -Tail 5; exit 1 }
}

. (Join-Path $root "spike\tools\ae_guard.ps1")
$sf = "C:\Program Files\Adobe\Adobe After Effects 2025\Support Files"
$aep = Join-Path $outDir "fast.aep"
$fail = 0
foreach ($mfr in $Mfrs) {
    Get-ChildItem $outDir -Filter "r_*.tif" -ErrorAction SilentlyContinue | Remove-Item -Confirm:$false
    $null = Repair-AeCrashState
    $env:RANDAMU_LOG = Join-Path $outDir "plugin_aer_$mfr.log"
    if (Test-Path $env:RANDAMU_LOG) { Remove-Item $env:RANDAMU_LOG }
    $log = Join-Path $outDir "aerender_$mfr.log"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath (Join-Path $sf "aerender.exe") -PassThru -NoNewWindow -RedirectStandardOutput $log `
        -ArgumentList @("-project", "`"$aep`"", "-mfr", $mfr, "100", "-v", "ERRORS_AND_PROGRESS")
    if (-not $p.WaitForExit($RenderTimeoutSec * 1000)) { Write-Host "TIMEOUT aerender mfr=$mfr"; $p.Kill(); $fail++; continue }
    Remove-Item env:RANDAMU_LOG
    Write-Host ("aerender mfr={0}: {1:N1} s" -f $mfr, $sw.Elapsed.TotalSeconds)
    & $py -I $check $outDir $mfr $Cases
    if ($LASTEXITCODE -ne 0) { $fail++ }
}
Write-Host $(if ($fail -eq 0) { "FAST ALL PASS" } else { "FAST FAILURES (runs): $fail" })
exit $(if ($fail -eq 0) { 0 } else { 1 })
