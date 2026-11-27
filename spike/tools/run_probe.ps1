# THROWAWAY SPIKE: run one ExtendScript probe in the hidden-desktop AE and print its log.
# usage: run_probe.ps1 -Jsx scenes\probe_sets.jsx -Name sets
param([Parameter(Mandatory)][string]$Jsx, [Parameter(Mandatory)][string]$Name, [int]$TimeoutSec = 300)
$ErrorActionPreference = "Stop"
$root   = Resolve-Path (Join-Path $PSScriptRoot "..")
$outDir = Join-Path $root "logs\probe_$Name"
New-Item -ItemType Directory -Force $outDir | Out-Null
Get-ChildItem "$outDir\*" -Include *.png, *.log, *.aep -ErrorAction SilentlyContinue | Remove-Item -Confirm:$false
$env:DUPLIX_SPIKE_DIR = ($outDir -replace '\\', '/')
$sf = "C:\Program Files\Adobe\Adobe After Effects 2025\Support Files"

. (Join-Path $PSScriptRoot "ae_bg.ps1")
. (Join-Path $PSScriptRoot "ae_guard.ps1")
$running = @(Get-Process AfterFX, aerender -ErrorAction SilentlyContinue)
if ($running) { throw "AfterFX/aerender is already running (pid $($running.Id -join ',')). Close it first." }
$null = Repair-AeCrashState

$tmp = Join-Path $env:TEMP "duplix_probe_$Name.jsx"
Copy-Item (Join-Path $root $Jsx) $tmp -Force
$p = Start-AeHidden -Exe (Join-Path $sf "AfterFX.exe") -Arguments "-r $tmp"
$jsxLog = Join-Path $outDir "jsx.log"
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
    if ((Test-Path $jsxLog) -and (Select-String -Path $jsxLog -Pattern "^done|EXCEPTION" -Quiet)) { break }
    Start-Sleep -Seconds 2
}
if (-not $p.HasExited) {
    if (-not (Stop-AeHidden -Process $p)) { $null = Repair-AeCrashState }
}
if (Test-Path $jsxLog) { Get-Content $jsxLog } else { "no jsx.log (AE exited=$($p.HasExited))" }
