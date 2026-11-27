# P0: DupliX's Copies keyframed in 3D Layers (tests/ae/count_keys.jsx), then the keyed comps rendered by aerender
# with MFR on, then check_count_keys.py.
#   pwsh tests/ae/run_count_keys.ps1
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$out = Join-Path $root "build\test\count_keys"
$sf = "C:\Program Files\Adobe\Adobe After Effects 2025\Support Files"
& (Join-Path $PSScriptRoot "run_jsx.ps1") -Jsx tests/ae/count_keys.jsx -Name count_keys -TimeoutSec 600 -Expect "^done" |
    Select-String "EXCEPTION|TIMEOUT|EXPECT|counts|MISSING"
. (Join-Path $root "spike\tools\ae_guard.ps1")
$null = Repair-AeCrashState
foreach ($i in 1, 2) {
    $log = Join-Path $out "aerender_$i.log"
    $p = Start-Process -FilePath (Join-Path $sf "aerender.exe") -PassThru -NoNewWindow -RedirectStandardOutput $log `
        -Environment @{ DUPLIX_LOG = (Join-Path $out "plugin_mfr_$i.log") } `
        -ArgumentList @("-project", "`"$(Join-Path $out "count_keys.aep")`"", "-rqindex", "$i", "-mfr", "ON", "100", "-v", "ERRORS_AND_PROGRESS")
    if (-not $p.WaitForExit(600000)) { $p.Kill(); Write-Host "TIMEOUT aerender $i" }
}
& (Join-Path $root "spike\.venv\Scripts\python.exe") -I (Join-Path $PSScriptRoot "check_count_keys.py") $out
exit $LASTEXITCODE
