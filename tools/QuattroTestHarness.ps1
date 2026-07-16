$ErrorActionPreference = "Stop"

if (-not ("QuattroTestHarnessNative" -as [type])) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class QuattroTestHarnessNative {
    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

    [DllImport("user32.dll")]
    public static extern IntPtr GetAncestor(IntPtr hWnd, uint flags);

    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(
        IntPtr hWnd,
        IntPtr hWndInsertAfter,
        int x,
        int y,
        int width,
        int height,
        uint flags);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam);
}
"@
}

if ($null -eq $script:QuattroTestProcessRoots) {
    $script:QuattroTestProcessRoots = @{}
}

function Start-QuattroTestProcess {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(Mandatory = $true)]
        [string]$WorkingDirectory,

        [string]$Arguments = "",

        [string]$UserConfigDirectory = "",

        [hashtable]$Environment = @{}
    )

    if (!(Test-Path -LiteralPath $FilePath -PathType Leaf)) {
        throw "Quattro test executable not found: $FilePath"
    }

    New-Item -ItemType Directory -Force -Path $WorkingDirectory | Out-Null
    if ([string]::IsNullOrWhiteSpace($UserConfigDirectory)) {
        $UserConfigDirectory = $WorkingDirectory
    }
    New-Item -ItemType Directory -Force -Path $UserConfigDirectory | Out-Null

    $runId = [Guid]::NewGuid().ToString("N")
    $markerPath = Join-Path $UserConfigDirectory ".quattro-test-root"
    Remove-Item -LiteralPath (Join-Path $UserConfigDirectory "logs\test-ready.txt") -Force -ErrorAction SilentlyContinue
    @(
        "run_id=$runId"
        "created=$([DateTime]::UtcNow.ToString('o'))"
        "executable=$([System.IO.Path]::GetFullPath($FilePath))"
    ) | Set-Content -LiteralPath $markerPath -Encoding UTF8

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = [System.IO.Path]::GetFullPath($FilePath)
    $startInfo.WorkingDirectory = [System.IO.Path]::GetFullPath($WorkingDirectory)
    $startInfo.Arguments = $Arguments
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $false
    $startInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Normal
    $startInfo.EnvironmentVariables["QUATTRO_TEST_MODE"] = "1"
    $startInfo.EnvironmentVariables["QUATTRO_TEST_NO_FOCUS"] = "1"
    $startInfo.EnvironmentVariables["QUATTRO_ACCEPTANCE_MODE"] = "background"
    $startInfo.EnvironmentVariables["QUATTRO_TEST_RUN_ID"] = $runId
    $startInfo.EnvironmentVariables["QUATTRO_USER_CONFIG_DIR"] = [System.IO.Path]::GetFullPath($UserConfigDirectory)
    foreach ($key in $Environment.Keys) {
        $value = $Environment[$key]
        if ($null -eq $value) {
            $startInfo.EnvironmentVariables.Remove([string]$key)
        } else {
            $startInfo.EnvironmentVariables[[string]$key] = [string]$value
        }
    }

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    if (!$process.Start()) {
        $process.Dispose()
        throw "Unable to start isolated Quattro test process: $FilePath"
    }
    $script:QuattroTestProcessRoots[$process.Id] = [System.IO.Path]::GetFullPath($UserConfigDirectory)
    return $process
}

function Wait-QuattroTestProcessReady {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process,

        [int]$TimeoutMs = 60000
    )

    if (!$script:QuattroTestProcessRoots.ContainsKey($Process.Id)) {
        throw "No isolated test root is registered for pid $($Process.Id)."
    }
    $readyPath = Join-Path $script:QuattroTestProcessRoots[$Process.Id] "logs\test-ready.txt"
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($Process.HasExited) {
            throw "Quattro test process exited before it became ready. pid=$($Process.Id)"
        }
        if (Test-Path -LiteralPath $readyPath) {
            $pidLine = Get-Content -LiteralPath $readyPath | Where-Object { $_ -eq "pid=$($Process.Id)" } | Select-Object -First 1
            if ($pidLine) {
                return
            }
        }
        Start-Sleep -Milliseconds 50
    }
    throw "Timed out waiting for Quattro test readiness. pid=$($Process.Id) marker=$readyPath"
}

function Assert-QuattroTestProcessNotForeground {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process,

        [string]$Context = "background acceptance"
    )

    if ($Process.HasExited) {
        return
    }

    $foreground = [QuattroTestHarnessNative]::GetForegroundWindow()
    if ($foreground -eq [IntPtr]::Zero) {
        return
    }
    $foregroundProcessId = [uint32]0
    [QuattroTestHarnessNative]::GetWindowThreadProcessId($foreground, [ref]$foregroundProcessId) | Out-Null
    if ($foregroundProcessId -eq [uint32]$Process.Id) {
        throw "Quattro test process stole foreground focus during ${Context}. pid=$($Process.Id) hwnd=$foreground"
    }
}

function Set-QuattroTestWindowBackground {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process,

        [Parameter(Mandatory = $true)]
        [IntPtr]$Window,

        [string]$Context = "window show"
    )

    Wait-QuattroTestProcessReady -Process $Process
    if ($Window -eq [IntPtr]::Zero -or ![QuattroTestHarnessNative]::IsWindow($Window)) {
        throw "Unable to apply background policy to an invalid window during ${Context}."
    }

    $GA_ROOTOWNER = 3
    $HWND_BOTTOM = [IntPtr]::new(1)
    $SWP_NOSIZE = 0x0001
    $SWP_NOMOVE = 0x0002
    $SWP_NOACTIVATE = 0x0010
    $SWP_NOOWNERZORDER = 0x0200
    $target = [QuattroTestHarnessNative]::GetAncestor($Window, $GA_ROOTOWNER)
    if ($target -eq [IntPtr]::Zero) {
        $target = $Window
    }
    [QuattroTestHarnessNative]::SetWindowPos(
        $target,
        $HWND_BOTTOM,
        0,
        0,
        0,
        0,
        $SWP_NOSIZE -bor $SWP_NOMOVE -bor $SWP_NOACTIVATE -bor $SWP_NOOWNERZORDER) | Out-Null
    Assert-QuattroTestProcessNotForeground -Process $Process -Context $Context
}

function Stop-QuattroTestProcess {
    [CmdletBinding()]
    param(
        [System.Diagnostics.Process]$Process,
        [IntPtr]$MainWindow = [IntPtr]::Zero,
        [int]$ExitCommand = 40005,
        [int]$TimeoutMs = 3000
    )

    if (!$Process -or $Process.HasExited) {
        return
    }

    try {
        if ($MainWindow -ne [IntPtr]::Zero -and [QuattroTestHarnessNative]::IsWindow($MainWindow)) {
            [QuattroTestHarnessNative]::PostMessage(
                $MainWindow,
                0x0111,
                [IntPtr]$ExitCommand,
                [IntPtr]::Zero) | Out-Null
            if ($Process.WaitForExit($TimeoutMs)) {
                $script:QuattroTestProcessRoots.Remove($Process.Id)
                return
            }
        }
    } catch {
        # The process may still be inside startup or may have destroyed its window.
    }

    try {
        if (!$Process.HasExited) {
            Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        }
    } catch {
        # Cleanup is best effort, but never attempt to terminate by process name.
    }
    $script:QuattroTestProcessRoots.Remove($Process.Id)
}
