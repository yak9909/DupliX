# Times a user-provided scene (tests/ae/user/*.aep) under variants built by user_scene.jsx:
# aerender per variant comp over -Frames frames; prints AE's own elapsed render time (startup excluded).
#   pwsh tests/ae/run_user.ps1 -Aep "tests/ae/user/DupliX Test.aep" [-Frames 120] [-Mfr ON] [-SkipScene]
param([Parameter(Mandatory)][string]$Aep, [int]$Frames = 120, [string]$Mfr = "ON", [switch]$SkipScene,
      [string]$Variants = "v_asis,v_count1,v_count0,v_plain,v_real10,v_real10_fx")
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$outDir = Join-Path $root "build\test\userscene"
if (-not $SkipScene) {
    # Never open the user's file itself: AE saves a modified project when the test closes it.
    $inDir = Join-Path $root "build\test\userscene_in"
    New-Item -ItemType Directory -Force $inDir | Out-Null
    $copy = Join-Path $inDir "user_copy.aep"
    Copy-Item (Resolve-Path $Aep).Path $copy -Force
    $env:RANDAMU_USER_AEP = $copy -replace '\\', '/'
    & (Join-Path $PSScriptRoot "run_jsx.ps1") -Jsx tests/ae/user_scene.jsx -Name userscene -TimeoutSec 400 -Expect "^done" |
        Select-String "comp |layer |EXCEPTION|TIMEOUT|EXPECT"
    Remove-Item env:RANDAMU_USER_AEP
    if ($LASTEXITCODE -ne 0) { exit 1 }
}
. (Join-Path $root "spike\tools\ae_guard.ps1")
$sf = "C:\Program Files\Adobe\Adobe After Effects 2025\Support Files"
$enc = [Text.Encoding]::GetEncoding(932)
foreach ($v in $Variants -split ",") {
    $null = Repair-AeCrashState
    $log = Join-Path $outDir "aerender_$v.log"
    Get-ChildItem $outDir -Filter "u_*.png" -ErrorAction SilentlyContinue | Remove-Item -Confirm:$false
    $env:RANDAMU_LOG = Join-Path $outDir "plugin_$v.log"
    if (Test-Path $env:RANDAMU_LOG) { Remove-Item $env:RANDAMU_LOG }
    $p = Start-Process -FilePath (Join-Path $sf "aerender.exe") -PassThru -NoNewWindow -RedirectStandardOutput $log `
        -ArgumentList @("-project", "`"$(Join-Path $outDir 'variants.aep')`"", "-comp", $v, "-s", "0", "-e", ($Frames - 1),
                        "-mfr", $Mfr, "100",
                        "-output", "`"$(Join-Path $outDir 'u_[#####].tif')`"", "-v", "ERRORS_AND_PROGRESS")
    if (-not $p.WaitForExit(600 * 1000)) { $p.Kill(); Write-Host "TIMEOUT $v"; continue }
    Remove-Item env:RANDAMU_LOG
    Start-Sleep -Milliseconds 500  # aerender's child may still hold the redirected log
    $fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    $t = (New-Object IO.StreamReader($fs, $enc)).ReadToEnd()
    $fs.Close()
    $m = [regex]::Match($t, "合計経過時間 : ([^\r\n]+)")
    $n = ([regex]::Matches($t, "\((\d+)\): ")).Count
    Write-Host ("{0,-14} {1} frames  render {2}" -f $v, $n, $(if ($m.Success) { $m.Groups[1].Value } else { "(no total)" }))
}
