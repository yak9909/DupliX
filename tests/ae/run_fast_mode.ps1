# Fast mode checks: builds fast.aep with tests/ae/fast_mode.jsx (hidden AE), renders it with aerender
# (MFR off and on) and compares fast vs normal mode with check_fast_mode.py. Exit 1 on any failure.
#   pwsh tests/ae/run_fast_mode.ps1 [-Cases fast_identity,fast_scatter] [-Mfrs OFF,ON] [-SkipScene] [-Gpu CUDA]
# -Gpu: the project's GPU acceleration (a GpuAccelType name). SOFTWARE (default) renders on the CPU into
# build\test\fast; CUDA renders into build\test\fast_gpu and also compares with the CPU run (check_gpu_cpu.py,
# run the CPU run first) and checks that the GPU path was used.
param([string]$Cases = "", [string[]]$Mfrs = @("OFF", "ON"), [switch]$SkipScene, [int]$RenderTimeoutSec = 600,
      [string]$Gpu = "SOFTWARE")
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$name = if ($Gpu -eq "SOFTWARE") { "fast" } else { "fast_gpu" }
$outDir = Join-Path $root "build\test\$name"
$env:RANDAMU_FAST_GPU = $Gpu
$py = Join-Path $root "spike\.venv\Scripts\python.exe"
$check = Join-Path $PSScriptRoot "check_fast_mode.py"

if (-not $SkipScene) {
    $env:RANDAMU_FAST_IMAGE = Join-Path $root "spike\assets\probe_img.png"
    $env:RANDAMU_FAST_ONLY = $Cases
    & (Join-Path $PSScriptRoot "run_jsx.ps1") -Jsx tests/ae/fast_mode.jsx -Name $name -TimeoutSec 900 -Expect "^done" |
        Select-String "EXCEPTION|TIMEOUT|EXPECT|window class|half:"
    if ($LASTEXITCODE -ne 0) { Write-Host "scene build failed"; Get-Content (Join-Path $outDir "jsx.log") -Tail 5; exit 1 }
}

. (Join-Path $root "spike\tools\ae_guard.ps1")
$sf = "C:\Program Files\Adobe\Adobe After Effects 2025\Support Files"
$aep = Join-Path $outDir "fast.aep"
$fail = 0
foreach ($mfr in $Mfrs) {
    Get-ChildItem $outDir -Filter "r_*.tif" -ErrorAction SilentlyContinue | Remove-Item -Confirm:$false
    $null = Repair-AeCrashState
    $env:RANDAMU_LOG = Join-Path $outDir "plugin_aer_$mfr.log"
    if (Test-Path $env:RANDAMU_LOG) { Remove-Item $env:RANDAMU_LOG }
    $log = Join-Path $outDir "aerender_$mfr.log"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath (Join-Path $sf "aerender.exe") -PassThru -NoNewWindow -RedirectStandardOutput $log `
        -ArgumentList @("-project", "`"$aep`"", "-mfr", $mfr, "100", "-v", "ERRORS_AND_PROGRESS")
    if (-not $p.WaitForExit($RenderTimeoutSec * 1000)) { Write-Host "TIMEOUT aerender mfr=$mfr"; $p.Kill(); $fail++; continue }
    Remove-Item env:RANDAMU_LOG
    Write-Host ("aerender mfr={0}: {1:N1} s" -f $mfr, $sw.Elapsed.TotalSeconds)
    # fast_nohooks: its render queue item again with no hooks installed (fast mode must not need them)
    $rqFile = Join-Path $outDir "nohooks_rq.txt"
    if (Test-Path $rqFile) {
        $idx = (Get-Content $rqFile -Raw).Trim()
        Get-ChildItem $outDir -Filter "r_fm_fast_nohooks_*.tif" -ErrorAction SilentlyContinue | Remove-Item -Confirm:$false
        $null = Repair-AeCrashState
        $env:RANDAMU_HOOKMASK = "0"
        $p = Start-Process -FilePath (Join-Path $sf "aerender.exe") -PassThru -NoNewWindow `
            -RedirectStandardOutput (Join-Path $outDir "aerender_nohooks_$mfr.log") `
            -ArgumentList @("-project", "`"$aep`"", "-rqindex", $idx, "-mfr", $mfr, "100", "-v", "ERRORS_AND_PROGRESS")
        if (-not $p.WaitForExit($RenderTimeoutSec * 1000)) { Write-Host "TIMEOUT aerender nohooks"; $p.Kill(); $fail++ }
        Remove-Item env:RANDAMU_HOOKMASK
    }
    & $py -I $check $outDir $mfr $Cases
    if ($LASTEXITCODE -ne 0) { $fail++ }
    if ($Gpu -ne "SOFTWARE") {
        $gpuLines = @(Select-String -Path (Join-Path $outDir "plugin_aer_$mfr.log") -Pattern "fast: gpu render ok=1")
        $bad = @(Select-String -Path (Join-Path $outDir "plugin_aer_$mfr.log") -Pattern "fast: gpu render ok=0")
        Write-Host ("  GPU path used: {0} logged renders ok, {1} failed" -f $gpuLines.Count, $bad.Count)
        if ($gpuLines.Count -eq 0 -or $bad.Count -gt 0) { $fail++ }
        & $py -I (Join-Path $PSScriptRoot "check_gpu_cpu.py") (Join-Path $root "build\test\fast") $outDir $mfr
        if ($LASTEXITCODE -ne 0) { $fail++ }
    }
}
Write-Host $(if ($fail -eq 0) { "FAST ALL PASS" } else { "FAST FAILURES (runs): $fail" })
exit $(if ($fail -eq 0) { 0 } else { 1 })
