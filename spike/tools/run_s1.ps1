# THROWAWAY SPIKE: build the S1/S2 test project with AfterFX.com, render it with aerender,
# then print the plugin log. Requires RandamuSpike.aex to be visible to AE
# (MediaCore\randamu_dev junction -> spike\build\out).
param([switch]$SkipScene, [string]$Mfr = "", [string]$S5Site = "pre", [int]$RenderTimeoutSec = 600)
$ErrorActionPreference = "Stop"

$root   = Resolve-Path (Join-Path $PSScriptRoot "..")
$outDir = Join-Path $root "logs\s1"
New-Item -ItemType Directory -Force $outDir | Out-Null
$clean = @("*.png", "*.tif", "*.mp4", "plugin.log", "aerender.out.log")
if (-not $SkipScene) { $clean += @("*.aep", "jsx.log") }
Get-ChildItem "$outDir\*" -Include $clean -ErrorAction SilentlyContinue | Remove-Item -Confirm:$false

$env:RANDAMU_SPIKE_DIR = ($outDir -replace '\\', '/')
$env:RANDAMU_SPIKE_LOG = Join-Path $outDir "plugin.log"
$env:RANDAMU_SPIKE_ASSETS = ((Join-Path $root "scenes\assets") -replace '\\', '/')
# S5: where the plugin renders the target through AEGP (pre = PreRender, smart = SmartRender),
# and a per-run salt mixed into the cache key so no run reuses an earlier run's disk cache.
$env:RANDAMU_S5_SITE = $S5Site
$env:RANDAMU_S5_SALT = [guid]::NewGuid().ToString("N")
Write-Host "S5 site=$S5Site salt=$env:RANDAMU_S5_SALT mfr='$Mfr'"

$sf = "C:\Program Files\Adobe\Adobe After Effects 2025\Support Files"

if (-not $SkipScene) {
    # AfterFX -r cannot take a script path containing spaces: copy to %TEMP% first.
    $jsx = Join-Path $env:TEMP "randamu_make_s1.jsx"
    Copy-Item (Join-Path $root "scenes\make_s1.jsx") $jsx -Force
    # -noui does not run -r scripts reliably on this machine; use the UI app (script calls app.quit()).
    . (Join-Path $PSScriptRoot "ae_bg.ps1")
    . (Join-Path $PSScriptRoot "ae_guard.ps1")
    # A running AfterFX (e.g. the user's own) would receive our -r script instead: refuse.
    $running = @(Get-Process AfterFX -ErrorAction SilentlyContinue)
    if ($running) { throw "AfterFX is already running (pid $($running.Id -join ',')). Close it first." }
    # A previous session that was killed leaves AppState=3 in the registry, which makes AE
    # show the modal crash-repair dialog. Mark it clean so the dialog never appears.
    $null = Repair-AeCrashState
    # AE runs on a hidden desktop (ae_bg.ps1): nothing appears on the user's screen.
    Write-Host "== building scene with AfterFX.exe -r (hidden desktop)"
    # AfterFX.exe does no quote processing: a quoted -r path silently runs nothing.
    if ($jsx -match '\s') { throw "script path must not contain spaces: $jsx" }
    $p = Start-AeHidden -Exe (Join-Path $sf "AfterFX.exe") -Arguments "-r $jsx"
    # app.quit() is not reliable (AE sometimes stays on the Home screen): wait for the
    # script's "saved" line, then close AE ourselves.
    $jsxLog = Join-Path $outDir "jsx.log"
    $deadline = (Get-Date).AddSeconds(300)
    $done = $false
    while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
        if ((Test-Path $jsxLog) -and (Select-String -Path $jsxLog -Pattern "^saved |EXCEPTION" -Quiet)) { $done = $true; break }
        Start-Sleep -Seconds 2
    }
    if (-not $done -and -not $p.HasExited) {
        # Something blocks AE (unknown dialog?): save pictures of AE's own windows only.
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
    Get-Content (Join-Path $outDir "jsx.log")
}

