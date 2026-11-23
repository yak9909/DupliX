# THROWAWAY SPIKE: keep AE's "クラッシュ修復オプション" (crash repair options) dialog from
# blocking automated launches. Dot-source this file.
#
# How AE detects a crash (found by diffing the registry, AE 25.6):
#   HKCU\Software\Adobe\After Effects\AppStates\<session guid>
#     AppState  = 3     while that AE session is running
#     AppState  = 1001  written on a clean exit
#     ProcessID = pid of that session (QWORD)
#   A session killed with Stop-Process keeps AppState=3. The next launch finds it and
#   shows the crash dialog (modal; the -r script waits behind it and runs once the dialog
#   is dismissed with 続行).
#
# Repair-AeCrashState marks such stale sessions as cleanly exited before we launch AE, so
# the dialog is never shown. Sessions whose process is still alive are left untouched
# (that could be the user's own AE).

$script:AeAppStatesKey = "HKCU:\Software\Adobe\After Effects\AppStates"

function Get-AeStaleSessions {
    if (-not (Test-Path $script:AeAppStatesKey)) { return }
    foreach ($k in Get-ChildItem $script:AeAppStatesKey) {
        $v = Get-ItemProperty $k.PSPath
        if ($null -eq $v.AppState -or $v.AppState -eq 1001) { continue }
        $alive = $false
        if ($v.ProcessID) {
            $proc = Get-Process -Id ([int]$v.ProcessID) -ErrorAction SilentlyContinue
            $alive = [bool]($proc -and $proc.ProcessName -eq "AfterFX")
        }
        if (-not $alive) { [pscustomobject]@{ Key = $k.PSPath; Session = $k.PSChildName; AppState = $v.AppState; Pid = $v.ProcessID } }
    }
}

# Second dialog: "<effect> が原因で After Effects が終了した可能性があります" (492x295, buttons
# エフェクトを無効にする / 有効のままにする). When AE crashes inside an effect it writes
#   %APPDATA%\Adobe\After Effects\<ver>\Effect Crashed.txt
# (UTF-16, "Crashed Effect File Version 1.1 ... <match name> <effect name>"). The next launch
# reads it, deletes it and shows the dialog (found by diffing AE's files right after a crash).
# Remove-AeEffectCrashRecord deletes the file only if it names one of OUR effects, so a crash
# record about some other plug-in (the user's) is left for AE to show.
$script:AeOwnEffectMatchNames = @("RandamuSpike", "randamu.aex", "RANDAMU Scatter")

function Remove-AeEffectCrashRecord {
    $n = 0
    foreach ($f in Get-ChildItem "$env:APPDATA\Adobe\After Effects\*\Effect Crashed.txt" -ErrorAction SilentlyContinue) {
        # The file is UTF-16 big-endian without a BOM; fields are separated by NULs, not spaces.
        $bytes = [IO.File]::ReadAllBytes($f.FullName)
        $text = [Text.Encoding]::BigEndianUnicode.GetString($bytes) + "`n" + [Text.Encoding]::Unicode.GetString($bytes)
        $ours = $false
        foreach ($m in $script:AeOwnEffectMatchNames) { if ($text.Contains($m)) { $ours = $true } }
        if ($ours) {
            Write-Host "[guard] removing crash record of our effect: $($f.FullName)"
            Remove-Item -LiteralPath $f.FullName -Force -Confirm:$false
            $n++
        } else {
            Write-Host "[guard] WARNING: crash record of another effect left in place: $($f.FullName)"
        }
    }
    return $n
}

# Returns the number of sessions that were marked clean (plus removed effect crash records).
function Repair-AeCrashState {
    $n = Remove-AeEffectCrashRecord
    foreach ($s in Get-AeStaleSessions) {
        Write-Host ("[guard] previous AE session {0} (pid {1}) ended without a clean exit (AppState={2}) -> marking clean" -f $s.Session, $s.Pid, $s.AppState)
        Set-ItemProperty -Path $s.Key -Name AppState -Value 1001 -Type DWord
        $n++
    }
    return $n
}
