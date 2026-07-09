param(
    [string]$Repo = 'E:\work\quattro',
    [string]$AppDir = "$env:TEMP\quattro-theme-menu-shot",
    [string]$OutDir = 'E:\work\quattro\screenshots\sort-acceptance'
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $AppDir | Out-Null
New-Item -ItemType Directory -Force $OutDir | Out-Null
$env:QUATTRO_USER_CONFIG_DIR = $AppDir
Get-Process Quattro -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Copy-Item "$Repo\build-vcpkg-preset\Release\Quattro.exe" "$AppDir\Quattro.exe" -Force
Copy-Item "$Repo\icons" "$AppDir\icons" -Recurse -Force
Copy-Item "$Repo\theme" "$AppDir\theme" -Recurse -Force

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Win32ThemeMenuShot {
  public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr hWnd, StringBuilder name, int count);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
}
"@

function Find-MainWindow([int]$ProcessId) {
  $script:foundHwnd = [IntPtr]::Zero
  $cb = [Win32ThemeMenuShot+EnumWindowsProc]{ param($hwnd, $lp)
    if (-not [Win32ThemeMenuShot]::IsWindowVisible($hwnd)) { return $true }
    $sb = New-Object System.Text.StringBuilder 128
    [void][Win32ThemeMenuShot]::GetClassName($hwnd, $sb, 128)
    if ($sb.ToString() -ne 'QuattroMainWindow') { return $true }
    [uint32]$windowPid = 0
    [void][Win32ThemeMenuShot]::GetWindowThreadProcessId($hwnd, [ref]$windowPid)
    if ($windowPid -eq [uint32]$ProcessId) { $script:foundHwnd = $hwnd; return $false }
    return $true
  }
  [void][Win32ThemeMenuShot]::EnumWindows($cb, [IntPtr]::Zero)
  return $script:foundHwnd
}

function Capture-Region($Name, $Rect) {
  $path = Join-Path $OutDir $Name
  $region = "$($Rect.Left),$($Rect.Top),$($Rect.Right - $Rect.Left),$($Rect.Bottom - $Rect.Top)"
  powershell -ExecutionPolicy Bypass -File "C:\Users\coding\.codex\skills\screenshot\scripts\take_screenshot.ps1" -Path $path -Region $region | Out-Null
  return $path
}

if (Test-Path "$AppDir\db") { Remove-Item "$AppDir\db" -Recurse -Force }
& "$Repo\build-vcpkg-preset\Release\QuattroSortScreenshotSeed.exe" $AppDir manual | Out-Null
Set-Content -Path "$AppDir\conf.ini" -Encoding Unicode -Value @"
[main]
bShowBtnSkin=1
Theme=default
"@
$proc = Start-Process -FilePath "$AppDir\Quattro.exe" -WorkingDirectory $AppDir -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 80; $i++) {
  Start-Sleep -Milliseconds 150
  $hwnd = Find-MainWindow $proc.Id
  if ($hwnd -ne [IntPtr]::Zero) { break }
}
if ($hwnd -eq [IntPtr]::Zero) { throw 'Quattro main window not found' }

[void][Win32ThemeMenuShot]::ShowWindow($hwnd, 9)
[void][Win32ThemeMenuShot]::SetWindowPos($hwnd, [IntPtr]::Zero, 80, 80, 420, 520, 0x0040)
[void][Win32ThemeMenuShot]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 800

$windowRect = New-Object Win32ThemeMenuShot+RECT
[void][Win32ThemeMenuShot]::GetWindowRect($hwnd, [ref]$windowRect)
$skinButtonX = $windowRect.Right - 101
$skinButtonY = $windowRect.Top + 17
[void][Win32ThemeMenuShot]::SetCursorPos($skinButtonX, $skinButtonY)
[Win32ThemeMenuShot]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
[Win32ThemeMenuShot]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 700

$menuRect = New-Object Win32ThemeMenuShot+RECT
$menuRect.Left = $skinButtonX - 24
$menuRect.Top = $skinButtonY - 6
$menuRect.Right = $skinButtonX + 260
$menuRect.Bottom = $skinButtonY + 180
$out = Capture-Region '08-menu-theme-icon-state.png' $menuRect

[void][Win32ThemeMenuShot]::PostMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
Start-Sleep -Milliseconds 300
if (-not $proc.HasExited) { $proc.Kill() }
Write-Output $out
