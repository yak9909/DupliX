# Viewer (preview) timing of a user scene, measured inside the plugin (RANDAMU_TIMING):
# starts AE hidden on a COPY of the scene, waits for startup, sends preview_setup.jsx (variant comps),
# then for each variant sends preview_step.jsx (viewer steps one frame every -IntervalMs) and summarises
# the plugin's "timing:" lines per comp. AE stays open between scripts (scripts sent with a second
# `AfterFX.exe -r` reach the running instance once its startup has finished).
#   pwsh tests/ae/run_preview.ps1 -Aep "tests/ae/user/DupliX Test.aep" [-Frames 40] [-IntervalMs 600]
param([Parameter(Mandatory)][string]$Aep, [int]$Frames = 40, [int]$IntervalMs = 600,
      [string]$Variants = "pv_asis,pv_one,pv_real10", [string]$Layer = "")
$env:RANDAMU_PV_LAYER = $Layer
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$out = Join-Path $root "build\test\preview"
New-Item -ItemType Directory -Force $out | Out-Null
Get-ChildItem $out -File | Remove-Item -Confirm:$false
. (Join-Path $root "spike\tools\ae_bg.ps1")
. (Join-Path $root "spike\tools\ae_guard.ps1")
if (Get-Process AfterFX, aerender -ErrorAction SilentlyContinue) { throw "AfterFX/aerender is already running. Close it first." }
$null = Repair-AeCrashState
$exe = "C:\Program Files\Adobe\Adobe After Effects 2025\Support Files\AfterFX.exe"

# AfterFX.exe cannot take paths with spaces: everything goes through %TEMP%. Never open the user's file.
$scene = Join-Path $env:TEMP "randamu_preview.aep"
Copy-Item (Resolve-Path $Aep).Path $scene -Force
foreach ($s in "preview_setup", "preview_step") { Copy-Item (Join-Path $PSScriptRoot "$s.jsx") (Join-Path $env:TEMP "randamu_$s.jsx") -Force }

$env:RANDAMU_TEST_DIR = $out -replace '\\', '/'
$env:RANDAMU_LOG = Join-Path $out "plugin.log"
$env:RANDAMU_TIMING = "1"
$ae = Start-AeHidden -Exe $exe -Arguments $scene
Remove-Item env:RANDAMU_LOG, env:RANDAMU_TIMING
function Send-Script([string]$name) {
    $s = Start-AeHidden -Exe $exe -Arguments ("-r " + (Join-Path $env:TEMP "randamu_$name.jsx"))
    $null = $s.WaitForExit(30000)
}
function Wait-File([string]$path, [int]$sec) {
    $deadline = (Get-Date).AddSeconds($sec)
    while (-not (Test-Path $path) -and (Get-Date) -lt $deadline -and -not $ae.HasExited) { Start-Sleep -Milliseconds 500 }
    Test-Path $path
}
try {
    $ready = $false
    for ($j = 0; $j -lt 80 -and -not $ae.HasExited; $j++) {
        Start-Sleep -Seconds 3
        if (Get-AeHiddenWindows -ProcessId $ae.Id | Where-Object { $_.Visible -and $_.Class -like "AE_CApplication*" }) { $ready = $true; break }
    }
    if (-not $ready) { throw "AE did not finish starting" }
    Start-Sleep -Seconds 3
    Send-Script "preview_setup"
    if (-not (Wait-File (Join-Path $out "variants.txt") 120)) { throw "setup script did not run" }
    $vars = @{}
    foreach ($l in Get-Content (Join-Path $out "variants.txt")) {
        if ($l -like "EXCEPTION*") { throw $l }
        $p = $l -split " "; $vars[$p[0]] = [int]$p[1]
    }
    foreach ($v in $Variants -split ",") {
        Remove-Item (Join-Path $out "step_done.txt") -ErrorAction SilentlyContinue
        Set-Content -Encoding utf8NoBOM (Join-Path $out "step_cmd.txt") "$v $Frames $IntervalMs"
        Send-Script "preview_step"
        if (-not (Wait-File (Join-Path $out "step_done.txt") ($Frames * $IntervalMs / 1000 + 120))) { Write-Host "TIMEOUT $v" }
        Start-Sleep -Seconds 2
    }
} finally {
    if (-not $ae.HasExited) { $null = Stop-AeHidden -Process $ae -TimeoutSec 20 }
    $null = Repair-AeCrashState
}

# Summary: "timing:" lines with drawn copies, per comp (first frame of each variant dropped: it includes
# setup work). Lines without copies are AE's pre-render passes over the whole comp, not drawn frames.
$rows = foreach ($l in Select-String -Path (Join-Path $out "plugin.log") -Pattern "timing: comp (\d+) t=([\d.]+) span=([\d.]+)ms in-plugin=([\d.]+)ms .*copies ([1-9]\d*)") {
    $g = $l.Matches[0].Groups
    [pscustomobject]@{ Comp = [int]$g[1].Value; T = [double]$g[2].Value; Span = [double]$g[3].Value; Plugin = [double]$g[4].Value; Copies = [int]$g[5].Value }
}
foreach ($v in $Variants -split ",") {
    $r = @($rows | Where-Object Comp -eq $vars[$v] | Sort-Object T | Select-Object -Skip 1)
    if (-not $r) { Write-Host ("{0,-10} no timing lines" -f $v); continue }
    $span = ($r | Measure-Object Span -Average -Maximum); $pl = ($r | Measure-Object Plugin -Average)
    Write-Host ("{0,-10} frames {1,3}  span avg {2,7:N1} ms (max {3,7:N1})  in-plugin avg {4,6:N1} ms  copies/frame {5}" -f
        $v, $r.Count, $span.Average, $span.Maximum, $pl.Average, ($r | Measure-Object Copies -Average).Average)
}
