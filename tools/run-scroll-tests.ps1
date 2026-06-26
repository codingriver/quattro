param(
    [string]$ExePath = "",
    [string]$SeedPath = "",
    [string]$Configuration = "Release",
    [string]$LogDir = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $ExePath = Join-Path $root "build\$Configuration\Quattro.exe"
}
if ([string]::IsNullOrWhiteSpace($SeedPath)) {
    $SeedPath = Join-Path (Split-Path -Parent $ExePath) "QuattroDbSeed.exe"
}
if ([string]::IsNullOrWhiteSpace($LogDir)) {
    $LogDir = Join-Path (Join-Path $root "build\$Configuration") "logs"
}
if (!(Test-Path $ExePath)) {
    throw "Executable not found: $ExePath"
}
if (!(Test-Path $SeedPath)) {
    throw "Database seed tool not found: $SeedPath"
}
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

Add-Type -AssemblyName System.Drawing

if (-not ([System.Management.Automation.PSTypeName]'NativeScrollUi').Type) {
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public struct ScrollRect {
    public int Left;
    public int Top;
    public int Right;
    public int Bottom;
}

public struct ScrollPoint {
    public int X;
    public int Y;
}

public static class NativeScrollUi {
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
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool MoveWindow(IntPtr hWnd, int x, int y, int width, int height, bool repaint);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hwnd, out ScrollRect rect);

    [DllImport("user32.dll")]
    public static extern bool ClientToScreen(IntPtr hwnd, ref ScrollPoint point);

    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdcBlt, uint nFlags);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

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
            bool classMatches = className == null || ClassName(hWnd) == className;
            bool titleMatches = titleContains == null || WindowText(hWnd).Contains(titleContains);
            if (classMatches && titleMatches) {
                found = hWnd;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public static IntPtr MakeWheelWParam(int delta) {
        return (IntPtr)unchecked((int)((delta & 0xffff) << 16));
    }

    public static IntPtr MakeLParam(int x, int y) {
        return (IntPtr)unchecked((int)(((y & 0xffff) << 16) | (x & 0xffff)));
    }

    public static IntPtr MakeClientLParam(IntPtr hwnd, int clientX, int clientY) {
        ScrollPoint point = new ScrollPoint();
        point.X = clientX;
        point.Y = clientY;
        ClientToScreen(hwnd, ref point);
        return MakeLParam(point.X, point.Y);
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
            throw "Process exited before window appeared."
        }
        $handle = [NativeScrollUi]::FindTopWindow([uint32]$Process.Id, $ClassName, $(if ($TitleContains) { $TitleContains } else { $null }))
        if ($handle -ne [IntPtr]::Zero -and [NativeScrollUi]::IsWindow($handle)) {
            return $handle
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for window: $ClassName"
}

function Capture-Window {
    param([IntPtr]$Hwnd, [string]$Path)

    [ScrollRect]$rect = New-Object ScrollRect
    if (![NativeScrollUi]::GetWindowRect($Hwnd, [ref]$rect)) {
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
        [NativeScrollUi]::PrintWindow($Hwnd, $hdc, 2) | Out-Null
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
                $color = $bitmap.GetPixel($x, $y).ToArgb()
                [void]$colors.Add($color)
            }
        }
        if ($colors.Count -lt 4) {
            throw "Screenshot appears blank: $Name"
        }
    } finally {
        $bitmap.Dispose()
    }
}

function Assert-ImageChanged {
    param([string]$Before, [string]$After, [string]$Name, [int]$MinimumChanged = 80)

    $left = [System.Drawing.Bitmap]::FromFile($Before)
    $right = [System.Drawing.Bitmap]::FromFile($After)
    try {
        if ($left.Width -ne $right.Width -or $left.Height -ne $right.Height) {
            throw "Screenshot size changed unexpectedly: $Name"
        }
        $changed = 0
        for ($y = 0; $y -lt $left.Height; $y += 6) {
            for ($x = 0; $x -lt $left.Width; $x += 6) {
                $a = $left.GetPixel($x, $y)
                $b = $right.GetPixel($x, $y)
                $delta = [Math]::Abs($a.R - $b.R) + [Math]::Abs($a.G - $b.G) + [Math]::Abs($a.B - $b.B)
                if ($delta -gt 24) {
                    ++$changed
                }
            }
        }
        if ($changed -lt $MinimumChanged) {
            throw "Scroll did not visibly change enough for $Name. changed=$changed"
        }
        return $changed
    } finally {
        $left.Dispose()
        $right.Dispose()
    }
}

