# Reference comparison: probe_<case> (randamu) vs ref_<case> (real duplicate layers), rendered by
# aerender with MFR off and on, plus a reopen check (saveFrameToPng after app.open). Exit 1 on any failure.
#   pwsh tests/ae/run_ref.ps1 [-Cases baseline,scale] [-SkipScene] [-Mfrs OFF,ON]
param([string]$Cases = "", [switch]$SkipScene, [string[]]$Mfrs = @("OFF", "ON"), [int]$RenderTimeoutSec = 600)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$outDir = Join-Path $root "build\test\ref"
New-Item -ItemType Directory -Force $outDir | Out-Null
$py = Join-Path $root "spike\.venv\Scripts\python.exe"
$cmp = Join-Path $PSScriptRoot "cmp.py"
$casesFile = Join-Path $PSScriptRoot "ref_cases.json"
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
        if ($p.keepFirst) { $dargs += "--keep-first" }
        & $dump @dargs | Set-Content -Encoding utf8NoBOM (Join-Path $outDir "copies_$($c.name).json")
    }
    $env:RANDAMU_REF_CASES = $casesFile
    $env:RANDAMU_REF_IMAGE = Join-Path $root "spike\assets\probe_img.png"
    $env:RANDAMU_REF_ONLY = $Cases
    & (Join-Path $PSScriptRoot "run_jsx.ps1") -Jsx tests/ae/make_ref.jsx -Name ref -TimeoutSec 900 -Expect "^done" |
        Select-String "EXCEPTION|TIMEOUT|EXPECT|window class"
    if ($LASTEXITCODE -ne 0) { Write-Host "scene build failed"; Get-Content (Join-Path $outDir "jsx.log") -Tail 5; exit 1 }
}

. (Join-Path $root "spike\tools\ae_guard.ps1")
$sf = "C:\Program Files\Adobe\Adobe After Effects 2025\Support Files"
$fail = 0
$aep = Join-Path $outDir "ref.aep"
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
    Write-Host ("aerender mfr={0}: {1:N1} s" -f $mfr, $sw.Elapsed.TotalSeconds)
    Remove-Item env:RANDAMU_LOG
    foreach ($c in $run) {
        # frame number in the name depends on the case's time
        $a = Get-ChildItem $outDir -Filter ("r_probe_{0}_*.tif" -f $c.name) | Select-Object -First 1 -ExpandProperty FullName
        $b = Get-ChildItem $outDir -Filter ("r_ref_{0}_*.tif" -f $c.name) | Select-Object -First 1 -ExpandProperty FullName
        if (-not $a -or -not $b) { Write-Host ("  {0,-12} MISSING output" -f $c.name); $fail++; continue }
        $r = & $py -I $cmp $a $b (Join-Path $outDir ("diff_{0}_{1}.png" -f $c.name, $mfr))
        $ok = $LASTEXITCODE -eq 0
        if (-not $ok) { $fail++ }
        Write-Host ("  {0,-12} {1} {2}" -f $c.name, $(if ($ok) { "PASS" } else { "FAIL" }), $r)
        Copy-Item $a (Join-Path $outDir ("aer_probe_{0}_{1}.tif" -f $c.name, $mfr)) -Force
    }
}
if (-not $SkipScene -and ($sel -contains "baseline")) {
    $r = & $py -I $cmp (Join-Path $outDir "reopen_probe.png") (Join-Path $outDir "reopen_ref.png") (Join-Path $outDir "diff_reopen.png")
    $ok = $LASTEXITCODE -eq 0
    if (-not $ok) { $fail++ }
    Write-Host ("  {0,-12} {1} {2}" -f "reopen", $(if ($ok) { "PASS" } else { "FAIL" }), $r)
}
Write-Host $(if ($fail -eq 0) { "REF ALL PASS" } else { "REF FAILURES: $fail" })
exit $(if ($fail -eq 0) { 0 } else { 1 })
