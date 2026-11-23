# THROWAWAY SPIKE: build probe_3d (saved .aep + render queue) in the hidden AE, then render it with
# aerender (fresh process, hooks installed at GLOBAL_SETUP) for MFR OFF/ON and compare probe vs ref.
# Fill options (RANDAMU_FILL_*) are taken from the caller's environment.
param([string]$Name = "3daer", [int]$RenderTimeoutSec = 300, [switch]$SkipScene, [string[]]$Mfrs = @("OFF", "ON"))
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$outDir = Join-Path $root "logs\probe_$Name"
$sf = "C:\Program Files\Adobe\Adobe After Effects 2025\Support Files"
. (Join-Path $PSScriptRoot "ae_guard.ps1")
$py = Join-Path $root ".venv\Scripts\python.exe"
$cmp = $env:RANDAMU_CMP_PY

if (-not $SkipScene) {
    $env:RANDAMU_3D_SAVE = "1"
    $env:RANDAMU_SPIKE_LOG = Join-Path $outDir "plugin_scene.log"
    & (Join-Path $PSScriptRoot "run_probe.ps1") -Jsx scenes\probe_3d.jsx -Name $Name -TimeoutSec 400 |
        Select-String "TARGET_ID|rq |saved|EXC|^done"
}
$ids = @(Select-String -Path (Join-Path $outDir "jsx.log") -Pattern "TARGET_ID=(\d+)" | ForEach-Object { $_.Matches[0].Groups[1].Value })
if ($ids.Count -eq 0) { Write-Host "no TARGET_ID in jsx.log (scene build failed)"; Get-Content (Join-Path $outDir "jsx.log") -Tail 5; exit 1 }
$id = $ids[0]
$env:RANDAMU_FILL_LAYERIDS = ($ids -join ",")
$aep = Join-Path $outDir "probe3d.aep"
Write-Host "target id=$id aep=$aep"

foreach ($mfr in $Mfrs) {
    Get-ChildItem $outDir -Filter "r_*.tif" -ErrorAction SilentlyContinue | Remove-Item -Confirm:$false
    $null = Repair-AeCrashState
    Remove-Item env:RANDAMU_PROBE_SETS -ErrorAction SilentlyContinue
    $env:RANDAMU_FILL_GLOBAL = "1"
    $env:RANDAMU_FILL_LAYERID = $id
    $env:RANDAMU_SPIKE_LOG = Join-Path $outDir "plugin_aer_$mfr.log"
    $log = Join-Path $outDir "aerender_$mfr.log"
    $p = Start-Process -FilePath (Join-Path $sf "aerender.exe") -PassThru -NoNewWindow -RedirectStandardOutput $log `
        -ArgumentList @("-project", "`"$aep`"", "-mfr", $mfr, "100", "-v", "ERRORS_AND_PROGRESS")
    $sw = [Diagnostics.Stopwatch]::StartNew()
    if (-not $p.WaitForExit($RenderTimeoutSec * 1000)) { Write-Host "TIMEOUT mfr=$mfr"; $p.Kill(); continue }
    Write-Host ("aerender mfr={0} took {1:N1} s (N={2})" -f $mfr, $sw.Elapsed.TotalSeconds, $env:RANDAMU_FILL_RANDOM)
    if (Select-String -Path $log -Pattern "aerender ERROR|Unable to receive" -Quiet) { Write-Host "aerender ERROR (mfr=$mfr)" }
    $a = Get-ChildItem $outDir -Filter "r_probe_*.tif" | Select-Object -First 1
    $b = Get-ChildItem $outDir -Filter "r_ref_*.tif" | Select-Object -First 1
    if (-not $a -or -not $b) { Write-Host "missing output (mfr=$mfr)"; Get-Content $log -Tail 8; continue }
    Write-Host ("mfr={0}: {1}" -f $mfr, (& $py $cmp $a.FullName $b.FullName (Join-Path $outDir "diff_$mfr.png")))
    Copy-Item $a.FullName (Join-Path $outDir "aer_probe_$mfr.tif") -Force
    Select-String -Path $env:RANDAMU_SPIKE_LOG -Pattern "FILL (global|hooks|fx|count|rects|calls)" | Select-Object -First 6 |
        ForEach-Object { $_.Line.Substring($_.Line.IndexOf('FILL')) }
}