function Send-Wheel {
    param([IntPtr]$Hwnd, [int]$ClientX, [int]$ClientY, [int]$Delta = -120, [int]$Count = 7)

    $wParam = [NativeScrollUi]::MakeWheelWParam($Delta)
    $lParam = [NativeScrollUi]::MakeClientLParam($Hwnd, $ClientX, $ClientY)
    for ($i = 0; $i -lt $Count; ++$i) {
        [NativeScrollUi]::PostMessage($Hwnd, 0x020A, $wParam, $lParam) | Out-Null
        Start-Sleep -Milliseconds 80
    }
    Start-Sleep -Milliseconds 250
}

$runDir = Join-Path ([System.IO.Path]::GetTempPath()) ("QuattroScrollTests_" + [Guid]::NewGuid().ToString("N"))
$process = $null
$previousNoFocus = $env:QUATTRO_TEST_NO_FOCUS
$env:QUATTRO_TEST_NO_FOCUS = '1'

try {
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    Copy-Item -LiteralPath $ExePath -Destination (Join-Path $runDir "Quattro.exe") -Force
    Copy-Item -LiteralPath $SeedPath -Destination (Join-Path $runDir "QuattroDbSeed.exe") -Force

    $seedOutput = & (Join-Path $runDir "QuattroDbSeed.exe") $runDir
    if ($LASTEXITCODE -ne 0) {
        $seedOutput | Out-String | Write-Host
        throw "Database seed failed."
    }
    $seedOutput | Set-Content -Path (Join-Path $LogDir "scroll-test-seed.txt") -Encoding UTF8

    $process = Start-Process -FilePath (Join-Path $runDir "Quattro.exe") -WorkingDirectory $runDir -PassThru -WindowStyle Normal
    $main = Wait-ProcessWindow -Process $process -ClassName "QuattroMainWindow" -TitleContains "Quattro"
    [NativeScrollUi]::MoveWindow($main, 80, 80, 680, 460, $true) | Out-Null
    Start-Sleep -Milliseconds 600

    $before = Join-Path $LogDir "scroll-before.png"
    $groups = Join-Path $LogDir "scroll-groups.png"
    $tags = Join-Path $LogDir "scroll-tags.png"
    $links = Join-Path $LogDir "scroll-links.png"

    Capture-Window -Hwnd $main -Path $before
    Assert-UsefulImage -Path $before -Name "before"

    Send-Wheel -Hwnd $main -ClientX 620 -ClientY 64 -Delta -120 -Count 8
    Capture-Window -Hwnd $main -Path $groups
    Assert-UsefulImage -Path $groups -Name "groups"
    $groupChanged = Assert-ImageChanged -Before $before -After $groups -Name "groups" -MinimumChanged 60

    Send-Wheel -Hwnd $main -ClientX 62 -ClientY 230 -Delta -120 -Count 9
    Capture-Window -Hwnd $main -Path $tags
    Assert-UsefulImage -Path $tags -Name "tags"
    $tagChanged = Assert-ImageChanged -Before $groups -After $tags -Name "tags" -MinimumChanged 60

    Send-Wheel -Hwnd $main -ClientX 360 -ClientY 260 -Delta -120 -Count 10
    Capture-Window -Hwnd $main -Path $links
    Assert-UsefulImage -Path $links -Name "links"
    $linkChanged = Assert-ImageChanged -Before $tags -After $links -Name "links" -MinimumChanged 60

    [NativeScrollUi]::PostMessage($main, 0x0111, [IntPtr]40005, [IntPtr]::Zero) | Out-Null
    if (!$process.WaitForExit(5000)) {
        throw "Process did not exit."
    }

    "scroll_tests=passed"
    "group_changed_samples=$groupChanged"
    "tag_changed_samples=$tagChanged"
    "link_changed_samples=$linkChanged"
    "screenshots=$before;$groups;$tags;$links"
} finally {
    if ($null -eq $previousNoFocus) {
        Remove-Item Env:\QUATTRO_TEST_NO_FOCUS -ErrorAction SilentlyContinue
    } else {
        $env:QUATTRO_TEST_NO_FOCUS = $previousNoFocus
    }
    if ($process -and !$process.HasExited) {
        try {
            [NativeScrollUi]::PostMessage($main, 0x0111, [IntPtr]40005, [IntPtr]::Zero) | Out-Null
            if (!$process.WaitForExit(2000)) {
                Stop-Process -Id $process.Id -Force
            }
        } catch {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
    }
    Remove-Item -LiteralPath $runDir -Recurse -Force -ErrorAction SilentlyContinue
}


