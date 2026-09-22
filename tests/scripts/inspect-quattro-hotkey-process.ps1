param(
    [Parameter(Mandatory = $true)][int]$TargetProcessId,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = "Stop"
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null

# Read-only inspection: no messages, input, activation, suspension or config access.
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class QuattroHotkeyInspection {
    public delegate bool EnumProc(IntPtr window, IntPtr parameter);
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)]
    public struct Gui {
        public uint Size, Flags;
        public IntPtr Active, Focus, Capture, MenuOwner, MoveSize, Caret;
        public Rect CaretRect;
    }
    public class Window {
        public long Handle, Owner;
        public uint Thread;
        public string ClassName, Title;
        public bool Visible, Enabled, Minimized, Hung;
        public Rect Bounds;
    }
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback, IntPtr parameter);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowTextW(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsHungAppWindow(IntPtr window);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr window, uint command);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool GetGUIThreadInfo(uint thread, ref Gui gui);
    public static string Desktop() {
        var gui = new Gui { Size = (uint)Marshal.SizeOf(typeof(Gui)) };
        var foreground = GetForegroundWindow();
        var known = GetGUIThreadInfo(0, ref gui);
        return foreground.ToInt64() + ":" + gui.Focus.ToInt64() + ":" + known;
    }
    public static Window[] Windows(uint target) {
        var windows = new List<Window>();
        EnumWindows((window, unused) => {
            uint process;
            uint thread = GetWindowThreadProcessId(window, out process);
            if (process != target) return true;
            var name = new StringBuilder(256);
            var title = new StringBuilder(256);
            GetClassNameW(window, name, name.Capacity);
            GetWindowTextW(window, title, title.Capacity);
            Rect bounds;
            GetWindowRect(window, out bounds);
            windows.Add(new Window {
                Handle = window.ToInt64(), Thread = thread,
                ClassName = name.ToString(), Title = title.ToString(),
                Visible = IsWindowVisible(window), Enabled = IsWindowEnabled(window),
                Minimized = IsIconic(window), Hung = IsHungAppWindow(window),
                Owner = GetWindow(window, 4).ToInt64(), Bounds = bounds
            });
            return true;
        }, IntPtr.Zero);
        return windows.ToArray();
    }
}
'@

$before = [QuattroHotkeyInspection]::Desktop()
$process = Get-Process -Id $TargetProcessId
$identity = Get-CimInstance Win32_Process -Filter "ProcessId = $TargetProcessId"
$report = [ordered]@{
    capturedAt = (Get-Date).ToString("o")
    processId = $process.Id
    executable = $identity.ExecutablePath
    commandLine = $identity.CommandLine
    startTime = $process.StartTime.ToString("o")
    cpuSeconds = $process.CPU
    threadCount = $process.Threads.Count
    windows = @([QuattroHotkeyInspection]::Windows($TargetProcessId))
    threads = @($process.Threads | ForEach-Object {
        [ordered]@{
            id = $_.Id
            state = $_.ThreadState.ToString()
            waitReason = $(if ($_.ThreadState -eq "Wait") { $_.WaitReason.ToString() } else { $null })
            cpuMilliseconds = $_.TotalProcessorTime.TotalMilliseconds
        }
    })
    foregroundBefore = $before
    foregroundAfter = [QuattroHotkeyInspection]::Desktop()
}
$report | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath (Join-Path $outputRoot "process-inspection.json") -Encoding utf8
$report.windows | Format-Table Handle, Thread, ClassName, Visible, Enabled, Minimized, Hung, Title -AutoSize
Write-Output "foreground_unchanged=$($report.foregroundBefore -eq $report.foregroundAfter)"
Write-Output "evidence=$outputRoot"
