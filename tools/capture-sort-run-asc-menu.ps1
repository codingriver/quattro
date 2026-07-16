param(
    [string]$Repo = 'E:\work\quattro',
    [string]$AppDir = "$env:TEMP\quattro-sort-shot-run-asc",
    [string]$OutDir = 'E:\work\quattro\screenshots\sort-acceptance',
    [switch]$InteractiveVisual
)
$ErrorActionPreference = 'Stop'
if (!$InteractiveVisual) {
  throw 'This legacy cursor-driven capture must run in an isolated interactive Windows session. Use run-menu-visual-tests.ps1 for background-safe menu screenshots.'
}
. (Join-Path $Repo 'tools\QuattroTestHarness.ps1')
New-Item -ItemType Directory -Force $AppDir | Out-Null
New-Item -ItemType Directory -Force $OutDir | Out-Null
Copy-Item "$Repo\build-vcpkg-preset\Release\Quattro.exe" "$AppDir\Quattro.exe" -Force
Copy-Item "$Repo\icons" "$AppDir\icons" -Recurse -Force
Copy-Item "$Repo\theme" "$AppDir\theme" -Recurse -Force

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Win32ShotRunAsc {
  public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr hWnd, StringBuilder name, int count);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
}
"@
function Find-MainWindow([int]$ProcessId) {
  $script:foundHwnd = [IntPtr]::Zero
  $cb = [Win32ShotRunAsc+EnumWindowsProc]{ param($hwnd, $lp)
    if (-not [Win32ShotRunAsc]::IsWindowVisible($hwnd)) { return $true }
    $sb = New-Object System.Text.StringBuilder 128
    [void][Win32ShotRunAsc]::GetClassName($hwnd, $sb, 128)
    if ($sb.ToString() -ne 'QuattroMainWindow') { return $true }
    [uint32]$windowPid = 0
    [void][Win32ShotRunAsc]::GetWindowThreadProcessId($hwnd, [ref]$windowPid)
    if ($windowPid -eq [uint32]$ProcessId) { $script:foundHwnd = $hwnd; return $false }
    return $true
  }
  [void][Win32ShotRunAsc]::EnumWindows($cb, [IntPtr]::Zero)
  return $script:foundHwnd
}
function Capture-Region($Name, $Rect) {
  $path = Join-Path $OutDir $Name
  $region = "$($Rect.Left),$($Rect.Top),$($Rect.Right - $Rect.Left),$($Rect.Bottom - $Rect.Top)"
  powershell -ExecutionPolicy Bypass -File "C:\Users\coding\.codex\skills\screenshot\scripts\take_screenshot.ps1" -Path $path -Region $region | Out-Null
  return $path
}
if (Test-Path "$AppDir\db") { Remove-Item "$AppDir\db" -Recurse -Force }
& "$Repo\build-vcpkg-preset\Release\QuattroSortScreenshotSeed.exe" $AppDir run-asc | Out-Null
$proc = Start-QuattroTestProcess -FilePath "$AppDir\Quattro.exe" -WorkingDirectory $AppDir
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 80; $i++) { Start-Sleep -Milliseconds 150; $hwnd = Find-MainWindow $proc.Id; if ($hwnd -ne [IntPtr]::Zero) { break } }
if ($hwnd -eq [IntPtr]::Zero) { throw 'Quattro main window not found' }
[void][Win32ShotRunAsc]::ShowWindow($hwnd, 4)
[void][Win32ShotRunAsc]::SetWindowPos($hwnd, [IntPtr]::new(1), 80, 80, 420, 520, 0x0011)
Set-QuattroTestWindowBackground -Process $proc -Window $hwnd -Context 'sort menu capture'
Start-Sleep -Milliseconds 800
$rect = New-Object Win32ShotRunAsc+RECT
[void][Win32ShotRunAsc]::GetWindowRect($hwnd, [ref]$rect)
$x = $rect.Left + 350
$y = $rect.Top + 285
[void][Win32ShotRunAsc]::SetCursorPos($x, $y)
[Win32ShotRunAsc]::mouse_event(0x0008, 0, 0, 0, [UIntPtr]::Zero)
[Win32ShotRunAsc]::mouse_event(0x0010, 0, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 500
[void][Win32ShotRunAsc]::SetCursorPos($x + 70, $y + 220)
Start-Sleep -Milliseconds 700
$menuRect = New-Object Win32ShotRunAsc+RECT
$menuRect.Left = $rect.Left + 320
$menuRect.Top = $rect.Top + 260
$menuRect.Right = $rect.Left + 760
$menuRect.Bottom = $rect.Top + 610
$out = Capture-Region '06-menu-sort-icons-run-count-asc.png' $menuRect
[void][Win32ShotRunAsc]::PostMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
Start-Sleep -Milliseconds 300
Stop-QuattroTestProcess -Process $proc -MainWindow $hwnd -TimeoutMs 2000
Write-Output $out
