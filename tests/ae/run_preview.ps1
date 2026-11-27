# Viewer (preview) timing of a user scene, measured inside the plugin (DUPLIX_TIMING):
# starts AE hidden on a COPY of the scene, waits for startup, sends preview_setup.jsx (variant comps),
# then for each variant sends preview_step.jsx (viewer steps one frame every -IntervalMs) and summarises
# the plugin's "timing:" lines per comp. AE stays open between scripts (scripts sent with a second
# `AfterFX.exe -r` reach the running instance once its startup has finished).
#   pwsh tests/ae/run_preview.ps1 -Aep "tests/ae/user/DupliX Test.aep" [-Frames 40] [-IntervalMs 600]
param([Parameter(Mandatory)][string]$Aep, [int]$Frames = 40, [int]$IntervalMs = 600,
      [string]$Variants = "pv_asis,pv_one,pv_real10", [string]$Layer = "", [int]$Edit = -1)
# -Edit <frame>: instead of timing, preview_edit.jsx edits DupliX's parameters on the variants at that frame and
# check_preview_edit.py compares each edited frame with a fresh duplicate's
$env:DUPLIX_PV_LAYER = $Layer
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
$scene = Join-Path $env:TEMP "duplix_preview.aep"
Copy-Item (Resolve-Path $Aep).Path $scene -Force
foreach ($s in "preview_setup", "preview_step", "preview_edit") { Copy-Item (Join-Path $PSScriptRoot "$s.jsx") (Join-Path $env:TEMP "duplix_$s.jsx") -Force }

