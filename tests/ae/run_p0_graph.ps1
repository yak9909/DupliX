# P0 (design v4): the graph editor's window writes the graph data as an undo step (tests/ae/p0_graph.jsx).
# AE runs hidden and stays open; the two steps are sent as scripts to the running instance, so AE's main loop (and
# the editor window's timer, DUPLIX_P0_AUTO) runs between them. Then check_p0_graph.py reads the colors.
#   pwsh tests/ae/run_p0_graph.ps1 [-WaitSec 8] [-Payload 0] [-Path direct|idle]
#   -Path direct: the window's handler writes; idle: it queues the write and AE's idle hook writes
#   -Refresh: PF_RefreshAllWindows after each write (does the Overview's custom UI draw the new graph data at once)
param([int]$WaitSec = 8, [int]$Payload = 0, [ValidateSet("direct", "idle")][string]$Path = "idle", [switch]$Refresh)
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
$env:DUPLIX_P0_REFRESH = $(if ($Refresh) { "1" } else { "0" })
$ae = Start-AeHidden -Exe $exe -Arguments ""
Remove-Item env:DUPLIX_LOG, env:DUPLIX_P0_AUTO, env:DUPLIX_P0_PAYLOAD, env:DUPLIX_P0_PATH, env:DUPLIX_P0_REFRESH -ErrorAction SilentlyContinue

function Send-Step([string]$step, [int]$sec) {
    Set-Content -Encoding utf8NoBOM (Join-Path $out "step.txt") $step
    # AE refuses a script while it still counts the previous one as running: Send-AeScript closes that warning and
    # sends again (spike/tools/ae_bg.ps1)
    $tries = Send-AeScript -Ae $ae -Exe $exe -Jsx $jsx -Done (Join-Path $out "step_done_$step.txt") -TimeoutSec $sec
    if ($tries -gt 1) { Write-Host "  step $step ran on try $tries" }
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
    Start-Sleep -Seconds 3  # the idle hook notices the undo meanwhile
    Send-Step "check2" 60
    Start-Sleep -Seconds 3
    Send-Step "reopen" 90
} finally {
    if (-not $ae.HasExited) { $null = Stop-AeHidden -Process $ae -TimeoutSec 20 }
    $null = Repair-AeCrashState
}
Get-Content (Join-Path $out "jsx.log")
Select-String -Path (Join-Path $out "plugin.log") -Pattern "p0: (write .*ms$|editor|user|sync|params ui|overview draw|refresh)" | ForEach-Object { $_.Line }
& (Join-Path $root "spike\.venv\Scripts\python.exe") -I (Join-Path $PSScriptRoot "check_p0_graph.py") $out
exit $LASTEXITCODE
