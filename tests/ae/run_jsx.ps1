# Run one ExtendScript in the hidden-desktop AE (docs/ae-test-harness.md) and print its log.
#   pwsh tests/ae/run_jsx.ps1 -Jsx tests/ae/smoke.jsx -Name smoke
# The script gets the output folder in $.getenv("RANDAMU_TEST_DIR") and must write jsx.log there,
# ending with a line "done" (or "EXCEPTION ..."). Plugin log: <out>\plugin.log (RANDAMU_LOG).
param([Parameter(Mandatory)][string]$Jsx, [Parameter(Mandatory)][string]$Name, [int]$TimeoutSec = 300,
      [string]$Expect = "")   # regex that must appear in jsx.log; sets the exit code
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
while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
    if ((Test-Path $jsxLog) -and (Select-String -Path $jsxLog -Pattern "^done|EXCEPTION" -Quiet)) { $finished = $true; break }
    Start-Sleep -Seconds 2
}
if (-not $finished -and -not $p.HasExited) { Write-Host "TIMEOUT after $TimeoutSec s (AE hung or script did not finish)" }
if (-not $p.HasExited) {
    if (-not (Stop-AeHidden -Process $p)) { $null = Repair-AeCrashState }
}
Remove-Item env:RANDAMU_LOG -ErrorAction SilentlyContinue
if (Test-Path $jsxLog) { Get-Content $jsxLog } else { "no jsx.log (AE exited=$($p.HasExited))" }
if ($Expect) {
    if ((Test-Path $jsxLog) -and (Select-String -Path $jsxLog -Pattern $Expect -Quiet)) { Write-Host "EXPECT OK: $Expect"; exit 0 }
    Write-Host "EXPECT FAILED: $Expect"; exit 1
}
