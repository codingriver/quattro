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
if ([string]::IsNullOrWhiteSpace($LogDir)) {
    $LogDir = Join-Path (Join-Path $root "build\$Configuration") "logs"
}
if (!(Test-Path $ExePath)) {
    throw "Executable not found: $ExePath"
}
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

Add-Type -AssemblyName System.Drawing

if (-not ([System.Management.Automation.PSTypeName]'NativeDialogUi').Type) {
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public struct DialogRect {
    public int Left;
    public int Top;
    public int Right;
    public int Bottom;
}

public static class NativeDialogUi {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr hWndParent, EnumWindowsProc lpEnumFunc, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr hWnd, StringBuilder lpClassName, int nMaxCount);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);

    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool MoveWindow(IntPtr hWnd, int x, int y, int width, int height, bool repaint);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hwnd, out DialogRect rect);

    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdcBlt, uint nFlags);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr hDlg, int nIDDlgItem);

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
            bool classMatches = className == null ||
                actualClass == className ||
                (className.EndsWith("*") && actualClass.StartsWith(className.Substring(0, className.Length - 1)));
            bool titleMatches = titleContains == null || title.Contains(titleContains);
            if (classMatches && titleMatches) {
                found = hWnd;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public static string OutOfBoundsVisibleChildren(IntPtr parent, int padding) {
        DialogRect parentRect;
        if (!GetWindowRect(parent, out parentRect)) {
            return "parent-rect-unavailable";
        }

        var output = new StringBuilder();
        EnumChildWindows(parent, (hWnd, lParam) => {
            if (!IsWindowVisible(hWnd)) {
                return true;
            }
            DialogRect childRect;
            if (!GetWindowRect(hWnd, out childRect)) {
                output.Append("child-rect-unavailable;");
                return true;
            }
            bool outside =
                childRect.Left < parentRect.Left - padding ||
                childRect.Top < parentRect.Top - padding ||
                childRect.Right > parentRect.Right + padding ||
                childRect.Bottom > parentRect.Bottom + padding;
            if (outside) {
                output.Append(ClassName(hWnd));
                output.Append("|");
                output.Append(WindowText(hWnd));
                output.Append("|");
                output.Append(childRect.Left);
                output.Append(",");
                output.Append(childRect.Top);
                output.Append(",");
                output.Append(childRect.Right);
                output.Append(",");
                output.Append(childRect.Bottom);
                output.Append(";");
            }
            return true;
        }, IntPtr.Zero);
        return output.ToString();
    }
}
'@
}

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
            throw "Process exited before window appeared: $ClassName"
        }
        $handle = [NativeDialogUi]::FindTopWindow([uint32]$Process.Id, $ClassName, $(if ($TitleContains) { $TitleContains } else { $null }))
        if ($handle -ne [IntPtr]::Zero -and [NativeDialogUi]::IsWindow($handle)) {
            return $handle
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for window: $ClassName"
}