$env:DUPLIX_TEST_DIR = $out -replace '\\', '/'
$env:DUPLIX_LOG = Join-Path $out "plugin.log"
$env:DUPLIX_TIMING = "1"
$ae = Start-AeHidden -Exe $exe -Arguments $scene
Remove-Item env:DUPLIX_LOG, env:DUPLIX_TIMING
Add-Type -TypeDefinition @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
public static class PvChild {
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, StringBuilder l);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    public static void Close(IntPtr h) { PostMessage(h, 0x0010 /* WM_CLOSE */, IntPtr.Zero, IntPtr.Zero); }
    public static List<string> Texts(IntPtr p) {
        var r = new List<string>();
        EnumChildWindows(p, (h, l) => { var c = new StringBuilder(256); var t = new StringBuilder(8192);
            GetClassName(h, c, 256); SendMessage(h, 0x000D /* WM_GETTEXT */, (IntPtr)8192, t); r.Add(c + " [" + t + "]"); return true; }, IntPtr.Zero);
        return r;
    }
}
"@
function Send-Once([string]$name) {
    $s = Start-AeHidden -Exe $exe -Arguments ("-r " + (Join-Path $env:TEMP "duplix_$name.jsx"))
    $null = $s.WaitForExit(30000)
}
# A script sent to the running AE sometimes never arrives (seen 11/26: the run then waited out every timeout and
# printed "no timing lines"). Each script writes script_got.txt first: send again (up to 3 times) when it does not
# appear within 25 s; the scripts run only once even if a late copy arrives.
function Send-Script([string]$name) {
    $got = Join-Path $out "script_got.txt"
    Remove-Item $got -ErrorAction SilentlyContinue
    for ($k = 1; $k -le 3; $k++) {
        Send-Once $name
        $deadline = (Get-Date).AddSeconds(25)
        while (-not (Test-Path $got) -and (Get-Date) -lt $deadline -and -not $ae.HasExited) { Start-Sleep -Milliseconds 300 }
        if (Test-Path $got) { return }
        if ($ae.HasExited) { throw "AE exited (sending $name)" }
        Write-Host "script $name did not arrive (try $k)"
        # what AE shows meanwhile (a dialog would block scripts)
        foreach ($d in Get-AeHiddenDialogs) { Write-Host ("  dialog: pid {0} {1} [{2}] {3}" -f $d.Pid, $d.Process, $d.Title, $d.Text) }
        foreach ($w in Get-AeHiddenWindows -ProcessId $ae.Id | Where-Object Visible) {
            Write-Host ("  window: {0} [{1}] {2}x{3}" -f $w.Class, $w.Title, $w.W, $w.H)
            if ($w.Class -ne "#32770") { continue }
            $texts = [PvChild]::Texts([IntPtr]$w.Hwnd)
            foreach ($c in $texts) { Write-Host "    child: $c" }
            # AE's warning "a second script while one is running was not run" (sent too early): close it, send again
            if (($texts -join " ") -match "2 つめのスクリプト|second script") { [PvChild]::Close([IntPtr]$w.Hwnd); Write-Host "    (closed)"; Start-Sleep -Seconds 3 }
        }
        foreach ($p in Get-CimInstance Win32_Process -Filter "Name='AfterFX.exe'") { Write-Host ("  process: {0} {1}" -f $p.ProcessId, $p.CommandLine) }
    }
    throw "script $name did not arrive after 3 tries"
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
    # the setup script is still finishing when its file appears: a script sent now is refused with a warning dialog
    Start-Sleep -Seconds 5
    $vars = @{}
    foreach ($l in Get-Content (Join-Path $out "variants.txt")) {
        if ($l -like "EXCEPTION*") { throw $l }
        $p = $l -split " "; $vars[$p[0]] = [int]$p[1]
    }
    if ($Edit -ge 0) {
        Set-Content -Encoding utf8NoBOM (Join-Path $out "step_cmd.txt") "$Variants $Edit"
        Send-Script "preview_edit"
        if (-not (Wait-File (Join-Path $out "step_done.txt") 900)) { Write-Host "TIMEOUT edit" }
        Get-Content (Join-Path $out "step_done.txt") -ErrorAction SilentlyContinue
        $Variants = ""
    }
    foreach ($v in $Variants -split "," | Where-Object { $_ }) {
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
if ($Edit -ge 0) { & (Join-Path $root "spike\.venv\Scripts\python.exe") (Join-Path $PSScriptRoot "check_preview_edit.py") $out; exit $LASTEXITCODE }

# Summary: "timing:" lines with drawn copies, per comp (first frame of each variant dropped: it includes
# setup work). Lines without copies are AE's pre-render passes over the whole comp, not drawn frames; fast mode
# (pv_fast*) draws without hooks: its lines with an effect render count.
$rows = foreach ($l in Select-String -Path (Join-Path $out "plugin.log") -Pattern "timing: comp (\d+) t=([\d.]+) span=([\d.]+)ms in-plugin=([\d.]+)ms .*render [\d.]+ms/(\d+), copies (\d+)") {
    $g = $l.Matches[0].Groups
    [pscustomobject]@{ Comp = [int]$g[1].Value; T = [double]$g[2].Value; Span = [double]$g[3].Value; Plugin = [double]$g[4].Value; Renders = [int]$g[5].Value; Copies = [int]$g[6].Value }
}
foreach ($v in $Variants -split ",") {
    $fast = $v -like "pv_fast*"
    $r = @($rows | Where-Object { $_.Comp -eq $vars[$v] -and ($(if ($fast) { $_.Renders -gt 0 } else { $_.Copies -gt 0 })) } | Sort-Object T | Select-Object -Skip 1)
    if (-not $r) { Write-Host ("{0,-10} no timing lines" -f $v); continue }
    $span = ($r | Measure-Object Span -Average -Maximum); $pl = ($r | Measure-Object Plugin -Average)
    Write-Host ("{0,-10} frames {1,3}  span avg {2,7:N1} ms (max {3,7:N1})  in-plugin avg {4,6:N1} ms  copies/frame {5}" -f
        $v, $r.Count, $span.Average, $span.Maximum, $pl.Average, ($r | Measure-Object Copies -Average).Average)
}
