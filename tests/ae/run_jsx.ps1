# Run one ExtendScript in the hidden-desktop AE (docs/ae-test-harness.md) and print its log.
#   pwsh tests/ae/run_jsx.ps1 -Jsx tests/ae/smoke.jsx -Name smoke
# The script gets the output folder in $.getenv("RANDAMU_TEST_DIR") and must write jsx.log there,
# ending with a line "done" (or "EXCEPTION ..."). Plugin log: <out>\plugin.log (RANDAMU_LOG).
param([Parameter(Mandatory)][string]$Jsx, [Parameter(Mandatory)][string]$Name, [int]$TimeoutSec = 300,
      [string]$Expect = "",   # regex that must appear in jsx.log; sets the exit code
      [string]$Check = "")    # python script run as: <py> <Check> <out dir>; its exit code is ours
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$outDir = Join-Path $root "build\test\$Name"
New-Item -ItemType Directory -Force $outDir | Out-Null
Get-ChildItem "$outDir\*" -Include *.png, *.log, *.aep, *.tif -ErrorAction SilentlyContinue | Remove-Item -Confirm:$false
$env:RANDAMU_TEST_DIR = ($outDir -replace '\\', '/')
$env:RANDAMU_LOG = Join-Path $outDir "plugin.log"
$sf = "C:\Program Files\Adobe\Adobe After Effects 2025\Support Files"

. (Join-Path $root "spike\tools\ae_bg.ps1")
. (Join-Path $root "spike\tools\ae_guard.ps1")
$running = @(Get-Process AfterFX, aerender -ErrorAction SilentlyContinue)
if ($running) { throw "AfterFX/aerender is already running (pid $($running.Id -join ',')). Close it first." }
$null = Repair-AeCrashState

# AfterFX.exe -r cannot take a path with spaces or quotes: copy to %TEMP%.
$tmp = Join-Path $env:TEMP "randamu_test_$Name.jsx"
Copy-Item (Join-Path $root $Jsx) $tmp -Force
$p = Start-AeHidden -Exe (Join-Path $sf "AfterFX.exe") -Arguments "-r $tmp"
$jsxLog = Join-Path $outDir "jsx.log"
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$finished = $false
$dialog = $false
$seen = @{}
while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
    if ((Test-Path $jsxLog) -and (Select-String -Path $jsxLog -Pattern "^done|EXCEPTION" -Quiet)) { $finished = $true; break }
    # a message box (a C runtime assertion, Windows Error Reporting, ...) blocks AE: report it at once and stop.
    # AE's own titled windows (progress, alerts drawn by AE) are listed once for the record.
    $all = @(Get-AeHiddenDialogs)
    foreach ($d in $all | Where-Object { -not $_.Blocking }) {
        $key = "$($d.Pid)|$($d.Title)"
        if (-not $seen.ContainsKey($key)) { $seen[$key] = 1; Write-Host ("(window from {0}: '{1}')" -f $d.Process, $d.Title) }
    }
    if ($all | Where-Object Blocking) {
        Start-Sleep -Seconds 1  # let it finish drawing its text
        $k = 0
        foreach ($d in @(Get-AeHiddenDialogs | Where-Object Blocking)) {
            Write-Host ("DIALOG from {0} (pid {1}): '{2}': {3}" -f $d.Process, $d.Pid, $d.Title, $d.Text)
            foreach ($w in Get-AeHiddenWindows -ProcessId $d.Pid | Where-Object { $_.Visible -and $_.Class -eq "#32770" }) {
                $null = Save-AeWindowImage -Hwnd $w.Hwnd -Path (Join-Path $outDir "dialog_$k.png"); $k++
            }
        }
        $dialog = $true
        break
    }
    Start-Sleep -Seconds 2
}
if (-not $finished -and -not $p.HasExited -and -not $dialog) {
    Write-Host "TIMEOUT after $TimeoutSec s (AE hung or script did not finish)"
    # Diagnostics: AE's own visible windows (a modal dialog blocks the script); dialogs are saved as PNG.
    $k = 0
    foreach ($w in Get-AeHiddenWindows -ProcessId $p.Id | Where-Object Visible) {
        Write-Host ("  window class={0} title='{1}'" -f $w.Class, $w.Title)
        if ($w.Class -eq "#32770") { $null = Save-AeWindowImage -Hwnd $w.Hwnd -Path (Join-Path $outDir "dialog_$k.png"); $k++ }
    }
}
if (-not $p.HasExited) {
    if (-not (Stop-AeHidden -Process $p)) { $null = Repair-AeCrashState }
}
Remove-Item env:RANDAMU_LOG -ErrorAction SilentlyContinue
if (Test-Path $jsxLog) { Get-Content $jsxLog } else { "no jsx.log (AE exited=$($p.HasExited))" }
if ($dialog) { Write-Host "FAILED: a dialog appeared (see DIALOG above and dialog_*.png)"; exit 1 }
if ($Expect) {
    if (-not ((Test-Path $jsxLog) -and (Select-String -Path $jsxLog -Pattern $Expect -Quiet))) { Write-Host "EXPECT FAILED: $Expect"; exit 1 }
    Write-Host "EXPECT OK: $Expect"
}
if ($Check) {
    & (Join-Path $root "spike\.venv\Scripts\python.exe") -I (Join-Path $root $Check) $outDir
    exit $LASTEXITCODE
}
exit 0
