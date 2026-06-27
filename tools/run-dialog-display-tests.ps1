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
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public struct DialogRect {
    public int Left;
    public int Top;
    public int Right;
    public int Bottom;
}

[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct LogFontRect {
    public int lfHeight;
    public int lfWidth;
    public int lfEscapement;
    public int lfOrientation;
    public int lfWeight;
    public byte lfItalic;
    public byte lfUnderline;
    public byte lfStrikeOut;
    public byte lfCharSet;
    public byte lfOutPrecision;
    public byte lfClipPrecision;
    public byte lfQuality;
    public byte lfPitchAndFamily;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
    public string lfFaceName;
}

public static class NativeDialogUi {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    private struct ChildInfo {
        public IntPtr Hwnd;
        public string ClassName;
        public string Text;
        public DialogRect Rect;
    }

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

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool SetWindowText(IntPtr hWnd, string lpString);

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

    [DllImport("user32.dll")]
    public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("gdi32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetObject(IntPtr hgdiobj, int cbBuffer, out LogFontRect obj);

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

    private static bool IsInteractiveClass(string className) {
        return className == "Edit" ||
            className == "ComboBox" ||
            className == "Button" ||
            className == "SysListView32" ||
            className == "ListBox";
    }

    private static int IntersectWidth(DialogRect left, DialogRect right) {
        return Math.Min(left.Right, right.Right) - Math.Max(left.Left, right.Left);
    }

    private static int IntersectHeight(DialogRect left, DialogRect right) {
        return Math.Min(left.Bottom, right.Bottom) - Math.Max(left.Top, right.Top);
    }

    private static bool IsLabelInteractiveOverlap(ChildInfo left, ChildInfo right, int minimumArea) {
        bool leftLabel = left.ClassName == "Static" && !String.IsNullOrWhiteSpace(left.Text);
        bool rightLabel = right.ClassName == "Static" && !String.IsNullOrWhiteSpace(right.Text);
        bool labelInteractivePair =
            (leftLabel && IsInteractiveClass(right.ClassName)) ||
            (rightLabel && IsInteractiveClass(left.ClassName));
        if (!labelInteractivePair) {
            return false;
        }

        int width = IntersectWidth(left.Rect, right.Rect);
        int height = IntersectHeight(left.Rect, right.Rect);
        return width > 3 && height > 3 && width * height >= minimumArea;
    }

    public static string OverlappingVisibleChildren(IntPtr parent, int minimumArea) {
        var children = new List<ChildInfo>();
        EnumChildWindows(parent, (hWnd, lParam) => {
            if (!IsWindowVisible(hWnd)) {
                return true;
            }
            DialogRect childRect;
            if (!GetWindowRect(hWnd, out childRect)) {
                return true;
            }
            children.Add(new ChildInfo {
                Hwnd = hWnd,
                ClassName = ClassName(hWnd),
                Text = WindowText(hWnd),
                Rect = childRect
            });
            return true;
        }, IntPtr.Zero);

        var output = new StringBuilder();
        for (int i = 0; i < children.Count; ++i) {
            for (int j = i + 1; j < children.Count; ++j) {
                ChildInfo left = children[i];
                ChildInfo right = children[j];
                if (!IsLabelInteractiveOverlap(left, right, minimumArea)) {
                    continue;
                }
                output.Append(left.ClassName);
                output.Append("|");
                output.Append(left.Text);
                output.Append("|");
                output.Append(left.Rect.Left);
                output.Append(",");
                output.Append(left.Rect.Top);
                output.Append(",");
                output.Append(left.Rect.Right);
                output.Append(",");
                output.Append(left.Rect.Bottom);
                output.Append("<->");
                output.Append(right.ClassName);
                output.Append("|");
                output.Append(right.Text);
                output.Append("|");
                output.Append(right.Rect.Left);
                output.Append(",");
                output.Append(right.Rect.Top);
                output.Append(",");
                output.Append(right.Rect.Right);
                output.Append(",");
                output.Append(right.Rect.Bottom);
                output.Append(";");
            }
        }
        return output.ToString();
    }
}
'@
}

$themeXml = [xml](Get-Content (Join-Path $root "theme\\default.xml"))
$editComponent = $themeXml.Theme.Component | Where-Object { $_.name -eq "edit" }
$expectedSingleLineFontPx = [int](($editComponent.Metric | Where-Object { $_.name -eq "singleLineFontSizePx" }).value)

function Get-ControlFontHeight {
    param([IntPtr]$Hwnd)

    $font = [NativeDialogUi]::SendMessage($Hwnd, 0x0031, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($font -eq [IntPtr]::Zero) {
        throw "Control has no font handle."
    }
    $fontInfo = [LogFontRect]::new()
    $fontSize = [System.Runtime.InteropServices.Marshal]::SizeOf($fontInfo)
    [void][NativeDialogUi]::GetObject($font, $fontSize, [ref]$fontInfo)
    return $fontInfo.lfHeight
}

function Assert-ControlFontHeight {
    param(
        [IntPtr]$Hwnd,
        [string]$Name,
        [int]$ExpectedPx
    )

    $actualHeight = Get-ControlFontHeight -Hwnd $Hwnd
    if ([math]::Abs($actualHeight) -ne $ExpectedPx) {
        throw "Unexpected font height for ${Name}: expected $ExpectedPx, actual $actualHeight"
    }
}

function Get-ProcessWindowSummary {
    param([System.Diagnostics.Process]$Process)

    $items = New-Object System.Collections.Generic.List[string]
    [NativeDialogUi]::EnumWindows({
        param([IntPtr]$Hwnd, [IntPtr]$Param)
        $windowProcessId = [uint32]0
        [NativeDialogUi]::GetWindowThreadProcessId($Hwnd, [ref]$windowProcessId) | Out-Null
        if ($windowProcessId -eq $Process.Id) {
            $visible = [NativeDialogUi]::IsWindowVisible($Hwnd)
            $items.Add(("{0}|{1}|visible={2}" -f [NativeDialogUi]::ClassName($Hwnd), [NativeDialogUi]::WindowText($Hwnd), $visible)) | Out-Null
        }
        return $true
    }, [IntPtr]::Zero) | Out-Null
    return ($items -join "; ")
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
    throw "Timed out waiting for window: $ClassName. Windows: $(Get-ProcessWindowSummary -Process $Process)"
}

function Find-ProcessWindow {
    param(
        [System.Diagnostics.Process]$Process,
        [string]$ClassName,
        [string]$TitleContains = ""
    )

    if ($Process.HasExited) {
        return [IntPtr]::Zero
    }
    return [NativeDialogUi]::FindTopWindow([uint32]$Process.Id, $ClassName, $(if ($TitleContains) { $TitleContains } else { $null }))
}

function Try-AcceptTextDialog {
    param(
        [System.Diagnostics.Process]$Process,
        [string]$TitleContains,
        [string]$Text,
        [int]$TimeoutMs = 800
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        $dialog = Find-ProcessWindow -Process $Process -ClassName "QuattroTextInputDialog_*" -TitleContains $TitleContains
        if ($dialog -ne [IntPtr]::Zero -and [NativeDialogUi]::IsWindow($dialog)) {
            [NativeDialogUi]::SetWindowText([NativeDialogUi]::GetDlgItem($dialog, 100), $Text) | Out-Null
            [NativeDialogUi]::PostMessage($dialog, 0x0111, [IntPtr]1, [IntPtr]::Zero) | Out-Null
            Wait-WindowClosed -Hwnd $dialog
            return $true
        }
        Start-Sleep -Milliseconds 50
    }
    return $false
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
    Start-Sleep -Milliseconds 250

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
    $overlap = [NativeDialogUi]::OverlappingVisibleChildren($Hwnd, 16)
    if (![string]::IsNullOrWhiteSpace($overlap)) {
        $overlap | Set-Content -Path (Join-Path $LogDir ("p2-" + $Name + "-overlap.txt")) -Encoding UTF8
        throw "Visible label overlaps an interactive control in $Name"
    }
    Capture-Window -Hwnd $Hwnd -Path $Screenshot
    Assert-UsefulImage -Path $Screenshot -Name $Name
}

$runDir = Join-Path ([System.IO.Path]::GetTempPath()) ("QuattroDialogTests_" + [Guid]::NewGuid().ToString("N"))
$process = $null
$previousNoFocus = $env:QUATTRO_TEST_NO_FOCUS
$env:QUATTRO_TEST_NO_FOCUS = '1'

try {
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    Copy-Item -LiteralPath $ExePath -Destination (Join-Path $runDir "Quattro.exe") -Force

    $process = Start-Process -FilePath (Join-Path $runDir "Quattro.exe") -WorkingDirectory $runDir -PassThru -WindowStyle Normal
    $main = Wait-ProcessWindow -Process $process -ClassName "QuattroMainWindow" -TitleContains "Quattro"
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

    [NativeDialogUi]::PostMessage($main, 0x0111, [IntPtr]40009, [IntPtr]::Zero) | Out-Null
    $textDialog = Wait-ProcessWindow -Process $process -ClassName "QuattroTextInputDialog_*"
    Assert-DialogSurface -Hwnd $textDialog -Name "text-dialog" -Screenshot (Join-Path $LogDir "p2-dialog-text.png")
    Assert-ControlFontHeight -Hwnd ([NativeDialogUi]::GetDlgItem($textDialog, 100)) -Name "text-dialog.edit" -ExpectedPx $expectedSingleLineFontPx
    [NativeDialogUi]::SetWindowText([NativeDialogUi]::GetDlgItem($textDialog, 100), "DialogTestGroup") | Out-Null
    [NativeDialogUi]::PostMessage($textDialog, 0x0111, [IntPtr]1, [IntPtr]::Zero) | Out-Null
    Wait-WindowClosed -Hwnd $textDialog

    [NativeDialogUi]::PostMessage($main, 0x0111, [IntPtr]40001, [IntPtr]::Zero) | Out-Null
    $linkDialog = Wait-ProcessWindow -Process $process -ClassName "QuattroLinkEditDialog"
    Assert-DialogSurface -Hwnd $linkDialog -Name "link-dialog" -Screenshot (Join-Path $LogDir "p2-dialog-link.png") -RequiredControlIds @(1003,1004,1007,1008,1010,1012,1014,1015,1017,1018,1019)
    Assert-ControlFontHeight -Hwnd ([NativeDialogUi]::GetDlgItem($linkDialog, 1003)) -Name "link-dialog.name" -ExpectedPx $expectedSingleLineFontPx
    [NativeDialogUi]::PostMessage($linkDialog, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Wait-WindowClosed -Hwnd $linkDialog

    [NativeDialogUi]::SendMessage($main, 0x0111, [IntPtr]40053, [IntPtr]::Zero) | Out-Null
    if (Try-AcceptTextDialog -Process $process -TitleContains "新建分组" -Text "DialogTodoGroup") {
        [NativeDialogUi]::SendMessage($main, 0x0111, [IntPtr]40053, [IntPtr]::Zero) | Out-Null
    }
    Start-Sleep -Milliseconds 500
    [NativeDialogUi]::PostMessage($main, 0x0111, [IntPtr]40054, [IntPtr]::Zero) | Out-Null
    $todoDialog = Wait-ProcessWindow -Process $process -ClassName "QuattroTodoEditDialog"
    Assert-DialogSurface -Hwnd $todoDialog -Name "todo-dialog" -Screenshot (Join-Path $LogDir "p2-dialog-todo.png") -RequiredControlIds @(101,102,103,104,105)
    Assert-ControlFontHeight -Hwnd ([NativeDialogUi]::GetDlgItem($todoDialog, 101)) -Name "todo-dialog.title" -ExpectedPx $expectedSingleLineFontPx
    Assert-ControlFontHeight -Hwnd ([NativeDialogUi]::GetDlgItem($todoDialog, 102)) -Name "todo-dialog.content" -ExpectedPx $expectedSingleLineFontPx
    Assert-ControlFontHeight -Hwnd ([NativeDialogUi]::GetDlgItem($todoDialog, 104)) -Name "todo-dialog.time" -ExpectedPx $expectedSingleLineFontPx
    [NativeDialogUi]::PostMessage($todoDialog, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Wait-WindowClosed -Hwnd $todoDialog

    [NativeDialogUi]::PostMessage($main, 0x0111, [IntPtr]40015, [IntPtr]::Zero) | Out-Null
    $searchDialog = Wait-ProcessWindow -Process $process -ClassName "QuattroSearchDialog"
    Assert-DialogSurface -Hwnd $searchDialog -Name "search-dialog" -Screenshot (Join-Path $LogDir "p2-dialog-search.png")
    Assert-ControlFontHeight -Hwnd ([NativeDialogUi]::GetDlgItem($searchDialog, 100)) -Name "search-dialog.edit" -ExpectedPx $expectedSingleLineFontPx
    [NativeDialogUi]::PostMessage($searchDialog, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Wait-WindowClosed -Hwnd $searchDialog

    [NativeDialogUi]::PostMessage($main, 0x0111, [IntPtr]40016, [IntPtr]::Zero) | Out-Null
    $settingsDialog = Wait-ProcessWindow -Process $process -ClassName "QuattroSettingsDialog"
    Assert-DialogSurface -Hwnd $settingsDialog -Name "settings-dialog" -Screenshot (Join-Path $LogDir "p2-dialog-settings.png")
    Assert-ControlFontHeight -Hwnd ([NativeDialogUi]::GetDlgItem($settingsDialog, 401)) -Name "settings-dialog.group-width" -ExpectedPx $expectedSingleLineFontPx
    Assert-ControlFontHeight -Hwnd ([NativeDialogUi]::GetDlgItem($settingsDialog, 203)) -Name "settings-dialog.help-url" -ExpectedPx $expectedSingleLineFontPx
    [NativeDialogUi]::PostMessage($settingsDialog, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Wait-WindowClosed -Hwnd $settingsDialog

    [NativeDialogUi]::PostMessage($main, 0x0111, [IntPtr]40005, [IntPtr]::Zero) | Out-Null
    if (!$process.WaitForExit(5000)) {
        throw "Process did not exit."
    }

    "dialog_display_tests=passed"
    "screenshots=$mainSmall;$mainLarge;p2-dialog-text.png;p2-dialog-link.png;p2-dialog-todo.png;p2-dialog-search.png;p2-dialog-settings.png"
} finally {
    if ($null -eq $previousNoFocus) {
        Remove-Item Env:\QUATTRO_TEST_NO_FOCUS -ErrorAction SilentlyContinue
    } else {
        $env:QUATTRO_TEST_NO_FOCUS = $previousNoFocus
    }
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


