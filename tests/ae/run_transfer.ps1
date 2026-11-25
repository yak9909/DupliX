# Fast mode switched on/off by script (plan 2026-11-25 Task 5): tests/ae/transfer.jsx in the hidden AE, then
# check_transfer.py. Exit 1 on any failure.   pwsh tests/ae/run_transfer.ps1
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$env:RANDAMU_FAST_IMAGE = Join-Path $root "spike\assets\probe_img.png"
& (Join-Path $PSScriptRoot "run_jsx.ps1") -Jsx tests/ae/transfer.jsx -Name transfer -TimeoutSec 400 -Check tests/ae/check_transfer.py |
    Select-String -NotMatch "^STATE"
exit $LASTEXITCODE
