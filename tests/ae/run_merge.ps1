# Merge optimisation checks: builds the scenes with tests/ae/merge.jsx in one hidden AE, which also renders every
# comp in every pass (default), and judges with check_merge.py. Exit 1 on any failure.
#   pwsh tests/ae/run_merge.ps1 [-Cases scene_basic,scene_light] [-Mfrs OFF,ON] [-CompareDraft] [-Gpu CUDA]
# -ForceD "x,y,z": the seam comps' group planes at the camera depth of that world point (RANDAMU_MERGE_FORCE_DW)
# -CompareDraft: one more pass (MFR off, the draft quality of all-group layers off) that must equal the OFF pass
# -Aerender [-SkipScene] [-Parallel 4]: the old way, one aerender per comp (AE's own command-line renderer)
param([string]$Cases = "", [string[]]$Mfrs = @("OFF"), [switch]$SkipScene, [int]$RenderTimeoutSec = 600,
      [string]$Gpu = "", [string]$ForceD = "", [int]$Parallel = 4, [switch]$Aerender, [switch]$CompareDraft)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$outDir = Join-Path $root "build\test\merge"
$py = Join-Path $root "spike\.venv\Scripts\python.exe"
$check = Join-Path $PSScriptRoot "check_merge.py"

# -Mfrs OFF,ON from bash arrives as one string: split it
$Mfrs = @($Mfrs | ForEach-Object { $_ -split "," } | Where-Object { $_ })

if (-not $Aerender) {
    # One AE for everything (default): merge.jsx builds the scenes and renders each comp in every pass itself
    # (per-render plugin log / cache salt / environment through $.setenv). ND = MFR off with the draft quality
    # of all-group layers off: every frame must equal the OFF pass.
    $passes = @($Mfrs) + $(if ($CompareDraft) { @("ND") } else { @() })
    $env:RANDAMU_MERGE_IMAGE = Join-Path $root "spike\assets\probe_img.png"
    $env:RANDAMU_MERGE_ONLY = $Cases
    $env:RANDAMU_MERGE_GPU = $Gpu
    $env:RANDAMU_MERGE_PASSES = $passes -join ","
    $env:RANDAMU_MERGE_FORCE_SEAM = $ForceD
    New-Item -ItemType Directory -Force $outDir | Out-Null
    $env:RANDAMU_LOG = Join-Path $outDir "plugin_build.log"  # logging on; the script switches the file per render
    Get-ChildItem $outDir -Filter "*.tif" -ErrorAction SilentlyContinue | Remove-Item -Confirm:$false
    $t0 = Get-Date
    & (Join-Path $PSScriptRoot "run_jsx.ps1") -Jsx tests/ae/merge.jsx -Name merge -TimeoutSec 1800 -Expect "^done" |
        Select-String "EXCEPTION|TIMEOUT|EXPECT"
    $jsxFail = $LASTEXITCODE -ne 0
    Remove-Item env:RANDAMU_LOG, env:RANDAMU_MERGE_PASSES, env:RANDAMU_MERGE_FORCE_SEAM -ErrorAction SilentlyContinue
    Write-Host ("one AE: {0:N0} s" -f ((Get-Date) - $t0).TotalSeconds)
    if ($jsxFail) { Write-Host "scene build / render failed"; Get-Content (Join-Path $outDir "jsx.log") -Tail 5; exit 1 }
    Get-Content (Join-Path $outDir "jsx.log") | Select-String "no output|EXCEPTION"
    $fail = 0
    foreach ($mfr in $Mfrs) {
        & $py -I $check $outDir $mfr $Cases
        if ($LASTEXITCODE -ne 0) { $fail++ }
    }
    if ($CompareDraft) {
        & $py -I (Join-Path $PSScriptRoot "cmp_dirs.py") $outDir $outDir --suffix OFF ND
        if ($LASTEXITCODE -ne 0) { $fail++ }
    }
    Write-Host $(if ($fail -eq 0) { "MERGE ALL PASS" } else { "MERGE FAILURES (runs): $fail" })
    exit $(if ($fail -eq 0) { 0 } else { 1 })
}

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
# (-Aerender) each comp renders in its own aerender (its own plugin log, environment and fresh process: the plugin's log
# limits and caches start empty). Almost all of an aerender run is AE's start-up (~40 s for < 1 s of rendering),
# so up to -Parallel run at once, each on its own copy of the project.
$slots = [math]::Max(1, $Parallel)
for ($i = 0; $i -lt $slots; $i++) { Copy-Item $aep (Join-Path $outDir "merge_s$i.aep") -Force }
foreach ($mfr in $Mfrs) {
    $null = Repair-AeCrashState
    $queue = [System.Collections.Generic.Queue[string]]::new([string[]]@($items.Keys | Sort-Object))
    $running = @{}  # slot -> @{ p; comp; start }
    while ($queue.Count -gt 0 -or $running.Count -gt 0) {
        for ($s = 0; $s -lt $slots -and $queue.Count -gt 0; $s++) {
            if ($running.ContainsKey($s)) { continue }
            $comp = $queue.Dequeue()
            Get-ChildItem $outDir -Filter "r_${comp}_*.tif" -ErrorAction SilentlyContinue | Remove-Item -Confirm:$false
            $log = Join-Path $outDir "plugin_${comp}_$mfr.log"
            if (Test-Path $log) { Remove-Item $log }
            $envs = @{ RANDAMU_LOG = $log; RANDAMU_CACHE_SALT = "merge-$(Get-Random)" }
            if ($ForceD -and $comp -like "*_seam") { $envs["RANDAMU_MERGE_FORCE_DW"] = $ForceD }  # the seam case only
            $p = Start-Process -FilePath (Join-Path $sf "aerender.exe") -PassThru -NoNewWindow `
                -RedirectStandardOutput (Join-Path $outDir "aerender_${comp}_$mfr.log") `
                -Environment $envs `
                -ArgumentList @("-project", "`"$(Join-Path $outDir "merge_s$s.aep")`"", "-rqindex", $items[$comp], "-mfr", $mfr, "100",
                                "-v", "ERRORS_AND_PROGRESS")
            $running[$s] = @{ p = $p; comp = $comp; start = Get-Date }
        }
        Start-Sleep -Milliseconds 500
        foreach ($s in @($running.Keys)) {
            $r = $running[$s]
            if ($r.p.HasExited) {
                $tif = Get-ChildItem $outDir -Filter "r_$($r.comp)_*.tif" | Select-Object -First 1
                if ($tif) { Copy-Item $tif.FullName (Join-Path $outDir "$($r.comp)_$mfr.tif") -Force }
                $running.Remove($s)
            } elseif (((Get-Date) - $r.start).TotalSeconds -gt $RenderTimeoutSec) {
                Write-Host "TIMEOUT aerender $($r.comp) mfr=$mfr"; $r.p.Kill(); $fail++; $running.Remove($s)
            }
        }
    }
    & $py -I $check $outDir $mfr $Cases
    if ($LASTEXITCODE -ne 0) { $fail++ }
}
Write-Host $(if ($fail -eq 0) { "MERGE ALL PASS" } else { "MERGE FAILURES (runs): $fail" })
exit $(if ($fail -eq 0) { 0 } else { 1 })
