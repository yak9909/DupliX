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
  public const string DesktopName = "randamu_bg";
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
}
"@
}

function Start-AeHidden([string]$Exe, [string]$Arguments) {
    $id = [AeBg]::Start($Exe, $Arguments, (Split-Path $Exe))
    Get-Process -Id $id
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
