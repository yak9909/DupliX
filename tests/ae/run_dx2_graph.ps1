# DupliX 2 (design v4 P1): a graph driven by keyed macros, and an editor script written through the idle hook
# (tests/ae/dx2_graph.jsx). AE runs hidden and stays open; the steps are sent as scripts to the running instance,
# so AE's main loop (and its idle hook) runs between them. Then check_dx2_graph.py compares the effect's dump with
# graph_test.exe eval and reads the pixels.
#   pwsh tests/ae/run_dx2_graph.ps1 [-WaitSec 5] [-Bpc 8|16|32]
param([int]$WaitSec = 5, [ValidateSet(8, 16, 32)][int]$Bpc = 8)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$out = Join-Path $root "build\test\dx2_graph"
New-Item -ItemType Directory -Force $out | Out-Null
Get-ChildItem $out -File | Remove-Item -Confirm:$false
. (Join-Path $root "spike\tools\ae_bg.ps1")
. (Join-Path $root "spike\tools\ae_guard.ps1")
if (Get-Process AfterFX, aerender -ErrorAction SilentlyContinue) { throw "AfterFX/aerender is already running. Close it first." }
$null = Repair-AeCrashState
$exe = "C:\Program Files\Adobe\Adobe After Effects 2025\Support Files\AfterFX.exe"
$jsx = Join-Path $env:TEMP "duplix_dx2_graph.jsx"
Copy-Item (Join-Path $PSScriptRoot "dx2_graph.jsx") $jsx -Force

$env:DUPLIX_TEST_DIR = $out -replace '\\', '/'
$env:DUPLIX_LOG = Join-Path $out "plugin.log"
$env:DUPLIX2_DUMP = Join-Path $out "dump.txt"
$env:DUPLIX2_TEST_SCRIPT = Join-Path $PSScriptRoot "dx2_graph_script.txt"
$env:DUPLIX_DX2_BPC = "$Bpc"
$env:DUPLIX2_IDLE_LOG = "1"
$ae = Start-AeHidden -Exe $exe -Arguments ""
Remove-Item env:DUPLIX_LOG, env:DUPLIX2_DUMP, env:DUPLIX2_TEST_SCRIPT, env:DUPLIX_DX2_BPC, env:DUPLIX2_IDLE_LOG -ErrorAction SilentlyContinue

function Send-Step([string]$step, [int]$sec) {
    Set-Content -Encoding utf8NoBOM (Join-Path $out "step.txt") $step
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
    Start-Sleep -Seconds $WaitSec  # the idle hook writes the editor script meanwhile
    Send-Step "check" 120
    Start-Sleep -Seconds 3  # the editor notices the undo meanwhile
    Send-Step "close" 60
    Start-Sleep -Seconds 3  # the editor closes meanwhile
} finally {
    foreach ($d in Get-AeHiddenDialogs) { Write-Host ("dialog: {0} [{1}] {2}" -f $d.Process, $d.Title, $d.Text) }
    if (-not $ae.HasExited) { $null = Stop-AeHidden -Process $ae -TimeoutSec 20 }
    $null = Repair-AeCrashState
}
Get-Content (Join-Path $out "jsx.log") | Where-Object { $_ -notmatch '^ #' }
Select-String -Path (Join-Path $out "plugin.log") -Pattern "dx2: (write|test script|preset|GlobalSetup|graph:|arb:|editor)" | ForEach-Object { $_.Line }
& (Join-Path $root "spike\.venv\Scripts\python.exe") -I (Join-Path $PSScriptRoot "check_dx2_graph.py") $out
exit $LASTEXITCODE
