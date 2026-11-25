# Times a user's scene per variant (tests/ae/user_time.jsx in one hidden AE on a COPY of the scene, MFR off).
#   pwsh tests/ae/run_user_time.ps1 -Aep "tests/ae/user/DupliX Test A.aep" [-Frames 30] [-Start 0] [-Variants asis,on,off,none]
param([Parameter(Mandatory)][string]$Aep, [int]$Frames = 30, [int]$Start = 0, [string]$Variants = "asis,on,off,none")
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$outDir = Join-Path $root "build\test\user_time"
# Never open the user's file itself: AE saves a modified project when the test closes it.
$inDir = Join-Path $root "build\test\user_time_in"  # run_jsx.ps1 empties $outDir
New-Item -ItemType Directory -Force $inDir | Out-Null
$copy = Join-Path $inDir "user_copy.aep"
Copy-Item (Resolve-Path $Aep).Path $copy -Force
$env:RANDAMU_USER_AEP = $copy -replace '\\', '/'
$env:RANDAMU_UT_FRAMES = "$Frames"
$env:RANDAMU_UT_START = "$Start"
$env:RANDAMU_UT_VARIANTS = $Variants
& (Join-Path $PSScriptRoot "run_jsx.ps1") -Jsx tests/ae/user_time.jsx -Name user_time -TimeoutSec 1800 -Expect "^done" |
    Select-String "EXCEPTION|TIMEOUT|EXPECT"
Remove-Item env:RANDAMU_USER_AEP, env:RANDAMU_UT_FRAMES, env:RANDAMU_UT_START, env:RANDAMU_UT_VARIANTS -ErrorAction SilentlyContinue
Get-Content (Join-Path $outDir "jsx.log")
