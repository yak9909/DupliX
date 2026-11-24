# UI session test (Task 7). Runs tests/ae/ui_session.jsx in the hidden-desktop AE (UI on); if it does not
# finish in time AE is judged hung and killed. Then prints the last STEP, the hook's thread statistics,
# the PNG checks (check_ui.py), and compares the keyframed UI frame with aerender (cmp.py).
#   pwsh tests/ae/run_ui.ps1 [-TimeoutSec 180]
param([int]$TimeoutSec = 180, [int]$RenderTimeoutSec = 300)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$outDir = Join-Path $root "build\test\ui"
$py = Join-Path $root "spike\.venv\Scripts\python.exe"
$fail = 0

$sw = [Diagnostics.Stopwatch]::StartNew()
$jsxOut = & (Join-Path $PSScriptRoot "run_jsx.ps1") -Jsx tests/ae/ui_session.jsx -Name ui -TimeoutSec $TimeoutSec -Expect "^done"
$jsxOk = $LASTEXITCODE -eq 0
Write-Host ("UI session: {0:N1} s, {1}" -f $sw.Elapsed.TotalSeconds, $(if ($jsxOk) { "finished" } else { "FAILED / HUNG" }))
$jsxOut | Select-String "TIMEOUT|EXCEPTION|window class|after undo" | ForEach-Object { Write-Host "  $_" }
$jsxLog = Join-Path $outDir "jsx.log"
$last = if (Test-Path $jsxLog) { Select-String -Path $jsxLog -Pattern "^STEP" | Select-Object -Last 1 } else { $null }
Write-Host ("  last step: {0}" -f $(if ($last) { $last.Line } else { "(none)" }))
if (-not $jsxOk) { $fail++ }

$plog = Join-Path $outDir "plugin.log"
if (Test-Path $plog) {
    $st = Select-String -Path $plog -Pattern "stats: ParamsFor" | Select-Object -Last 1
    Write-Host ("  hook thread stats: {0}" -f $(if ($st) { $st.Line } else { "(no queries logged)" }))
    $fb = @(Select-String -Path $plog -Pattern "AEGP param read failed").Count
    Write-Host "  AEGP read failures (fell back to snapshot): $fb"
}

if ($jsxOk) {
    & $py -I (Join-Path $PSScriptRoot "check_ui.py") $outDir
    if ($LASTEXITCODE -ne 0) { $fail++ }

    # keyframed rotation range: UI frame (saveFrameToPng) vs aerender
    . (Join-Path $root "spike\tools\ae_guard.ps1")
    $null = Repair-AeCrashState
    Get-ChildItem $outDir -Filter "r_kf_*.tif" -ErrorAction SilentlyContinue | Remove-Item -Confirm:$false
    $sf = "C:\Program Files\Adobe\Adobe After Effects 2025\Support Files"
    $env:RANDAMU_LOG = Join-Path $outDir "plugin_aer.log"
    $p = Start-Process -FilePath (Join-Path $sf "aerender.exe") -PassThru -NoNewWindow `
        -RedirectStandardOutput (Join-Path $outDir "aerender.log") `
        -ArgumentList @("-project", "`"$(Join-Path $outDir 'ui.aep')`"", "-v", "ERRORS_AND_PROGRESS")
    if (-not $p.WaitForExit($RenderTimeoutSec * 1000)) { Write-Host "TIMEOUT aerender"; $p.Kill(); $fail++ }
    Remove-Item env:RANDAMU_LOG
    $tif = Get-ChildItem $outDir -Filter "r_kf_*.tif" | Select-Object -First 1 -ExpandProperty FullName
    if (-not $tif) { Write-Host "  kf aerender output MISSING"; $fail++ }
    else {
        $r = & $py -I (Join-Path $PSScriptRoot "cmp.py") (Join-Path $outDir "kf_ui.png") $tif (Join-Path $outDir "diff_kf.png")
        $ok = $LASTEXITCODE -eq 0
        if (-not $ok) { $fail++ }
        Write-Host ("  {0,-28} {1} {2}" -f "keyframed UI == aerender", $(if ($ok) { "PASS" } else { "FAIL" }), $r)
    }
}
Write-Host $(if ($fail -eq 0) { "UI ALL PASS" } else { "UI FAILURES: $fail" })
exit $(if ($fail -eq 0) { 0 } else { 1 })