function Wait-WindowClosed {
    param([IntPtr]$Hwnd, [int]$TimeoutMs = 5000)

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (![NativeDialogUi]::IsWindow($Hwnd)) {
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Window did not close."
}

function Capture-Window {
    param([IntPtr]$Hwnd, [string]$Path)

    [DialogRect]$rect = New-Object DialogRect
    if (![NativeDialogUi]::GetWindowRect($Hwnd, [ref]$rect)) {
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
        [NativeDialogUi]::PrintWindow($Hwnd, $hdc, 2) | Out-Null
    } finally {
        $graphics.ReleaseHdc($hdc)
        $graphics.Dispose()
    }
    $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
}

function Assert-UsefulImage {
    param([string]$Path, [string]$Name)

    $bitmap = [System.Drawing.Bitmap]::FromFile($Path)
    try {
        $colors = New-Object 'System.Collections.Generic.HashSet[int]'
        for ($y = 0; $y -lt $bitmap.Height; $y += 16) {
            for ($x = 0; $x -lt $bitmap.Width; $x += 16) {
                [void]$colors.Add($bitmap.GetPixel($x, $y).ToArgb())
            }
        }
        if ($colors.Count -lt 4) {
            throw "Screenshot appears blank: $Name"
        }
    } finally {
        $bitmap.Dispose()
    }
}

function Assert-DialogSurface {
    param([IntPtr]$Hwnd, [string]$Name, [string]$Screenshot, [int[]]$RequiredControlIds = @())

    foreach ($id in $RequiredControlIds) {
        $child = [IntPtr]::Zero
        $deadline = [DateTime]::UtcNow.AddSeconds(5)
        while ([DateTime]::UtcNow -lt $deadline) {
            $child = [NativeDialogUi]::GetDlgItem($Hwnd, $id)
            if ($child -ne [IntPtr]::Zero -and [NativeDialogUi]::IsWindow($child)) {
                break
            }
            Start-Sleep -Milliseconds 50
        }
        if ($child -eq [IntPtr]::Zero -or ![NativeDialogUi]::IsWindow($child)) {
            throw "Required control missing in ${Name}: $id"
        }
    }
    $outside = [NativeDialogUi]::OutOfBoundsVisibleChildren($Hwnd, 4)
    if (![string]::IsNullOrWhiteSpace($outside)) {
        $outside | Set-Content -Path (Join-Path $LogDir ("p2-" + $Name + "-bounds.txt")) -Encoding UTF8
        throw "Visible child control outside parent in $Name"
    }
    Capture-Window -Hwnd $Hwnd -Path $Screenshot
    Assert-UsefulImage -Path $Screenshot -Name $Name
}

$runDir = Join-Path ([System.IO.Path]::GetTempPath()) ("QuattroDialogTests_" + [Guid]::NewGuid().ToString("N"))
$process = $null

try {
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    Copy-Item -LiteralPath $ExePath -Destination (Join-Path $runDir "Quattro.exe") -Force

    $process = Start-Process -FilePath (Join-Path $runDir "Quattro.exe") -WorkingDirectory $runDir -PassThru -WindowStyle Normal
    $main = Wait-ProcessWindow -Process $process -ClassName "QuattroMainWindow" -TitleContains "Quattro"
    [NativeDialogUi]::SetForegroundWindow($main) | Out-Null
    Start-Sleep -Milliseconds 300

    $mainSmall = Join-Path $LogDir "p2-main-small.png"
    [NativeDialogUi]::MoveWindow($main, 80, 80, 460, 320, $true) | Out-Null
    Start-Sleep -Milliseconds 400
    Capture-Window -Hwnd $main -Path $mainSmall
    Assert-UsefulImage -Path $mainSmall -Name "main-small"

    $mainLarge = Join-Path $LogDir "p2-main-large.png"
    [NativeDialogUi]::MoveWindow($main, 80, 80, 860, 620, $true) | Out-Null
    Start-Sleep -Milliseconds 400
    Capture-Window -Hwnd $main -Path $mainLarge
    Assert-UsefulImage -Path $mainLarge -Name "main-large"

    [NativeDialogUi]::PostMessage($main, 0x0111, [IntPtr]40010, [IntPtr]::Zero) | Out-Null
    $textDialog = Wait-ProcessWindow -Process $process -ClassName "QuattroTextInputDialog_*"
    Assert-DialogSurface -Hwnd $textDialog -Name "text-dialog" -Screenshot (Join-Path $LogDir "p2-dialog-text.png")
    [NativeDialogUi]::PostMessage($textDialog, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Wait-WindowClosed -Hwnd $textDialog

    [NativeDialogUi]::PostMessage($main, 0x0111, [IntPtr]40001, [IntPtr]::Zero) | Out-Null
    $linkDialog = Wait-ProcessWindow -Process $process -ClassName "QuattroLinkEditDialog"
    Assert-DialogSurface -Hwnd $linkDialog -Name "link-dialog" -Screenshot (Join-Path $LogDir "p2-dialog-link.png") -RequiredControlIds @(1003,1004,1007,1008,1010,1012,1014,1015,1017,1018,1019)
    [NativeDialogUi]::PostMessage($linkDialog, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Wait-WindowClosed -Hwnd $linkDialog

    [NativeDialogUi]::PostMessage($main, 0x0111, [IntPtr]40015, [IntPtr]::Zero) | Out-Null
    $searchDialog = Wait-ProcessWindow -Process $process -ClassName "QuattroSearchDialog"
    Assert-DialogSurface -Hwnd $searchDialog -Name "search-dialog" -Screenshot (Join-Path $LogDir "p2-dialog-search.png")
    [NativeDialogUi]::PostMessage($searchDialog, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Wait-WindowClosed -Hwnd $searchDialog

    [NativeDialogUi]::PostMessage($main, 0x0111, [IntPtr]40016, [IntPtr]::Zero) | Out-Null
    $settingsDialog = Wait-ProcessWindow -Process $process -ClassName "QuattroSettingsDialog"
    Assert-DialogSurface -Hwnd $settingsDialog -Name "settings-dialog" -Screenshot (Join-Path $LogDir "p2-dialog-settings.png")
    [NativeDialogUi]::PostMessage($settingsDialog, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Wait-WindowClosed -Hwnd $settingsDialog

    [NativeDialogUi]::PostMessage($main, 0x0111, [IntPtr]40005, [IntPtr]::Zero) | Out-Null
    if (!$process.WaitForExit(5000)) {
        throw "Process did not exit."
    }

    "dialog_display_tests=passed"
    "screenshots=$mainSmall;$mainLarge;p2-dialog-text.png;p2-dialog-link.png;p2-dialog-search.png;p2-dialog-settings.png"
} finally {
    if ($process -and !$process.HasExited) {
        try {
            [NativeDialogUi]::PostMessage($main, 0x0111, [IntPtr]40005, [IntPtr]::Zero) | Out-Null
            if (!$process.WaitForExit(2000)) {
                Stop-Process -Id $process.Id -Force
            }
        } catch {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
    }
    Remove-Item -LiteralPath $runDir -Recurse -Force -ErrorAction SilentlyContinue
}
