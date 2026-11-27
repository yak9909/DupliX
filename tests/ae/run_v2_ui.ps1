# DupliX 2 (prototype): pictures of AE's window with the Effect Controls showing the effect (the custom UI cannot
# be checked by a render). Runs tests/ae/v2_ui.jsx in the hidden-desktop AE, waits for AE to draw, saves every
# large visible AE window as build\test\v2_ui\window_<n>.png, then closes AE.
#   pwsh tests/ae/run_v2_ui.ps1 [-WaitSec 10]
param([int]$TimeoutSec = 240, [int]$WaitSec = 10)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$outDir = Join-Path $root "build\test\v2_ui"
New-Item -ItemType Directory -Force $outDir | Out-Null
Get-ChildItem "$outDir\*" -Include *.png, *.log -ErrorAction SilentlyContinue | Remove-Item -Confirm:$false
$env:DUPLIX_TEST_DIR = ($outDir -replace '\\', '/')
$env:DUPLIX_LOG = Join-Path $outDir "plugin.log"
$sf = "C:\Program Files\Adobe\Adobe After Effects 2025\Support Files"
. (Join-Path $root "spike\tools\ae_bg.ps1")
. (Join-Path $root "spike\tools\ae_guard.ps1")
$running = @(Get-Process AfterFX, aerender -ErrorAction SilentlyContinue)
if ($running) { throw "AfterFX/aerender is already running (pid $($running.Id -join ',')). Close it first." }
$null = Repair-AeCrashState
$tmp = Join-Path $env:TEMP "duplix_test_v2_ui.jsx"
Copy-Item (Join-Path $PSScriptRoot "v2_ui.jsx") $tmp -Force
$p = Start-AeHidden -Exe (Join-Path $sf "AfterFX.exe") -Arguments "-r $tmp"
$jsxLog = Join-Path $outDir "jsx.log"
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
    if ((Test-Path $jsxLog) -and (Select-String -Path $jsxLog -Pattern "^done|EXCEPTION" -Quiet)) { break }
    Start-Sleep -Seconds 2
}
if (Test-Path $jsxLog) { Get-Content $jsxLog | Write-Host }
Start-Sleep -Seconds $WaitSec   # AE draws its panels once the script has returned
$n = 0
foreach ($w in Get-AeHiddenWindows -ProcessId $p.Id | Where-Object { $_.Visible -and $_.W -ge 400 -and $_.H -ge 300 }) {
    $path = Join-Path $outDir "window_$n.png"
    $ok = Save-AeWindowImage -Hwnd $w.Hwnd -Path $path
    Write-Host ("window_{0}.png  {1} x {2}  class={3}  title='{4}'  ok={5}" -f $n, $w.W, $w.H, $w.Class, $w.Title, $ok)
    $n++
}
$null = Stop-AeHidden -Process $p
