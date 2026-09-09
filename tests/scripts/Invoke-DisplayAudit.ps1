param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$RunRoot,
    [string]$Scenario = "",
    [string]$Arguments = "",
    [int]$TimeoutSeconds = 180
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$RunRoot = [IO.Path]::GetFullPath($RunRoot)
New-Item -ItemType Directory -Path $RunRoot -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $RunRoot "temp") -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repo "theme") -Destination $RunRoot -Recurse -Force
New-Item -ItemType Directory -Path (Join-Path $RunRoot "icons/menu") -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repo "icons/menu/tabler") -Destination (Join-Path $RunRoot "icons/menu") -Recurse -Force

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class DisplayAuditNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int left, top, right, bottom; }
    [StructLayout(LayoutKind.Sequential)]
    public struct GuiInfo {
        public int size, flags;
        public IntPtr active, focus, capture, menuOwner, moveSize, caret;
        public Rect caretRect;
    }
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint thread, ref GuiInfo info);
    public static string Snapshot() {
        var hwnd = GetForegroundWindow();
        uint pid;
        var thread = GetWindowThreadProcessId(hwnd, out pid);
        var info = new GuiInfo();
        info.size = Marshal.SizeOf(typeof(GuiInfo));
        GetGUIThreadInfo(thread, ref info);
        return hwnd.ToInt64() + ":" + pid + ":" + info.focus.ToInt64();
    }
}
'@

$id = [Guid]::NewGuid().ToString("N")
$before = [DisplayAuditNative]::Snapshot()
$start = New-Object Diagnostics.ProcessStartInfo
$start.FileName = [IO.Path]::GetFullPath($Executable)
$start.WorkingDirectory = $RunRoot
$start.Arguments = $Arguments
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$environment = @{
    QUATTRO_TEST_MODE = "1"
    QUATTRO_TEST_NO_FOCUS = "1"
    QUATTRO_ACCEPTANCE_MODE = "background"
    QUATTRO_TEST_RUN_ID = $id
    QUATTRO_TEST_SUPPRESS_TRAY = "1"
    QUATTRO_USER_CONFIG_DIR = $RunRoot
    QUATTRO_UI_ACCEPTANCE_OUTPUT_DIR = $RunRoot
    TEMP = (Join-Path $RunRoot "temp")
    TMP = (Join-Path $RunRoot "temp")
}
if ($Scenario) { $environment["QUATTRO_UI_ACCEPTANCE_${Scenario}_ONLY"] = "1" }
foreach ($key in $environment.Keys) { $start.EnvironmentVariables[$key] = $environment[$key] }
"run_id=$id" | Set-Content -LiteralPath (Join-Path $RunRoot ".quattro-test-root")
$process = New-Object Diagnostics.Process
$process.StartInfo = $start
try {
    if (!$process.Start()) { throw "Unable to start audit process" }
    $identity = @{
        pid = $process.Id
        executable = $start.FileName
        arguments = $Arguments
        runId = $id
        started = [DateTime]::UtcNow.ToString("o")
        foregroundBefore = $before
        scenario = $Scenario
    }
    $identity | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $RunRoot "process.json")
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while (!$process.WaitForExit(250)) {
        $fg = [DisplayAuditNative]::GetForegroundWindow()
        $fgPid = [uint32]0
        [DisplayAuditNative]::GetWindowThreadProcessId($fg, [ref]$fgPid) | Out-Null
        if ($fgPid -eq $process.Id) { throw "Audit process acquired foreground" }
        if ($timer.Elapsed.TotalSeconds -gt $TimeoutSeconds) { throw "Audit timeout after $TimeoutSeconds seconds" }
    }
    $stdout.Result | Set-Content -LiteralPath (Join-Path $RunRoot "stdout.txt")
    $stderr.Result | Set-Content -LiteralPath (Join-Path $RunRoot "stderr.txt")
    $after = [DisplayAuditNative]::Snapshot()
    @{
        exitCode = $process.ExitCode
        elapsedSeconds = $timer.Elapsed.TotalSeconds
        foregroundBefore = $before
        foregroundAfter = $after
        foregroundUnchanged = ($before -eq $after)
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $RunRoot "result.json")
    "exit_code=$($process.ExitCode) foreground_unchanged=$($before -eq $after) output=$RunRoot"
    $stdout.Result
    $stderr.Result
} finally {
    if ($process.Id -and !$process.HasExited) {
        $process.Kill()
        $process.WaitForExit(5000) | Out-Null
    }
    $process.Dispose()
}
