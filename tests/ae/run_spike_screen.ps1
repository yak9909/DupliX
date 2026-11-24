# SPIKE: renders build/test/spike_screen/spike.aep (made by spike_screen.jsx with RANDAMU_SPIKE_SAVEONLY=1)
# with aerender and RANDAMU_SPIKE_SCREEN=1, then checks the frames with check_spike_screen.py.
#   pwsh tests/ae/run_spike_screen.ps1 [-Scene]
param([switch]$Scene)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$out = Join-Path $root "build\test\spike_screen"
if ($Scene) {
    $env:RANDAMU_SPIKE_SAVEONLY = "1"
    & (Join-Path $PSScriptRoot "run_jsx.ps1") -Jsx tests/ae/spike_screen.jsx -Name spike_screen -TimeoutSec 300 -Expect "^done" | Select-Object -Last 1
    Remove-Item env:RANDAMU_SPIKE_SAVEONLY
}
. (Join-Path $root "spike\tools\ae_guard.ps1")
$null = Repair-AeCrashState
Get-ChildItem $out -Filter "r_*.tif" -ErrorAction SilentlyContinue | Remove-Item -Confirm:$false
$env:RANDAMU_SPIKE_SCREEN = "1"
$env:RANDAMU_LOG = Join-Path $out "plugin_aer.log"
$p = Start-Process -FilePath "C:\Program Files\Adobe\Adobe After Effects 2025\Support Files\aerender.exe" -PassThru -NoNewWindow `
    -RedirectStandardOutput (Join-Path $out "aerender.log") -ArgumentList @("-project", "`"$(Join-Path $out 'spike.aep')`"", "-v", "ERRORS_AND_PROGRESS")
$null = $p.WaitForExit(300000)
Remove-Item env:RANDAMU_SPIKE_SCREEN, env:RANDAMU_LOG
Get-ChildItem $out -Filter "r_*.tif" | ForEach-Object Name
& (Join-Path $root "spike\.venv\Scripts\python.exe") -I (Join-Path $PSScriptRoot "check_spike_screen.py") $out
