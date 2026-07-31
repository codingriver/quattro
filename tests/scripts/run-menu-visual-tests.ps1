param(
    [string]$ExePath = "",
    [string]$Configuration = "Release",
    [string]$LogDir = "",
    [switch]$ToolboxMenu,
    [switch]$InteractiveVisual
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
. (Join-Path $PSScriptRoot "QuattroTestHarness.ps1")

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

if (-not ([System.Management.Automation.PSTypeName]'NativeMenuVisualUi').Type) {
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public struct MenuVisualRect {
    public int Left;
    public int Top;
    public int Right;
    public int Bottom;
}

public static class NativeMenuVisualUi {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

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
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hwnd, out MenuVisualRect rect);

    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr hwnd, out MenuVisualRect rect);

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
            bool classMatches = ClassName(hWnd) == className;
            bool titleMatches = titleContains == null || WindowText(hWnd).Contains(titleContains);
            if (classMatches && titleMatches) {
                found = hWnd;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public static IntPtr FindVisiblePopupMenu() {
        IntPtr found = IntPtr.Zero;
        EnumWindows((hWnd, lParam) => {
            if (ClassName(hWnd) == "#32768" && IsWindowVisible(hWnd)) {
                found = hWnd;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public static IntPtr FindRightmostVisiblePopupMenu() {
        IntPtr found = IntPtr.Zero;
        int foundLeft = Int32.MinValue;
        EnumWindows((hWnd, lParam) => {
            if (ClassName(hWnd) == "#32768" && IsWindowVisible(hWnd)) {
                MenuVisualRect rect;
                if (GetWindowRect(hWnd, out rect) && rect.Left > foundLeft) {
                    found = hWnd;
                    foundLeft = rect.Left;
                }
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public static IntPtr MakeLParam(int low, int high) {
        return (IntPtr)((high << 16) | (low & 0xffff));
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
        $handle = [NativeMenuVisualUi]::FindTopWindow([uint32]$Process.Id, $ClassName, $(if ($TitleContains) { $TitleContains } else { $null }))
        if ($handle -ne [IntPtr]::Zero -and [NativeMenuVisualUi]::IsWindow($handle)) {
            Set-QuattroTestWindowBackground -Process $Process -Window $handle -Context "wait for $ClassName"
            return $handle
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for window: $ClassName"
}

function Wait-PopupMenu {
    param([int]$TimeoutMs = 5000)

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        $popup = [NativeMenuVisualUi]::FindVisiblePopupMenu()
        if ($popup -ne [IntPtr]::Zero -and [NativeMenuVisualUi]::IsWindow($popup)) {
            return $popup
        }
        Start-Sleep -Milliseconds 80
    }
    throw "Timed out waiting for popup menu."
}

function Wait-RightmostPopupMenu {
    param([int]$MinimumLeft, [int]$TimeoutMs = 5000)

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        $popup = [NativeMenuVisualUi]::FindRightmostVisiblePopupMenu()
        if ($popup -ne [IntPtr]::Zero -and [NativeMenuVisualUi]::IsWindow($popup)) {
            [MenuVisualRect]$rect = New-Object MenuVisualRect
            if ([NativeMenuVisualUi]::GetWindowRect($popup, [ref]$rect) -and $rect.Left -ge $MinimumLeft) {
                return $popup
            }
        }
        Start-Sleep -Milliseconds 80
    }
    throw "Timed out waiting for submenu popup."
}

function Capture-Window {
    param([IntPtr]$Hwnd, [string]$Path, [int]$MinimumHeight = 220)

    [MenuVisualRect]$rect = New-Object MenuVisualRect
    if (![NativeMenuVisualUi]::GetWindowRect($Hwnd, [ref]$rect)) {
        throw "Unable to read popup rectangle."
    }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -lt 120 -or $height -lt $MinimumHeight) {
        throw "Popup rectangle is too small: ${width}x${height}"
    }

    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $printed = $false
    try {
        $hdc = $graphics.GetHdc()
        try {
            $printed = [NativeMenuVisualUi]::PrintWindow($Hwnd, $hdc, 2)
        } finally {
            $graphics.ReleaseHdc($hdc)
        }
        if (!$printed) {
            throw "PrintWindow failed for popup hwnd=$Hwnd"
        }
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

function Assert-MenuScreenshot {
    param([string]$Path)

    $bitmap = [System.Drawing.Bitmap]::FromFile($Path)
    try {
        $colors = New-Object 'System.Collections.Generic.HashSet[int]'
        $stateBluePixels = 0
        $leftIconInkPixels = 0
        $rightArrowPixels = 0
        $separatorPixels = 0

        for ($y = 0; $y -lt $bitmap.Height; ++$y) {
            for ($x = 0; $x -lt $bitmap.Width; ++$x) {
                $color = $bitmap.GetPixel($x, $y)
                if (($x % 12) -eq 0 -and ($y % 12) -eq 0) {
                    [void]$colors.Add($color.ToArgb())
                }

                if ($x -lt 34 -and $color.B -gt 150 -and $color.G -gt 100 -and $color.R -lt 90) {
                    ++$stateBluePixels
                }
                if ($x -lt 34 -and $color.R -lt 245 -and $color.G -lt 245 -and $color.B -lt 245) {
                    ++$leftIconInkPixels
                }
                if ($x -gt ($bitmap.Width - 24) -and $color.R -lt 100 -and $color.G -lt 110 -and $color.B -lt 150) {
                    ++$rightArrowPixels
                }
                if ($color.R -ge 180 -and $color.R -le 245 -and $color.G -ge 180 -and $color.G -le 245 -and $color.B -ge 180 -and $color.B -le 250) {
                    ++$separatorPixels
                }
            }
        }

        if ($colors.Count -lt 8) {
            throw "Menu screenshot appears blank."
        }
        if ($stateBluePixels -lt 20) {
            throw "Menu screenshot does not show selected-state/icon blue pixels. bluePixels=$stateBluePixels"
        }
        if ($leftIconInkPixels -lt 180) {
            throw "Menu screenshot does not show enough menu icons. inkPixels=$leftIconInkPixels"
        }
        if ($rightArrowPixels -lt 8) {
            throw "Menu screenshot does not show submenu arrows. arrowPixels=$rightArrowPixels"
        }
        if ($separatorPixels -lt 30) {
            throw "Menu screenshot does not show separators/borders. separatorPixels=$separatorPixels"
        }
    } finally {
        $bitmap.Dispose()
    }
}

function Assert-SubmenuScreenshot {
    param([string]$Path)

    $bitmap = [System.Drawing.Bitmap]::FromFile($Path)
    try {
        $blueIconPixels = 0
        $darkIconPixels = 0
        for ($y = 0; $y -lt $bitmap.Height; ++$y) {
            for ($x = 0; $x -lt [Math]::Min(70, $bitmap.Width); ++$x) {
                $color = $bitmap.GetPixel($x, $y)
                if ($color.B -gt 150 -and $color.G -gt 100 -and $color.R -lt 110) {
                    ++$blueIconPixels
                }
                if ($color.R -lt 100 -and $color.G -lt 110 -and $color.B -lt 150) {
                    ++$darkIconPixels
                }
            }
        }
        if ($blueIconPixels -lt 150) {
            throw "System-function submenu has too few blue local/menu icons. bluePixels=$blueIconPixels"
        }
        if ($darkIconPixels -lt 25) {
            throw "System-function submenu has too few dark system/action icons. darkPixels=$darkIconPixels"
        }
    } finally {
        $bitmap.Dispose()
    }
}

function Get-SystemSubmenuArrowY {
    param([string]$Path)

    $bitmap = [System.Drawing.Bitmap]::FromFile($Path)
    try {
        $ys = New-Object 'System.Collections.Generic.List[int]'
        for ($y = 190; $y -lt [Math]::Min(320, $bitmap.Height); ++$y) {
            for ($x = [Math]::Max(0, $bitmap.Width - 26); $x -lt ($bitmap.Width - 4); ++$x) {
                $color = $bitmap.GetPixel($x, $y)
                if ($color.R -lt 100 -and $color.G -lt 110 -and $color.B -lt 150) {
                    $ys.Add($y)
                    break
                }
            }
        }
        if ($ys.Count -eq 0) {
            throw "Unable to locate system submenu arrow in screenshot."
        }
        $start = $ys[0]
        $previous = $ys[0]
        for ($i = 1; $i -lt $ys.Count; ++$i) {
            if ($ys[$i] -gt ($previous + 1)) {
                return [int](($start + $previous) / 2)
            }
            $previous = $ys[$i]
        }
        return [int](($start + $previous) / 2)
    } finally {
        $bitmap.Dispose()
    }
}

$runDir = Join-Path ([System.IO.Path]::GetTempPath()) ("QuattroMenuVisualTests_" + [Guid]::NewGuid().ToString("N"))
$process = $null

try {
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    Copy-Item -LiteralPath $ExePath -Destination (Join-Path $runDir "Quattro.exe") -Force
    $sourceIcons = Join-Path $root "icons"
    if (Test-Path $sourceIcons) {
        Copy-Item -LiteralPath $sourceIcons -Destination $runDir -Recurse -Force
    }
    @"
[main]
bTopMost=0
bGlobalHotKeysEnabled=0
bShowTitle=1
bShowGroup=1
bShowTag=1
bShowBtnMenu=1
bShowBtnSearch=1
nWidth=388
nHeight=560
nPosX=120
nPosY=120
Theme=default
"@ | Set-Content -Path (Join-Path $runDir "conf.ini") -Encoding UTF8

    $process = Start-QuattroTestProcess -FilePath (Join-Path $runDir "Quattro.exe") -WorkingDirectory $runDir
    $main = Wait-ProcessWindow -Process $process -ClassName "QuattroMainWindow" -TitleContains "Quattro"
    Start-Sleep -Milliseconds 500

    [MenuVisualRect]$mainRect = New-Object MenuVisualRect
    [NativeMenuVisualUi]::GetWindowRect($main, [ref]$mainRect) | Out-Null
    [MenuVisualRect]$clientRect = New-Object MenuVisualRect
    [NativeMenuVisualUi]::GetClientRect($main, [ref]$clientRect) | Out-Null
    Start-Sleep -Milliseconds 150

    $menuButtonX = ($clientRect.Right - $clientRect.Left) - $(if ($ToolboxMenu) { 75 } else { 45 })
    $menuButtonY = 17
    $lparam = [NativeMenuVisualUi]::MakeLParam($menuButtonX, $menuButtonY)
    [NativeMenuVisualUi]::PostMessage($main, 0x0201, [IntPtr]1, $lparam) | Out-Null
    Start-Sleep -Milliseconds 80
    [NativeMenuVisualUi]::PostMessage($main, 0x0202, [IntPtr]::Zero, $lparam) | Out-Null

    $popup = Wait-PopupMenu
    Start-Sleep -Milliseconds 250
    $screenshot = Join-Path $LogDir $(if ($ToolboxMenu) { "toolbox-menu.png" } else { "right-top-menu.png" })
    Capture-Window -Hwnd $popup -Path $screenshot -MinimumHeight $(if ($ToolboxMenu) { 120 } else { 220 })
    if (!$ToolboxMenu) {
        Assert-MenuScreenshot -Path $screenshot
    }

    "menu_visual_tests=passed"
    "screenshot=$screenshot"
} finally {
    if ($process -and !$process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $runDir -Recurse -Force -ErrorAction SilentlyContinue
}


