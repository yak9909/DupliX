# THROWAWAY SPIKE — S4: re-render checks inside one AfterFX session (hidden desktop).
# Needs spike/logs/s1/s1.aep (run run_s1.ps1 first). Outputs go to spike/logs/s4.
#   -Step0Only   only render the unchanged comp twice (cache sanity check)
#   -NoGuid      negative control: the plugin leaves the target's receipt GUID out of the cache key
#   -S5b 1|compare   S5b: geometry from AEGP (1 = the jsx also strips the corner expressions)
#   -NoSlot          (with -S5b 1) clear Slot 1/2 too = product-like, no layer params
#   -NoMix           negative control for S5b: geometry is not mixed into the cache key
#   -S7              every layer is a target (plugin + extra layer-list cases in the jsx)
param([switch]$Step0Only, [switch]$NoGuid, [string]$S5b = "0", [switch]$NoSlot, [switch]$NoMix, [switch]$S7,
      [int]$TimeoutSec = 900)
$ErrorActionPreference = "Stop"

$root   = Resolve-Path (Join-Path $PSScriptRoot "..")
$s1Dir  = Join-Path $root "logs\s1"
$outDir = Join-Path $root "logs\s4"
New-Item -ItemType Directory -Force $outDir | Out-Null
Get-ChildItem "$outDir\*" -Include *.tif, *.png, *.log, *.aep -ErrorAction SilentlyContinue | Remove-Item -Confirm:$false
$aepSrc = Join-Path $s1Dir "s1.aep"
if (-not (Test-Path $aepSrc)) { throw "missing $aepSrc (run run_s1.ps1 first)" }
Copy-Item $aepSrc (Join-Path $outDir "s1.aep") -Force

$env:DUPLIX_SPIKE_DIR = ($outDir -replace '\\', '/')
$env:DUPLIX_SPIKE_LOG = Join-Path $outDir "plugin.log"
$env:DUPLIX_S5_SITE = "pre"
$env:DUPLIX_S5_SALT = [guid]::NewGuid().ToString("N")
$env:DUPLIX_S4_STEPS = if ($Step0Only) { "0" } else { "all" }
$env:DUPLIX_S4_NOGUID = if ($NoGuid) { "1" } else { "0" }
$env:DUPLIX_S5B = $S5b
$env:DUPLIX_S4_NOSLOT = if ($NoSlot) { "1" } else { "0" }
$env:DUPLIX_S5B_NOMIX = if ($NoMix) { "1" } else { "0" }
$env:DUPLIX_S7 = if ($S7) { "1" } else { "0" }
Write-Host "S4 s5b=$S5b noslot=$env:DUPLIX_S4_NOSLOT nomix=$env:DUPLIX_S5B_NOMIX steps=$env:DUPLIX_S4_STEPS noguid=$env:DUPLIX_S4_NOGUID salt=$env:DUPLIX_S5_SALT"

$sf = "C:\Program Files\Adobe\Adobe After Effects 2025\Support Files"
$jsx = Join-Path $env:TEMP "duplix_make_s4.jsx"
Copy-Item (Join-Path $root "scenes\make_s4.jsx") $jsx -Force
if ($jsx -match '\s') { throw "script path must not contain spaces: $jsx" }

. (Join-Path $PSScriptRoot "ae_bg.ps1")
. (Join-Path $PSScriptRoot "ae_guard.ps1")
$running = @(Get-Process AfterFX, aerender -ErrorAction SilentlyContinue)
if ($running) { throw "AfterFX/aerender is already running (pid $($running.Id -join ',')). Close it first." }
$null = Repair-AeCrashState

Write-Host "== AfterFX.exe -r make_s4.jsx (hidden desktop)"
$p = Start-AeHidden -Exe (Join-Path $sf "AfterFX.exe") -Arguments "-r $jsx"
$jsxLog = Join-Path $outDir "s4_jsx.log"
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$done = $false
while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
    if ((Test-Path $jsxLog) -and (Select-String -Path $jsxLog -Pattern "^S4 done|EXCEPTION" -Quiet)) { $done = $true; break }
    Start-Sleep -Seconds 2
}
if (-not $done -and -not $p.HasExited) {
    Write-Host "WARNING: script did not finish in time. AE windows:"
    foreach ($w in Get-AeHiddenWindows -ProcessId $p.Id | Where-Object { $_.Visible -and $_.W -gt 100 }) {
        $png = Join-Path $outDir ("aewin_{0}.png" -f $w.Hwnd)
        $null = Save-AeWindowImage -Hwnd $w.Hwnd -Path $png
        Write-Host ("  {0} '{1}' {2}x{3} -> {4}" -f $w.Class, $w.Title, $w.W, $w.H, $png)
    }
}
if (-not $p.HasExited -and -not $p.WaitForExit(20000)) {
    if (-not (Stop-AeHidden -Process $p)) { $null = Repair-AeCrashState }
}
Write-Host "AfterFX done"
if (Test-Path $jsxLog) { Get-Content $jsxLog }

Write-Host "== check"
& (Join-Path $root ".venv\Scripts\python.exe") (Join-Path $root "tools\check_s4.py") $outDir
