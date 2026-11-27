# P0 (design v4): the graph editor's window writes the graph data as an undo step (tests/ae/p0_graph.jsx).
# AE runs hidden and stays open; the two steps are sent as scripts to the running instance, so AE's main loop (and
# the editor window's timer, DUPLIX_P0_AUTO) runs between them. Then check_p0_graph.py reads the colors.
#   pwsh tests/ae/run_p0_graph.ps1 [-WaitSec 8] [-Payload 0] [-Path direct|idle]
#   -Path direct: the window's handler writes; idle: it queues the write and AE's idle hook writes
param([int]$WaitSec = 8, [int]$Payload = 0, [ValidateSet("direct", "idle")][string]$Path = "idle")
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$out = Join-Path $root "build\test\p0_graph"
New-Item -ItemType Directory -Force $out | Out-Null
Get-ChildItem $out -File | Remove-Item -Confirm:$false
. (Join-Path $root "spike\tools\ae_bg.ps1")
. (Join-Path $root "spike\tools\ae_guard.ps1")
if (Get-Process AfterFX, aerender -ErrorAction SilentlyContinue) { throw "AfterFX/aerender is already running. Close it first." }
$null = Repair-AeCrashState
$exe = "C:\Program Files\Adobe\Adobe After Effects 2025\Support Files\AfterFX.exe"
$jsx = Join-Path $env:TEMP "duplix_p0_graph.jsx"
Copy-Item (Join-Path $PSScriptRoot "p0_graph.jsx") $jsx -Force

$env:DUPLIX_TEST_DIR = $out -replace '\\', '/'
$env:DUPLIX_LOG = Join-Path $out "plugin.log"
$env:DUPLIX_P0_AUTO = "1"
$env:DUPLIX_P0_PAYLOAD = "$Payload"
$env:DUPLIX_P0_PATH = $Path
$ae = Start-AeHidden -Exe $exe -Arguments ""
Remove-Item env:DUPLIX_LOG, env:DUPLIX_P0_AUTO, env:DUPLIX_P0_PAYLOAD, env:DUPLIX_P0_PATH

function Send-Step([string]$step, [int]$sec) {
    Set-Content -Encoding utf8NoBOM (Join-Path $out "step.txt") $step
    $done = Join-Path $out "step_done_$step.txt"
    for ($k = 1; $k -le 3; $k++) {
        $s = Start-AeHidden -Exe $exe -Arguments "-r $jsx"
        $null = $s.WaitForExit(30000)
        $deadline = (Get-Date).AddSeconds($sec)
        while (-not (Test-Path $done) -and (Get-Date) -lt $deadline -and -not $ae.HasExited) { Start-Sleep -Milliseconds 300 }
        if (Test-Path $done) { return }
        if ($ae.HasExited) { throw "AE exited (step $step)" }
        foreach ($d in Get-AeHiddenDialogs) { Write-Host ("  dialog: {0} [{1}] {2}" -f $d.Process, $d.Title, $d.Text) }
        Write-Host "step $step did not finish (try $k)"
    }
    throw "step $step did not run"
}
try {
    $ready = $false
    for ($j = 0; $j -lt 80 -and -not $ae.HasExited; $j++) {
        Start-Sleep -Seconds 3
        if (Get-AeHiddenWindows -ProcessId $ae.Id | Where-Object { $_.Visible -and $_.Class -like "AE_CApplication*" }) { $ready = $true; break }
    }
    if (-not $ready) { throw "AE did not finish starting" }
    Start-Sleep -Seconds 3
    Send-Step "setup" 60
    Start-Sleep -Seconds $WaitSec  # the editor's timer writes meanwhile
    Send-Step "check" 60
} finally {
    if (-not $ae.HasExited) { $null = Stop-AeHidden -Process $ae -TimeoutSec 20 }
    $null = Repair-AeCrashState
}
Get-Content (Join-Path $out "jsx.log")
Select-String -Path (Join-Path $out "plugin.log") -Pattern "p0: (write|editor|user)" | ForEach-Object { $_.Line }
& (Join-Path $root "spike\.venv\Scripts\python.exe") -I (Join-Path $PSScriptRoot "check_p0_graph.py") $out
exit $LASTEXITCODE
