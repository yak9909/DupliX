# Merge optimisation checks: builds merge.aep with tests/ae/merge.jsx (hidden AE), renders each render queue item
# with aerender (its own plugin log, a fresh cache salt) and judges with check_merge.py. Exit 1 on any failure.
#   pwsh tests/ae/run_merge.ps1 [-Cases scene_basic,scene_light] [-Mfrs OFF,ON] [-SkipScene] [-Gpu CUDA]
# -ForceD "x,y,z": every group plane at the camera depth of that world point (RANDAMU_MERGE_FORCE_DW; the seam case)
param([string]$Cases = "", [string[]]$Mfrs = @("OFF"), [switch]$SkipScene, [int]$RenderTimeoutSec = 600,
      [string]$Gpu = "", [string]$ForceD = "")
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$outDir = Join-Path $root "build\test\merge"
$py = Join-Path $root "spike\.venv\Scripts\python.exe"
$check = Join-Path $PSScriptRoot "check_merge.py"

if (-not $SkipScene) {
    $env:RANDAMU_MERGE_IMAGE = Join-Path $root "spike\assets\probe_img.png"
    $env:RANDAMU_MERGE_ONLY = $Cases
    $env:RANDAMU_MERGE_GPU = $Gpu
    & (Join-Path $PSScriptRoot "run_jsx.ps1") -Jsx tests/ae/merge.jsx -Name merge -TimeoutSec 900 -Expect "^done" |
        Select-String "EXCEPTION|TIMEOUT|EXPECT"
    if ($LASTEXITCODE -ne 0) { Write-Host "scene build failed"; Get-Content (Join-Path $outDir "jsx.log") -Tail 5; exit 1 }
}

. (Join-Path $root "spike\tools\ae_guard.ps1")
$sf = "C:\Program Files\Adobe\Adobe After Effects 2025\Support Files"
$aep = Join-Path $outDir "merge.aep"
$items = @{}
$sel = if ($Cases) { $Cases -split "," } else { @() }
foreach ($line in Get-Content (Join-Path $outDir "rq.txt")) {
    $k, $v = $line -split "=", 2
    if ($sel.Count -eq 0 -or $sel -contains ($k -replace '^(mg|nm)_', '')) { $items[$k] = $v }
}
$fail = 0
foreach ($mfr in $Mfrs) {
    foreach ($comp in $items.Keys | Sort-Object) {
        Get-ChildItem $outDir -Filter "r_${comp}_*.tif" -ErrorAction SilentlyContinue | Remove-Item -Confirm:$false
        $null = Repair-AeCrashState
        $log = Join-Path $outDir "plugin_${comp}_$mfr.log"
        if (Test-Path $log) { Remove-Item $log }
        $envs = @{ RANDAMU_LOG = $log; RANDAMU_CACHE_SALT = "merge-$(Get-Random)" }
        if ($ForceD -and $comp -like "*_seam") { $envs["RANDAMU_MERGE_FORCE_DW"] = $ForceD }  # the seam case only
        $p = Start-Process -FilePath (Join-Path $sf "aerender.exe") -PassThru -NoNewWindow `
            -RedirectStandardOutput (Join-Path $outDir "aerender_${comp}_$mfr.log") `
            -Environment $envs `
            -ArgumentList @("-project", "`"$aep`"", "-rqindex", $items[$comp], "-mfr", $mfr, "100", "-v", "ERRORS_AND_PROGRESS")
        if (-not $p.WaitForExit($RenderTimeoutSec * 1000)) { Write-Host "TIMEOUT aerender $comp mfr=$mfr"; $p.Kill(); $fail++; continue }
        $tif = Get-ChildItem $outDir -Filter "r_${comp}_*.tif" | Select-Object -First 1
        if ($tif) { Copy-Item $tif.FullName (Join-Path $outDir "${comp}_$mfr.tif") -Force }
    }
    & $py -I $check $outDir $mfr $Cases
    if ($LASTEXITCODE -ne 0) { $fail++ }
}
Write-Host $(if ($fail -eq 0) { "MERGE ALL PASS" } else { "MERGE FAILURES (runs): $fail" })
exit $(if ($fail -eq 0) { 0 } else { 1 })
