# A user's scene with the merge on and off (tests/ae/user_merge.jsx in one hidden AE on a COPY of the scene), then
# compares each frame (cmp_merge_frames.py: statistics and a difference image per frame).
#   pwsh tests/ae/run_user_merge.ps1 -Aep "tests/ae/user/DupliX Test A.aep" [-Frames 8] [-At "0,12,40"]
param([Parameter(Mandatory)][string]$Aep, [int]$Frames = 8, [string]$At = "", [string]$Disable = "", [string]$MoveZ = "")
$env:DUPLIX_UM_DISABLE = $Disable
$env:DUPLIX_UM_MOVEZ = $MoveZ
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$outDir = Join-Path $root "build\test\user_merge"
New-Item -ItemType Directory -Force $outDir | Out-Null
Get-ChildItem $outDir -File | Remove-Item -Confirm:$false
# Never open the user's file itself: AE saves a modified project when the test closes it.
$inDir = Join-Path $root "build\test\user_merge_in"  # run_jsx.ps1 empties $outDir
New-Item -ItemType Directory -Force $inDir | Out-Null
$copy = Join-Path $inDir "user_copy.aep"
Copy-Item (Resolve-Path $Aep).Path $copy -Force
$env:DUPLIX_USER_AEP = $copy -replace '\\', '/'
$env:DUPLIX_UM_FRAMES = "$Frames"
$env:DUPLIX_UM_AT = $At
& (Join-Path $PSScriptRoot "run_jsx.ps1") -Jsx tests/ae/user_merge.jsx -Name user_merge -TimeoutSec 1200 -Expect "^done" |
    Select-String "EXCEPTION|TIMEOUT|EXPECT"
Remove-Item env:DUPLIX_USER_AEP, env:DUPLIX_UM_FRAMES, env:DUPLIX_UM_AT -ErrorAction SilentlyContinue
Get-Content (Join-Path $outDir "jsx.log") | Select-String -NotMatch "^rendered"
& (Join-Path $root "spike\.venv\Scripts\python.exe") -I (Join-Path $PSScriptRoot "cmp_merge_frames.py") $outDir