$aep = Join-Path $outDir "s1.aep"
if (-not (Test-Path $aep)) { throw "scene not created: $aep" }

# If AE crashes inside a plugin, the next AE launch shows a one-shot "<plugin> may have caused
# AE to quit" dialog (492x295). To find where AE keeps that record, snapshot the registry and
# AE's files now, and again right after a crash (before any AE launch consumes the record).
function Save-AeStateSnapshot([string]$Dir, [string]$Tag) {
    New-Item -ItemType Directory -Force $Dir | Out-Null
    reg export "HKCU\Software\Adobe" (Join-Path $Dir "reg_$Tag.reg") /y | Out-Null
    $rows = foreach ($r in "$env:APPDATA\Adobe", "$env:LOCALAPPDATA\Adobe", "$env:LOCALAPPDATA\Temp\Adobe", "$env:USERPROFILE\Documents\Adobe") {
        if (Test-Path $r) {
            Get-ChildItem $r -Recurse -File -Force -ErrorAction SilentlyContinue | ForEach-Object {
                $h = if ($_.Length -lt 4MB) { (Get-FileHash $_.FullName -Algorithm MD5 -ErrorAction SilentlyContinue).Hash } else { "big" }
                "{0}`t{1}`t{2}" -f $_.FullName, $_.Length, $h
            }
        }
    }
    $rows | Set-Content (Join-Path $Dir "files_$Tag.txt") -Encoding utf8
}
$stateDir = Join-Path $outDir "crash_state"
Save-AeStateSnapshot $stateDir "before_render"

Write-Host "== aerender"
$args = @("-project", "`"$aep`"", "-v", "ERRORS_AND_PROGRESS")
if ($Mfr) { $args += @("-mfr", $Mfr, "100") }   # aerender: -mfr <ON|OFF> <max_cpu_percent>
$p = Start-Process -FilePath (Join-Path $sf "aerender.exe") -ArgumentList $args -PassThru -NoNewWindow `
        -RedirectStandardOutput (Join-Path $outDir "aerender.out.log")
# A render that re-enters AE (AEGP render inside an effect) can deadlock: never wait forever.
if (-not $p.WaitForExit($RenderTimeoutSec * 1000)) {
    Write-Host "TIMEOUT: aerender did not finish in $RenderTimeoutSec s - killing it (possible deadlock)"
    $kids = @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($p.Id)" -ErrorAction SilentlyContinue)
    foreach ($k in $kids) { Stop-Process -Id $k.ProcessId -Force -ErrorAction SilentlyContinue }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    . (Join-Path $PSScriptRoot "ae_guard.ps1")
    $null = Repair-AeCrashState
}
Write-Host "aerender exit: $($p.ExitCode)"
# aerender can exit 0 even when its AE host died mid-render ("Unable to receive").
if (Select-String -Path (Join-Path $outDir "aerender.out.log") -Pattern "aerender ERROR|Unable to receive" -Quiet) {
    Write-Host "WARNING: aerender reported an error (AE host crashed?)"
    Start-Sleep -Seconds 5
    Save-AeStateSnapshot $stateDir "after_crash"
    Write-Host "state snapshots saved in $stateDir (before_render / after_crash)"
}
Get-Content (Join-Path $outDir "aerender.out.log") | Select-Object -Last 30

Write-Host "== plugin.log"
if (Test-Path $env:RANDAMU_SPIKE_LOG) { Get-Content $env:RANDAMU_SPIKE_LOG } else { Write-Host "(no plugin log — plugin not loaded?)" }
Get-ChildItem "$outDir\*" -Include *.png, *.tif | Select-Object Name, Length

$renders = @(Select-String -Path $env:RANDAMU_SPIKE_LOG -Pattern "SmartRender: bpc" -ErrorAction SilentlyContinue).Count
Write-Host "SmartRender calls this run: $renders"
if ($renders -eq 0) { Write-Host "WARNING: plugin never rendered - frames came from AE's cache" }

Write-Host "== check"
& (Join-Path $root ".venv\Scripts\python.exe") (Join-Path $root "tools\check_s1.py") $outDir
