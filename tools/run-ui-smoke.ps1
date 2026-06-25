param(
    [string]$ExePath = "",
    [string]$Configuration = "Release",
    [string]$LogDir = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $ExePath = Join-Path $root "build\$Configuration\Quattro.exe"
}
if (!(Test-Path $ExePath)) {
    throw "Executable not found: $ExePath"
}

if ([string]::IsNullOrWhiteSpace($LogDir)) {
    $LogDir = Join-Path (Join-Path $root "build\$Configuration") "logs"
}
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public struct RECT {
    public int Left;
    public int Top;
    public int Right;
    public int Bottom;
}

public static class NativeUi {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr hWnd, StringBuilder lpClassName, int nMaxCount);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowEx(IntPtr hwndParent, IntPtr hwndChildAfter, string lpszClass, string lpszWindow);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool SetWindowText(IntPtr hWnd, string lpString);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern IntPtr SendMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdcBlt, uint nFlags);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern int GetWindowLong(IntPtr hwnd, int index);

    public static string ClassName(IntPtr hwnd) {
        var text = new StringBuilder(256);
        GetClassName(hwnd, text, text.Capacity);
        return text.ToString();
    }

    public static string WindowText(IntPtr hwnd) {
        var text = new StringBuilder(512);
        GetWindowText(hwnd, text, text.Capacity);
        return text.ToString();
    }

    public static IntPtr FindTopWindow(uint pid, string className, string titleContains) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((hWnd, lParam) => {
            uint windowPid;
            GetWindowThreadProcessId(hWnd, out windowPid);
            if (windowPid != pid) {
                return true;
            }

            string actualClass = ClassName(hWnd);
            string title = WindowText(hWnd);
            bool classMatches = className == null || actualClass == className;
            bool titleMatches = titleContains == null || title.Contains(titleContains);
            if (classMatches && titleMatches) {
                found = hWnd;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@

function Wait-ProcessWindow {
    param(
        [System.Diagnostics.Process]$Process,
        [string]$ClassName,
        [string]$TitleContains = "",
        [int]$TimeoutMs = 10000
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($Process.HasExited) {
            throw "Process exited before window appeared."
        }
        $handle = [NativeUi]::FindTopWindow([uint32]$Process.Id, $ClassName, $(if ($TitleContains) { $TitleContains } else { $null }))
        if ($handle -ne [IntPtr]::Zero -and [NativeUi]::IsWindow($handle)) {
            return $handle
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for window: $ClassName. Windows: $(Get-ProcessWindowSummary -Process $Process)"
}

function Get-ProcessWindowSummary {
    param([System.Diagnostics.Process]$Process)

    $items = New-Object System.Collections.Generic.List[string]
    [NativeUi]::EnumWindows({
        param([IntPtr]$Hwnd, [IntPtr]$Param)
        $windowProcessId = [uint32]0
        [NativeUi]::GetWindowThreadProcessId($Hwnd, [ref]$windowProcessId) | Out-Null
        if ($windowProcessId -eq $Process.Id) {
            $visible = [NativeUi]::IsWindowVisible($Hwnd)
            $items.Add(("{0}|{1}|visible={2}" -f [NativeUi]::ClassName($Hwnd), [NativeUi]::WindowText($Hwnd), $visible)) | Out-Null
        }
        return $true
    }, [IntPtr]::Zero) | Out-Null
    return ($items -join "; ")
}

function Wait-WindowClosed {
    param([IntPtr]$Hwnd, [int]$TimeoutMs = 5000)

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (![NativeUi]::IsWindow($Hwnd)) {
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Window did not close."
}

function Wait-WindowVisible {
    param([IntPtr]$Hwnd, [int]$TimeoutMs = 10000)

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ([NativeUi]::IsWindow($Hwnd) -and [NativeUi]::IsWindowVisible($Hwnd)) {
            Start-Sleep -Milliseconds 300
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Window did not become visible."
}

function Wait-ReportValue {
    param([string]$ReportPath, [string]$Name, [int]$TimeoutMs = 10000)

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path $ReportPath) {
            $line = Get-Content -LiteralPath $ReportPath | Where-Object { $_ -like "$Name=*" } | Select-Object -First 1
            if ($line) {
                return [int]($line.Substring($Name.Length + 1))
            }
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Report value not found: $Name"
}

function Invoke-TextCommand {
    param(
        [System.Diagnostics.Process]$Process,
        [IntPtr]$MainWindow,
        [int]$Command,
        [string]$DialogTitle,
        [string]$Text
    )

    [NativeUi]::PostMessage($MainWindow, 0x0111, [IntPtr]$Command, [IntPtr]::Zero) | Out-Null
    $dialog = Wait-ProcessWindow -Process $Process -ClassName "QuattroTextInputDialog" -TitleContains $DialogTitle
    $edit = [NativeUi]::FindWindowEx($dialog, [IntPtr]::Zero, "Edit", $null)
    if ($edit -eq [IntPtr]::Zero) {
        throw "Text input edit control not found."
    }
    [NativeUi]::SetWindowText($edit, $Text) | Out-Null
    [NativeUi]::PostMessage($dialog, 0x0111, [IntPtr]1, [IntPtr]::Zero) | Out-Null
    Wait-WindowClosed -Hwnd $dialog
}

function Capture-Window {
    param([IntPtr]$Hwnd, [string]$Path)

    [RECT]$rect = New-Object RECT
    if (![NativeUi]::GetWindowRect($Hwnd, [ref]$rect)) {
        throw "Unable to read window rectangle."
    }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -le 0 -or $height -le 0) {
        throw "Invalid window rectangle."
    }

    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $hdc = $graphics.GetHdc()
    try {
        [NativeUi]::PrintWindow($Hwnd, $hdc, 2) | Out-Null
    } finally {
        $graphics.ReleaseHdc($hdc)
        $graphics.Dispose()
    }
    $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
}

$runDir = Join-Path ([System.IO.Path]::GetTempPath()) ("QuattroUiSmoke_" + [Guid]::NewGuid().ToString("N"))
$process = $null

try {
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    Copy-Item -LiteralPath $ExePath -Destination (Join-Path $runDir "Quattro.exe") -Force
    @"
[main]
bAutoDock=0
bHideUnhot=0
bTopMost=0
bHideOnStart=0
bHideNotify=0
bShowTitle=1
bShowGroup=1
bShowTag=1
nWidth=388
nHeight=560
nPosX=80
nPosY=80
Theme=gray
"@ | Set-Content -Path (Join-Path $runDir "conf.ini") -Encoding UTF8

    $testExe = Join-Path $runDir "Quattro.exe"
    $process = Start-Process -FilePath $testExe -WorkingDirectory $runDir -PassThru -WindowStyle Normal
    $main = Wait-ProcessWindow -Process $process -ClassName "QuattroMainWindow" -TitleContains "Quattro"
    Wait-WindowVisible -Hwnd $main
    $report = Join-Path $runDir "logs\startup-report.txt"
    $initialGroups = Wait-ReportValue -ReportPath $report -Name "major_groups"
    $initialTags = Wait-ReportValue -ReportPath $report -Name "tags"

    [NativeUi]::SendMessage($main, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    if ($process.HasExited) {
        throw "WM_CLOSE exited the process; expected tray hide."
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([NativeUi]::IsWindowVisible($main) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }
    if ([NativeUi]::IsWindowVisible($main)) {
        $style = [NativeUi]::GetWindowLong($main, -16)
        throw "WM_CLOSE did not hide the main window. style=0x$($style.ToString('X')) windows=$(Get-ProcessWindowSummary -Process $process)"
    }

    [NativeUi]::PostMessage($main, 0x8065, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while (![NativeUi]::IsWindowVisible($main) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }
    if (![NativeUi]::IsWindowVisible($main)) {
        throw "Main window did not wake after tray-hide test."
    }

    [NativeUi]::SendMessage($main, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([NativeUi]::IsWindowVisible($main) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }
    if ([NativeUi]::IsWindowVisible($main)) {
        throw "WM_CLOSE did not hide before tray-click restore test."
    }
    [NativeUi]::PostMessage($main, 0x8066, [IntPtr]::Zero, [IntPtr]0x0202) | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while (![NativeUi]::IsWindowVisible($main) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }
    if (![NativeUi]::IsWindowVisible($main)) {
        throw "Tray left-click did not restore the main window."
    }

    [NativeUi]::SetForegroundWindow($main) | Out-Null
    [NativeUi]::SendMessage($main, 0x0111, [IntPtr]40009, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 300
    [NativeUi]::SendMessage($main, 0x0111, [IntPtr]40012, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 500

    $screenshot = Join-Path $LogDir "ui-smoke-main.png"
    Capture-Window -Hwnd $main -Path $screenshot

    [NativeUi]::PostMessage($main, 0x0111, [IntPtr]40005, [IntPtr]::Zero) | Out-Null
    if (!$process.WaitForExit(5000)) {
        throw "Process did not exit through tray exit command."
    }

    Remove-Item -LiteralPath $report -Force -ErrorAction SilentlyContinue
    $process = Start-Process -FilePath $testExe -WorkingDirectory $runDir -PassThru -WindowStyle Normal
    $main = Wait-ProcessWindow -Process $process -ClassName "QuattroMainWindow" -TitleContains "Quattro"
    $finalGroups = Wait-ReportValue -ReportPath $report -Name "major_groups"
    $finalTags = Wait-ReportValue -ReportPath $report -Name "tags"
    if ($finalGroups -lt ($initialGroups + 1)) {
        throw "Group count did not increase. initial=$initialGroups final=$finalGroups"
    }
    if ($finalTags -lt ($initialTags + 2)) {
        throw "Tag count did not increase enough. initial=$initialTags final=$finalTags"
    }

    Copy-Item -LiteralPath $report -Destination (Join-Path $LogDir "ui-smoke-startup-report.txt") -Force
    [NativeUi]::PostMessage($main, 0x0111, [IntPtr]40005, [IntPtr]::Zero) | Out-Null
    $process.WaitForExit(5000) | Out-Null

    "ui_smoke=passed"
    "initial_major_groups=$initialGroups"
    "final_major_groups=$finalGroups"
    "initial_tags=$initialTags"
    "final_tags=$finalTags"
    "screenshot=$screenshot"
} finally {
    if (![string]::IsNullOrWhiteSpace($runDir)) {
        $appLog = Join-Path $runDir "logs\app.log"
        if (Test-Path $appLog) {
            Copy-Item -LiteralPath $appLog -Destination (Join-Path $LogDir "ui-smoke-app.log") -Force
        }
    }
    if ($process -and !$process.HasExited) {
        try {
            [NativeUi]::PostMessage($main, 0x0111, [IntPtr]40005, [IntPtr]::Zero) | Out-Null
            if (!$process.WaitForExit(2000)) {
                Stop-Process -Id $process.Id -Force
            }
        } catch {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
    }
    Remove-Item -LiteralPath $runDir -Recurse -Force -ErrorAction SilentlyContinue
}
