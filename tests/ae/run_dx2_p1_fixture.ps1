# DupliX 2: a project saved by P1's build opens with this build and draws the same elements (tests/ae/dx2_p1_fixture.jsx).
#   pwsh tests/ae/run_dx2_p1_fixture.ps1 -Make   # with P1's aex: writes tests/ae/fixtures/dx2_p1.aep and dx2_p1_dump.txt
#   pwsh tests/ae/run_dx2_p1_fixture.ps1         # with this aex: opens it, renders the same frames, compares the dumps
# Prints "P1 FIXTURE PASS" (exit 0) or "P1 FIXTURE FAIL" with the first difference.
param([switch]$Make)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$fix = Join-Path $root "tests\ae\fixtures"
New-Item -ItemType Directory -Force $fix | Out-Null
$aep = Join-Path $fix "dx2_p1.aep"
$want = Join-Path $fix "dx2_p1_dump.txt"
$name = if ($Make) { "dx2_p1_make" } else { "dx2_p1_check" }
$out = Join-Path $root "build\test\$name"
New-Item -ItemType Directory -Force $out | Out-Null
$dump = Join-Path $out "dump.txt"
Remove-Item $dump -ErrorAction SilentlyContinue
$env:DUPLIX2_DUMP = $dump
$env:DUPLIX_FIXTURE_MODE = if ($Make) { "make" } else { "check" }
$env:DUPLIX_FIXTURE_AEP = $aep -replace '\\', '/'
& (Join-Path $PSScriptRoot "run_jsx.ps1") -Jsx tests/ae/dx2_p1_fixture.jsx -Name $name -Expect "^done"
$code = $LASTEXITCODE
Remove-Item env:DUPLIX2_DUMP, env:DUPLIX_FIXTURE_MODE, env:DUPLIX_FIXTURE_AEP -ErrorAction SilentlyContinue
if ($code -ne 0) { Write-Host "P1 FIXTURE FAIL: the script did not finish"; exit 1 }

# the dump as one block per time (the first block for each time: AE may render a frame more than once)
function Read-Blocks([string]$path) {
    $blocks = [ordered]@{}
    $cur = $null
    foreach ($l in Get-Content $path) {
        if ($l -match '^t (\S+) layer') { $cur = $Matches[1]; if ($blocks.Contains($cur)) { $cur = "#dup" } else { $blocks[$cur] = New-Object System.Collections.Generic.List[string] }; continue }
        if ($cur -and $cur -ne "#dup") { $blocks[$cur].Add($l) }
    }
    return $blocks
}
if ($Make) {
    Copy-Item $dump $want -Force
    $b = Read-Blocks $want
    Write-Host ("made {0} ({1} times: {2})" -f $aep, $b.Count, ($b.Keys -join ", "))
    exit 0
}
$a = Read-Blocks $want
$b = Read-Blocks $dump
foreach ($t in $a.Keys) {
    if (-not $b.Contains($t)) { Write-Host "P1 FIXTURE FAIL: no frame at t=$t"; exit 1 }
    $x = $a[$t]; $y = $b[$t]
    if ($x.Count -ne $y.Count) { Write-Host "P1 FIXTURE FAIL: t=$t has $($y.Count) lines, want $($x.Count)"; exit 1 }
    for ($i = 0; $i -lt $x.Count; $i++) {
        if ($x[$i] -ne $y[$i]) { Write-Host "P1 FIXTURE FAIL: t=$t line $($i + 1)`n  want $($x[$i])`n  got  $($y[$i])"; exit 1 }
    }
}
Write-Host ("P1 FIXTURE PASS ({0} frames the same)" -f $a.Count)
exit 0
