param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [ValidateSet("Release", "Debug")][string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$buildRoot = (Resolve-Path -LiteralPath $BuildDirectory).Path
$runId = "window-activation-" + [Guid]::NewGuid().ToString("N")
$runRoot = Join-Path $projectRoot "out/$runId"
New-Item -ItemType Directory -Path $runRoot | Out-Null
Set-Content -LiteralPath (Join-Path $runRoot ".quattro-test-root") -Value $runId

# Read-only desktop observations; never restore focus by activating a window.
if (-not ("WindowActivationReadOnlyDesktop" -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class WindowActivationReadOnlyDesktop {
    [StructLayout(LayoutKind.Sequential)]
    public struct Gui {
        public uint cbSize, flags;
        public IntPtr active, focus, capture, menuOwner, moveSize, caret;
        public int left, top, right, bottom;
    }
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint thread, ref Gui gui);
    public static string Snapshot() {
        var gui = new Gui { cbSize = (uint)Marshal.SizeOf(typeof(Gui)) };
        var foreground = GetForegroundWindow();
        bool known = GetGUIThreadInfo(0, ref gui);
        return foreground.ToInt64() + ":" + gui.focus.ToInt64() + ":" + known;
    }
}
'@
}

function Get-ExistingQuattroIdentity {
    @(Get-CimInstance Win32_Process -Filter "Name LIKE 'Quattro%'" |
        Sort-Object ProcessId | ForEach-Object {
            $process = Get-Process -Id $_.ProcessId -ErrorAction Stop
            "$($_.ProcessId)|$($_.ExecutablePath)|$($_.CreationDate.ToString('o'))|$($process.MainWindowHandle)"
        }) -join "`n"
}

function Invoke-IsolatedTest([string]$Name, [string]$Executable, [string]$Arguments, [bool]$HasChild) {
    if (!(Test-Path -LiteralPath $Executable -PathType Leaf)) {
        throw "Missing newly built test executable: $Executable"
    }
    $testRoot = Join-Path $runRoot $Name
    $tempRoot = Join-Path $testRoot "temp"
    New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Executable
    $info.Arguments = $Arguments
    $info.WorkingDirectory = $testRoot
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.EnvironmentVariables["QUATTRO_TEST_MODE"] = "1"
    $info.EnvironmentVariables["QUATTRO_TEST_NO_FOCUS"] = "1"
    $info.EnvironmentVariables["QUATTRO_ACCEPTANCE_MODE"] = "background"
    $info.EnvironmentVariables["QUATTRO_TEST_RUN_ID"] = "$runId-$Name"
    $info.EnvironmentVariables["QUATTRO_USER_CONFIG_DIR"] = (Join-Path $testRoot "config")
    $info.EnvironmentVariables["QUATTRO_TEST_SUPPRESS_TRAY"] = "1"
    $info.EnvironmentVariables["TEMP"] = $tempRoot
    $info.EnvironmentVariables["TMP"] = $tempRoot
    $before = [WindowActivationReadOnlyDesktop]::Snapshot()
    $process = [System.Diagnostics.Process]::Start($info)
    $output = $process.StandardOutput.ReadToEndAsync()
    $errors = $process.StandardError.ReadToEndAsync()
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $children = @{}
    $expectedChild = Join-Path $tempRoot "quattro-dock-activation-$($process.Id)/Quattro.exe"
    try {
        "$($process.Id)|$Executable|$Arguments|$runId-$Name" |
            Set-Content -LiteralPath (Join-Path $testRoot "processes.log")
        do {
            if ($HasChild) {
                foreach ($child in @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $($process.Id)")) {
                    if ($child.ExecutablePath -and
                        [IO.Path]::GetFullPath($child.ExecutablePath) -eq [IO.Path]::GetFullPath($expectedChild)) {
                        if (!$children.ContainsKey([int]$child.ProcessId)) {
                            $children[[int]$child.ProcessId] = $child.CreationDate
                            "$($child.ProcessId)|$($child.ExecutablePath)|$($child.CommandLine)|$runId-$Name" |
                                Add-Content -LiteralPath (Join-Path $testRoot "processes.log")
                        }
                    }
                }
            }
            if ($process.WaitForExit(100)) { break }
            if ($watch.ElapsedMilliseconds -gt 60000) { throw "$Name exceeded 60 seconds" }
        } while ($true)
        $process.WaitForExit()
        $output.GetAwaiter().GetResult() | Set-Content -LiteralPath (Join-Path $testRoot "stdout.log")
        $errors.GetAwaiter().GetResult() | Set-Content -LiteralPath (Join-Path $testRoot "stderr.log")
        $after = [WindowActivationReadOnlyDesktop]::Snapshot()
        "$Name exit=$($process.ExitCode) elapsed_ms=$($watch.ElapsedMilliseconds) foreground_before=$before foreground_after=$after" |
            Tee-Object -FilePath (Join-Path $runRoot "results.log") -Append
        if ($process.ExitCode -ne 0) { throw "$Name failed; see $testRoot" }
        if ($before -ne $after) { throw "Foreground/focus changed during $Name; no corrective activation attempted" }
    } finally {
        # Exact recorded PID + creation time + expected test path; never process-name cleanup.
        foreach ($childId in @($children.Keys)) {
            $child = Get-CimInstance Win32_Process -Filter "ProcessId = $childId"
            if ($child -and $child.CreationDate -eq $children[$childId] -and
                $child.ExecutablePath -and
                [IO.Path]::GetFullPath($child.ExecutablePath) -eq [IO.Path]::GetFullPath($expectedChild)) {
                Stop-Process -Id $childId -ErrorAction Stop
            }
        }
        if (!$process.HasExited) { $process.Kill() }
        $process.Dispose()
    }
}

$initialDesktop = [WindowActivationReadOnlyDesktop]::Snapshot()
$initialInstances = Get-ExistingQuattroIdentity
$initialInstances | Set-Content -LiteralPath (Join-Path $runRoot "existing-instances-before.log")
try {
    Invoke-IsolatedTest "unit" (Join-Path $buildRoot "$Configuration/QuattroTests.exe") "--window-activation-only" $false
    Invoke-IsolatedTest "dock" (Join-Path $buildRoot "$Configuration/QuattroDockActivationAcceptance.exe") "" $true
} finally {
    $finalInstances = Get-ExistingQuattroIdentity
    $finalDesktop = [WindowActivationReadOnlyDesktop]::Snapshot()
    $finalInstances | Set-Content -LiteralPath (Join-Path $runRoot "existing-instances-after.log")
    "overall_foreground_before=$initialDesktop overall_foreground_after=$finalDesktop existing_instances_unchanged=$($initialInstances -eq $finalInstances)" |
        Tee-Object -FilePath (Join-Path $runRoot "results.log") -Append
    Write-Output "evidence=$runRoot"
}
if ($initialInstances -ne $finalInstances -or $initialDesktop -ne $finalDesktop) {
    throw "Existing process/window or foreground/focus changed; inspect evidence"
}
Write-Output "window_activation_background_tests=passed interactive_foreground_validation=not_run"
