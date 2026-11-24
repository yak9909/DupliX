# Speed: renders probe_<case> (randamu, N copies) and ref_<case> (N real duplicate layers) with aerender,
# each comp separately, over a frame range, and prints the wall time and the hook profile.
#   pwsh tests/ae/run_perf.ps1 [-Frames 30] [-Gpu CUDA] [-SkipScene] [-Mfr OFF|ON]
#   -ProbeOnly: skip the ref comps (real layers)
param([int]$Frames = 30, [string]$Gpu = "", [switch]$SkipScene, [string]$Mfr = "ON", [string]$Cases = "",
      [switch]$ProbeOnly)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$outDir = Join-Path $root "build\test\perf"
New-Item -ItemType Directory -Force $outDir | Out-Null
$casesFile = Join-Path $PSScriptRoot "perf_cases.json"
$all = (Get-Content $casesFile -Raw -Encoding UTF8 | ConvertFrom-Json).cases
$sel = if ($Cases) { $Cases -split "," } else { @($all | ForEach-Object name) }
$run = @($all | Where-Object { $sel -contains $_.name })

if (-not $SkipScene) {
    $dump = Join-Path $root "build\dump\randamu_dump.exe"
    foreach ($c in $run) {
        $p = $c.params
        $dargs = @("--count", $p.count, "--seed", $(if ($p.seed) { $p.seed } else { 0 }))
        if ($p.pos) { $dargs += @("--pos") + $p.pos }
        if ($p.rot) { $dargs += @("--rot") + $p.rot }
        if ($p.scale) { $dargs += @("--scale") + $p.scale }
        if ($p.opacity) { $dargs += @("--opacity") + $p.opacity }
        & $dump @dargs | Set-Content -Encoding utf8NoBOM (Join-Path $outDir "copies_$($c.name).json")
    }
    $env:RANDAMU_REF_CASES = $casesFile
    $env:RANDAMU_REF_IMAGE = Join-Path $root "spike\assets\probe_img.png"
    $env:RANDAMU_REF_ONLY = $Cases
    $env:RANDAMU_REF_GPU = $Gpu
    & (Join-Path $PSScriptRoot "run_jsx.ps1") -Jsx tests/ae/make_ref.jsx -Name perf -TimeoutSec 900 -Expect "^done" |
        Select-String "EXCEPTION|TIMEOUT|EXPECT FAILED|gpu"
    if ($LASTEXITCODE -ne 0) { Write-Host "scene build failed"; exit 1 }
}

. (Join-Path $root "spike\tools\ae_guard.ps1")
$sf = "C:\Program Files\Adobe\Adobe After Effects 2025\Support Files"
$aep = Join-Path $outDir "ref.aep"
foreach ($c in $run) {
    foreach ($kind in $(if ($ProbeOnly) { @("probe") } else { @("probe", "ref") })) {
        $comp = "${kind}_$($c.name)"
        $null = Repair-AeCrashState
        $env:RANDAMU_LOG = Join-Path $outDir "plugin_$comp.log"
        if (Test-Path $env:RANDAMU_LOG) { Remove-Item $env:RANDAMU_LOG }
        Get-ChildItem $outDir -Filter "p_*.tif" -ErrorAction SilentlyContinue | Remove-Item -Confirm:$false
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $p = Start-Process -FilePath (Join-Path $sf "aerender.exe") -PassThru -NoNewWindow `
            -RedirectStandardOutput (Join-Path $outDir "aerender_$comp.log") `
            -ArgumentList @("-project", "`"$aep`"", "-comp", $comp, "-s", "0", "-e", ($Frames - 1), "-mfr", $Mfr, "100",
                            "-output", "`"$(Join-Path $outDir 'p_[#####].tif')`"", "-v", "ERRORS_AND_PROGRESS")
        if (-not $p.WaitForExit(600 * 1000)) { $p.Kill(); Write-Host "TIMEOUT $comp"; continue }
        $sec = $sw.Elapsed.TotalSeconds
        $n = @(Get-ChildItem $outDir -Filter "p_*.tif").Count
        Write-Host ("{0,-16} {1,7:N1} s for {2} frames ({3:N2} s/frame)" -f $comp, $sec, $n, ($sec / [Math]::Max($n, 1)))
        $prof = Select-String -Path $env:RANDAMU_LOG -Pattern "prof:" -ErrorAction SilentlyContinue | Select-Object -Last 2
        foreach ($l in $prof) { Write-Host ("    " + ($l.Line -split "\] ", 2)[-1]) }
        Remove-Item env:RANDAMU_LOG
    }
}
