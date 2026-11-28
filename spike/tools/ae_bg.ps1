# THROWAWAY SPIKE: run AfterFX on a hidden Win32 desktop so it never appears on (or steals
# focus from) the user's screen. Dot-source this file.
#   Start-AeHidden -Exe <path> -Arguments <string>  -> System.Diagnostics.Process
#   Get-AeHiddenWindows -ProcessId <pid>             -> windows of that pid on the hidden desktop
#   Save-AeWindowImage -Hwnd <h> -Path <png>         -> PrintWindow capture (only this window's
#                                                       pixels; never the user's screen)

if (-not ("AeBg" -as [type])) {
Add-Type @"
using System; using System.Text; using System.Collections.Generic; using System.Runtime.InteropServices;
public class AeBg {
  public const string DesktopName = "duplix_bg";
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  struct STARTUPINFO { public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
    public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError; }
  [StructLayout(LayoutKind.Sequential)]
  struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
  [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern IntPtr CreateDesktop(string name, IntPtr dev, IntPtr mode, uint flags, uint access, IntPtr sa);
  [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern IntPtr OpenDesktop(string name, uint flags, bool inherit, uint access);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern bool CreateProcess(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags,
    IntPtr env, string dir, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumDesktopWindows(IntPtr desk, EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public struct RECT { public int L, T, R, B; }
  const uint GENERIC_ALL = 0x10000000;
  static IntPtr desk = IntPtr.Zero;

  public static IntPtr Desktop() {
    if (desk != IntPtr.Zero) return desk;
    desk = OpenDesktop(DesktopName, 0, false, GENERIC_ALL);
    if (desk == IntPtr.Zero) desk = CreateDesktop(DesktopName, IntPtr.Zero, IntPtr.Zero, 0, GENERIC_ALL, IntPtr.Zero);
    if (desk == IntPtr.Zero) throw new Exception("CreateDesktop failed: " + Marshal.GetLastWin32Error());
    return desk;   // keep the handle open for the life of this process so the desktop survives
  }
  public static int Start(string exe, string args, string dir) {
    Desktop();
    var si = new STARTUPINFO(); si.cb = Marshal.SizeOf(si); si.lpDesktop = "WinSta0\\" + DesktopName;
    PROCESS_INFORMATION pi;
    var cmd = new StringBuilder("\"" + exe + "\" " + args);
    if (!CreateProcess(exe, cmd, IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, dir, ref si, out pi))
      throw new Exception("CreateProcess failed: " + Marshal.GetLastWin32Error());
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return pi.dwProcessId;
  }
  public static string ClassOf(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
  public static string TitleOf(IntPtr h) { var s = new StringBuilder(512); GetWindowText(h, s, 512); return s.ToString(); }
  public static List<IntPtr> Windows(uint pid) {
    var list = new List<IntPtr>();
    EnumDesktopWindows(Desktop(), (h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid) list.Add(h); return true; }, IntPtr.Zero);
    return list;
  }
  // every visible titled #32770 window on the hidden desktop, of any process: "pid|title|blocking|text of its
  // controls". blocking = it has a standard Win32 button (a message box: a C runtime assertion, Windows Error
  // Reporting, a MessageBox), which waits for a click nobody sees. AE's own panels and progress windows are
  // #32770 windows drawn by AE (OS_ViewContainer, no Win32 buttons): not blocking.
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr l);
  public static List<string> Dialogs() {
    var list = new List<string>();
    EnumDesktopWindows(Desktop(), (h, l) => {
      if (IsWindowVisible(h) && ClassOf(h) == "#32770" && TitleOf(h).Length > 0) {
        uint p; GetWindowThreadProcessId(h, out p);
        var text = new StringBuilder();
        bool button = false;
        EnumChildWindows(h, (c, l2) => {
          if (ClassOf(c) == "Button") button = true;
          var t = TitleOf(c); if (t.Length > 0) text.Append(t.Replace("\r", " ").Replace("\n", " ")).Append(" / ");
          return true;
        }, IntPtr.Zero);
        list.Add(p + "|" + TitleOf(h) + "|" + (button ? "1" : "0") + "|" + text.ToString());
      }
      return true;
    }, IntPtr.Zero);
    return list;
  }
}
"@
}

# The text of a window's children through WM_GETTEXT (AE's own untitled #32770 warnings are drawn by AE but keep
# their text there: 11/26 "a second script while one is running was not run").
if (-not ("AeText" -as [type])) {
Add-Type @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
public static class AeText {
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w,
        StringBuilder l, uint flags, uint ms, out IntPtr r);
    // the first child window of a class (0 if none)
    public static IntPtr FindChild(IntPtr p, string cls) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(p, (h, l) => { var c = new StringBuilder(256); GetClassName(h, c, 256);
            if (c.ToString() == cls) { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }
    [DllImport("user32.dll")] static extern int GetWindowLong(IntPtr h, int i);
    // the first child of a class whose style has these bits (DupliX 2's Code view: the multi-line EDIT)
    public static IntPtr FindChildByStyle(IntPtr p, string cls, int bits) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(p, (h, l) => { var c = new StringBuilder(256); GetClassName(h, c, 256);
            if (c.ToString() == cls && (GetWindowLong(h, -16 /* GWL_STYLE */) & bits) == bits) { found = h; return false; }
            return true; }, IntPtr.Zero);
        return found;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, string l);
    public static void SetText(IntPtr h, string s) { SendMessage(h, 0x000C /* WM_SETTEXT */, IntPtr.Zero, s); }
    public static List<string> Texts(IntPtr p) {
        var list = new List<string>();
        EnumChildWindows(p, (h, l) => { var c = new StringBuilder(256); var t = new StringBuilder(8192); IntPtr r;
            GetClassName(h, c, 256);
            SendMessageTimeout(h, 0x000D /* WM_GETTEXT */, (IntPtr)8192, t, 2 /* SMTO_ABORTIFHUNG */, 1000, out r);
            if (t.Length > 0) list.Add(c + " [" + t.ToString().Replace("\r", " ").Replace("\n", " ") + "]");
            return true; }, IntPtr.Zero);
        return list;
    }
}
"@
}

# Every visible #32770 of a process, titled or not, with its children's text: what is blocking AE when a script it
# was sent does not run. Saves each as <Dir>\window_<n>.png when -Dir is given.
function Get-AeModalWindows([int]$ProcessId, [string]$Dir = "") {
    $k = 0
    foreach ($w in Get-AeHiddenWindows -ProcessId $ProcessId | Where-Object { $_.Visible -and $_.Class -eq "#32770" }) {
        $texts = [AeText]::Texts([IntPtr]$w.Hwnd)
        if ($Dir) { $null = Save-AeWindowImage -Hwnd $w.Hwnd -Path (Join-Path $Dir "window_$k.png"); $k++ }
        [pscustomobject]@{ Hwnd = $w.Hwnd; Title = $w.Title; W = $w.W; H = $w.H; Text = ($texts -join " / ") }
    }
}

# Sends a script to an AE that is already running (AfterFX.exe -r) and waits for the file the script writes last.
# AE refuses a script while it still counts the previous one as running, for a few seconds after that script's last
# line (more when the GPU is busy), and says so in an untitled #32770 that blocks AE: "1 つのスクリプトを実行中のときに
# 2 つめのスクリプトを実行しようとしたため、2 つめのスクリプトは実行されませんでした" (11/26, 11/28). That warning is closed and
# the script sent again; any other modal window is reported at once (it would block every later script too).
function Send-AeScript([System.Diagnostics.Process]$Ae, [string]$Exe, [string]$Jsx, [string]$Done, [int]$TimeoutSec = 120,
                       [int]$Tries = 5) {
    for ($k = 1; $k -le $Tries; $k++) {
        $s = Start-AeHidden -Exe $Exe -Arguments "-r $Jsx"
        $null = $s.WaitForExit(30000)
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        $refused = $false
        while (-not (Test-Path $Done) -and (Get-Date) -lt $deadline -and -not $Ae.HasExited) {
            Start-Sleep -Milliseconds 500
            # AE's titled #32770 windows are its progress and panels ("スクリプト ... を実行中..." while a script runs):
            # not blocking. Its untitled ones are its alerts; a Win32 message box (Get-AeHiddenDialogs) also blocks.
            foreach ($m in Get-AeModalWindows -ProcessId $Ae.Id | Where-Object { -not $_.Title }) {
                if ($m.Text -match "2 つめのスクリプト|second script") {
                    $null = [AeBg]::PostMessage([IntPtr]$m.Hwnd, 0x0010, [IntPtr]0, [IntPtr]0)  # WM_CLOSE
                    $refused = $true
                } else {
                    throw ("an AE alert blocks AE while sending {0}: {1}" -f (Split-Path $Jsx -Leaf), $m.Text)
                }
            }
            foreach ($d in Get-AeHiddenDialogs | Where-Object Blocking) {
                throw ("a message box blocks AE while sending {0}: {1} [{2}] {3}" -f (Split-Path $Jsx -Leaf), $d.Process, $d.Title, $d.Text)
            }
            if ($refused) { break }
        }
        if (Test-Path $Done) { return $k }
        if ($Ae.HasExited) { throw "AE exited while sending $(Split-Path $Jsx -Leaf)" }
        if (-not $refused) { throw "$(Split-Path $Jsx -Leaf) did not finish in $TimeoutSec s (no window blocks AE)" }
        Write-Host ("  {0}: AE was still running the previous script (try {1}): sent again" -f (Split-Path $Jsx -Leaf), $k)
        Start-Sleep -Seconds 2
    }
    throw "$(Split-Path $Jsx -Leaf): refused $Tries times (AE kept counting the previous script as running)"
}

function Start-AeHidden([string]$Exe, [string]$Arguments) {
    $id = [AeBg]::Start($Exe, $Arguments, (Split-Path $Exe))
    Get-Process -Id $id
}

# Visible dialogs on the hidden desktop (any process): objects with Pid, Process, Title, Text. A test that waits on
# AE checks this regularly: a dialog blocks AE forever, and nobody sees it on the hidden desktop.
function Get-AeHiddenDialogs {
    foreach ($d in [AeBg]::Dialogs()) {
        $f = $d -split "\|", 4
        $name = (Get-Process -Id ([int]$f[0]) -ErrorAction SilentlyContinue).ProcessName
        [pscustomobject]@{ Pid = [int]$f[0]; Process = $name; Title = $f[1]; Blocking = $f[2] -eq "1"; Text = $f[3] }
    }
}

function Get-AeHiddenWindows([int]$ProcessId) {
    foreach ($h in [AeBg]::Windows([uint32]$ProcessId)) {
        $r = New-Object AeBg+RECT; $null = [AeBg]::GetWindowRect($h, [ref]$r)
        [pscustomobject]@{ Hwnd = [long]$h; Class = [AeBg]::ClassOf($h); Title = [AeBg]::TitleOf($h)
                           Visible = [AeBg]::IsWindowVisible($h); W = $r.R - $r.L; H = $r.B - $r.T }
    }
}

# Close AE without killing it, so it records a clean exit. The Home screen (a large visible
# DroverLord window) swallows WM_CLOSE, so send Escape to it first. Retries because AE
# ignores close requests while it is still loading. Kills only as a last resort.
function Stop-AeHidden([System.Diagnostics.Process]$Process, [int]$TimeoutSec = 90) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while (-not $Process.HasExited -and (Get-Date) -lt $deadline) {
        foreach ($w in Get-AeHiddenWindows -ProcessId $Process.Id | Where-Object Visible) {
            if ($w.Class -eq "DroverLord - Window Class") {
                $null = [AeBg]::PostMessage([IntPtr]$w.Hwnd, 0x100, [IntPtr]0x1B, [IntPtr]0)   # Escape
                $null = [AeBg]::PostMessage([IntPtr]$w.Hwnd, 0x101, [IntPtr]0x1B, [IntPtr]0)
            }
        }
        Start-Sleep -Milliseconds 500
        foreach ($w in Get-AeHiddenWindows -ProcessId $Process.Id | Where-Object { $_.Class -like "AE_CApplication*" }) {
            $null = [AeBg]::PostMessage([IntPtr]$w.Hwnd, 0x10, [IntPtr]0, [IntPtr]0)          # WM_CLOSE
        }
        if ($Process.WaitForExit(10000)) { break }
    }
    if (-not $Process.HasExited) {
        Stop-Process -Id $Process.Id -Force -Confirm:$false
        $null = $Process.WaitForExit(10000)
        Write-Host "[bg] AfterFX did not close in ${TimeoutSec}s: killed"
        return $false
    }
    return $true
}

function Save-AeWindowImage([long]$Hwnd, [string]$Path) {
    Add-Type -AssemblyName System.Drawing
    $h = [IntPtr]$Hwnd; $r = New-Object AeBg+RECT
    if (-not [AeBg]::GetWindowRect($h, [ref]$r)) { return $false }
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
    $ok = [AeBg]::PrintWindow($h, $hdc, 2)   # PW_RENDERFULLCONTENT
    $g.ReleaseHdc($hdc); $g.Dispose(); $bmp.Save($Path); $bmp.Dispose()
    $ok
}
