param(
    [string]$Repo = 'E:\work\quattro',
    [string]$OutPath = 'E:\work\quattro\screenshots\adblock-radio-fix.png'
)
$ErrorActionPreference = 'Stop'
$appDir = "$Repo\build-vcpkg-x64-tests-complete\Release"
Copy-Item "$Repo\theme" "$appDir\theme" -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item "$Repo\icons" "$appDir\icons" -Recurse -Force -ErrorAction SilentlyContinue

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Win32AdBlockShot {
  public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr hWnd, StringBuilder name, int count);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdcBlt, uint nFlags);
  [DllImport("user32.dll")] public static extern IntPtr GetWindowDC(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr hwnd, IntPtr hdc);
}
"@

function Capture-Window($hWnd, $Path) {
    $rect = New-Object Win32AdBlockShot+RECT
    [void][Win32AdBlockShot]::GetWindowRect($hWnd, [ref]$rect)
    $w = $rect.Right - $rect.Left
    $h = $rect.Bottom - $rect.Top
    if ($w -le 0 -or $h -le 0) { throw "Invalid window size" }
    Add-Type -AssemblyName System.Drawing
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $gfx.GetHdc()
    [void][Win32AdBlockShot]::PrintWindow($hWnd, $hdc, 2)
    $gfx.ReleaseHdc($hdc)
    $gfx.Dispose()
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

$exe = "$appDir\AppLaunchLocker.exe"
$proc = Start-Process -FilePath $exe -WorkingDirectory $appDir -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 80; $i++) {
    Start-Sleep -Milliseconds 150
    $script:foundHwnd = [IntPtr]::Zero
    $cb = [Win32AdBlockShot+EnumWindowsProc]{ param($h, $lp)
        if (-not [Win32AdBlockShot]::IsWindowVisible($h)) { return $true }
        $sb = New-Object System.Text.StringBuilder 128
        [void][Win32AdBlockShot]::GetClassName($h, $sb, 128)
        if ($sb.ToString() -ne 'AdBlockMainWindow') { return $true }
        [uint32]$wpid = 0
        [void][Win32AdBlockShot]::GetWindowThreadProcessId($h, [ref]$wpid)
        if ($wpid -eq [uint32]$proc.Id) { $script:foundHwnd = $h; return $false }
        return $true
    }
    [void][Win32AdBlockShot]::EnumWindows($cb, [IntPtr]::Zero)
    if ($script:foundHwnd -ne [IntPtr]::Zero) { $hwnd = $script:foundHwnd; break }
}
if ($hwnd -eq [IntPtr]::Zero) {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    throw "AdBlockMainWindow not found"
}
[void][Win32AdBlockShot]::ShowWindow($hwnd, 4)
[void][Win32AdBlockShot]::SetWindowPos($hwnd, [IntPtr]::new(1), 80, 80, 0, 0, 0x0011)
[void][Win32AdBlockShot]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 800
Capture-Window $hwnd $OutPath
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Write-Host "Saved $OutPath"
