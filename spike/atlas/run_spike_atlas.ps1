# THROWAWAY spike runner (merge optimisation): builds atlas.aep (tests/ae/spike_atlas.jsx) and renders it once per
# DUPLIX_SPIKE_ATLAS mode with aerender (MFR off), saving build\test\atlas\m_<mode>.png and plugin_<mode>.log.
#   pwsh tests/ae/run_spike_atlas.ps1 -Modes off,expand,rect [-SkipScene]
param([string[]]$Modes = @("off"), [switch]$SkipScene)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$outDir = Join-Path $root "build\test\atlas"
$py = Join-Path $root "spike\.venv\Scripts\python.exe"
if (-not $SkipScene) {
    $env:DUPLIX_SPIKE_IMAGE = Join-Path $root "spike\assets\probe_img.png"
    & (Join-Path $PSScriptRoot "run_jsx.ps1") -Jsx tests/ae/spike_atlas.jsx -Name atlas -TimeoutSec 600 -Expect "^done" |
        Select-String "EXCEPTION|TIMEOUT|EXPECT"
    if ($LASTEXITCODE -ne 0) { Write-Host "scene build failed"; exit 1 }
}
. (Join-Path $root "spike\tools\ae_guard.ps1")
$sf = "C:\Program Files\Adobe\Adobe After Effects 2025\Support Files"
$aep = Join-Path $outDir "atlas.aep"
foreach ($m in ($Modes -join "," -split ",")) {
    Get-ChildItem $outDir -Filter "r_*.tif" -ErrorAction SilentlyContinue | Remove-Item -Confirm:$false
    $null = Repair-AeCrashState
    $env:DUPLIX_SPIKE_ATLAS = $m
    $env:DUPLIX_CACHE_SALT = "$m-$(Get-Random)"
    $env:DUPLIX_LOG = Join-Path $outDir "plugin_$m.log"
    if (Test-Path $env:DUPLIX_LOG) { Remove-Item $env:DUPLIX_LOG }
    $p = Start-Process -FilePath (Join-Path $sf "aerender.exe") -PassThru -NoNewWindow `
        -RedirectStandardOutput (Join-Path $outDir "aerender_$m.log") `
        -ArgumentList @("-project", "`"$aep`"", "-mfr", "OFF", "100", "-v", "ERRORS_AND_PROGRESS")
    if (-not $p.WaitForExit(300000)) { Write-Host "TIMEOUT $m"; $p.Kill(); continue }
    Remove-Item env:DUPLIX_SPIKE_ATLAS, env:DUPLIX_CACHE_SALT, env:DUPLIX_LOG
    $tif = Get-ChildItem $outDir -Filter "r_atlas_*.tif" | Select-Object -First 1 -ExpandProperty FullName
    if (-not $tif) { Write-Host "$m : no output"; continue }
    & $py -I -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])" $tif (Join-Path $outDir "m_$m.png")
    Write-Host "$m : $(Join-Path $outDir "m_$m.png")"
}
