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

public struct DisplayTestRect {
    public int Left;
    public int Top;
    public int Right;
    public int Bottom;
}

public static class NativeDisplaySettingsUi {
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
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdcBlt, uint nFlags);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hwnd, out DisplayTestRect rect);

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
    param([System.Diagnostics.Process]$Process, [string]$ClassName, [string]$TitleContains = "Quattro", [int]$TimeoutMs = 10000)

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($Process.HasExited) {
            throw "Process exited before window appeared."
        }
        $handle = [NativeDisplaySettingsUi]::FindTopWindow([uint32]$Process.Id, $ClassName, $TitleContains)
        if ($handle -ne [IntPtr]::Zero -and [NativeDisplaySettingsUi]::IsWindow($handle)) {
            return $handle
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for window: $ClassName"
}

function Wait-WindowVisible {
    param([IntPtr]$Hwnd, [int]$TimeoutMs = 10000)

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ([NativeDisplaySettingsUi]::IsWindow($Hwnd) -and [NativeDisplaySettingsUi]::IsWindowVisible($Hwnd)) {
            Start-Sleep -Milliseconds 350
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Window did not become visible."
}

function Capture-Window {
    param([IntPtr]$Hwnd, [string]$Path)

    [DisplayTestRect]$rect = New-Object DisplayTestRect
    if (![NativeDisplaySettingsUi]::GetWindowRect($Hwnd, [ref]$rect)) {
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
        [NativeDisplaySettingsUi]::PrintWindow($Hwnd, $hdc, 2) | Out-Null
    } finally {
        $graphics.ReleaseHdc($hdc)
        $graphics.Dispose()
    }
    $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
}

function Assert-UsefulImage {
    param([string]$Path, [string]$Name)

    if (!(Test-Path $Path)) {
        throw "Missing screenshot for ${Name}: $Path"
    }
    $bitmap = [System.Drawing.Bitmap]::FromFile($Path)
    try {
        if ($bitmap.Width -lt 320 -or $bitmap.Height -lt 320) {
            throw "Screenshot too small for ${Name}: $($bitmap.Width)x$($bitmap.Height)"
        }
        $colors = New-Object 'System.Collections.Generic.HashSet[int]'
        for ($y = 0; $y -lt $bitmap.Height; $y += 12) {
            for ($x = 0; $x -lt $bitmap.Width; $x += 12) {
                [void]$colors.Add($bitmap.GetPixel($x, $y).ToArgb())
            }
        }
        if ($colors.Count -lt 16) {
            throw "Screenshot lacks visual variation for ${Name}: colors=$($colors.Count)"
        }
    } finally {
        $bitmap.Dispose()
    }
}

function Get-ChangedSamples {
    param(
        [string]$LeftPath,
        [string]$RightPath,
        [int]$X,
        [int]$Y,
        [int]$Width,
        [int]$Height,
        [int]$Step = 3,
        [int]$Threshold = 20
    )

    $left = [System.Drawing.Bitmap]::FromFile($LeftPath)
    $right = [System.Drawing.Bitmap]::FromFile($RightPath)
    try {
        $maxX = [Math]::Min($left.Width, $right.Width)
        $maxY = [Math]::Min($left.Height, $right.Height)
        $endX = [Math]::Min($X + $Width, $maxX)
        $endY = [Math]::Min($Y + $Height, $maxY)
        $changed = 0
        for ($py = [Math]::Max(0, $Y); $py -lt $endY; $py += $Step) {
            for ($px = [Math]::Max(0, $X); $px -lt $endX; $px += $Step) {
                $a = $left.GetPixel($px, $py)
                $b = $right.GetPixel($px, $py)
                $delta = [Math]::Abs([int]$a.R - [int]$b.R) + [Math]::Abs([int]$a.G - [int]$b.G) + [Math]::Abs([int]$a.B - [int]$b.B)
                if ($delta -ge $Threshold) {
                    ++$changed
                }
            }
        }
        return $changed
    } finally {
        $left.Dispose()
        $right.Dispose()
    }
}

function Assert-RegionChanged {
    param(
        [string]$LeftPath,
        [string]$RightPath,
        [string]$Name,
        [int]$X,
        [int]$Y,
        [int]$Width,
        [int]$Height,
        [int]$MinChanged
    )

    $changed = Get-ChangedSamples -LeftPath $LeftPath -RightPath $RightPath -X $X -Y $Y -Width $Width -Height $Height
    if ($changed -lt $MinChanged) {
        throw "Display setting did not produce enough runtime visual change for ${Name}: changed=$changed min=$MinChanged"
    }
    "${Name}_changed_samples=$changed"
}

function Wait-ReportPath {
    param([string]$Path, [int]$TimeoutMs = 10000)

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path $Path) {
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Startup report was not generated: $Path"
}

function Get-ReportValue {
    param([string]$CaseName, [string]$Name)

    $report = Join-Path $LogDir "display-settings-report-$CaseName.txt"
    if (!(Test-Path $report)) {
        throw "Missing display settings report for case: $CaseName"
    }
    $line = Get-Content -LiteralPath $report | Where-Object { $_ -like "$Name=*" } | Select-Object -First 1
    if (!$line) {
        throw "Missing report field ${Name} for case: $CaseName"
    }
    return $line.Substring($Name.Length + 1)
}

function Assert-ReportValue {
    param([string]$CaseName, [string]$Name, [string]$Expected)

    $actual = Get-ReportValue -CaseName $CaseName -Name $Name
    if ($actual -ne $Expected) {
        throw "Unexpected runtime display setting for ${CaseName}.${Name}: expected=$Expected actual=$actual"
    }
    "${CaseName}_${Name}=$actual"
}

function Write-Config {
    param([string]$RunDir, [hashtable]$Values)

    $defaults = [ordered]@{
        bAutoDock = 0
        bHideUnhot = 0
        bTopMost = 0
        bHideOnStart = 0
        bHideNotify = 0
        bShowTitle = 1
        bShowGroup = 1
        bShowTag = 1
        bRunCount = 1
        bShowDate = 1
        bShowBtnSearch = 1
        bShowBtnMenu = 1
        bShowBtnSkin = 1
        bLnkNameSingleline = 1
        bShowTooltip = 1
        bGroupRight = 0
        bTagRight = 0
        nGroupWidth = 104
        nTagWidth = 124
        TagAlign = "center"
        nAlpha = 255
        nWidth = 430
        nHeight = 560
        nPosX = 90
        nPosY = 90
        Theme = "default"
    }
    foreach ($key in $Values.Keys) {
        $defaults[$key] = $Values[$key]
    }
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("[main]") | Out-Null
    foreach ($key in $defaults.Keys) {
        $lines.Add("$key=$($defaults[$key])") | Out-Null
    }
    $lines | Set-Content -LiteralPath (Join-Path $RunDir "conf.ini") -Encoding Unicode
}

function Capture-Case {
    param([string]$BaseRunDir, [string]$Name, [hashtable]$Config, [bool]$AllowBlank = $false)

    $RunDir = Join-Path $BaseRunDir $Name
    New-Item -ItemType Directory -Force -Path $RunDir | Out-Null
    Copy-Item -LiteralPath $ExePath -Destination (Join-Path $RunDir "Quattro.exe") -Force
    Write-Config -RunDir $RunDir -Values $Config

    $seed = Join-Path (Split-Path -Parent $ExePath) "QuattroDbSeed.exe"
    if (!(Test-Path $seed)) {
        throw "Seed helper not found: $seed"
    }
    & $seed $RunDir | Tee-Object -FilePath (Join-Path $LogDir "display-settings-seed-$Name.txt") | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "QuattroDbSeed failed with exit code $LASTEXITCODE for case: $Name"
    }

    $process = $null
    try {
        $process = Start-Process -FilePath (Join-Path $RunDir "Quattro.exe") -WorkingDirectory $RunDir -PassThru -WindowStyle Normal
        $main = Wait-ProcessWindow -Process $process -ClassName "QuattroMainWindow"
        Wait-WindowVisible -Hwnd $main
        [NativeDisplaySettingsUi]::ShowWindow($main, 4) | Out-Null
        [NativeDisplaySettingsUi]::SetWindowPos($main, [IntPtr]::new(-1), 90, 90, 0, 0, 0x0041) | Out-Null
        Start-Sleep -Milliseconds 450
        $report = Join-Path $RunDir "logs\startup-report.txt"
        Wait-ReportPath -Path $report
        Copy-Item -LiteralPath $report -Destination (Join-Path $LogDir "display-settings-report-$Name.txt") -Force
        $path = Join-Path $LogDir "display-settings-$Name.png"
        Capture-Window -Hwnd $main -Path $path
        [NativeDisplaySettingsUi]::SetWindowPos($main, [IntPtr]::new(-2), 0, 0, 0, 0, 0x0043) | Out-Null
        if (!$AllowBlank) {
            Assert-UsefulImage -Path $path -Name $Name
        }
        [NativeDisplaySettingsUi]::PostMessage($main, 0x0111, [IntPtr]40005, [IntPtr]::Zero) | Out-Null
        if (!$process.WaitForExit(5000)) {
            throw "Process did not exit for case: $Name"
        }
        return $path
    } finally {
        if ($process -and !$process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
    }
}

$runDir = Join-Path ([System.IO.Path]::GetTempPath()) ("QuattroDisplaySettings_" + [Guid]::NewGuid().ToString("N"))
$previousNoFocus = $env:QUATTRO_TEST_NO_FOCUS
$env:QUATTRO_TEST_NO_FOCUS = '1'

try {
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null

    $all = Capture-Case -BaseRunDir $runDir -Name "all-visible" -Config @{}
    $titleHidden = Capture-Case -BaseRunDir $runDir -Name "title-hidden" -Config @{
        bShowTitle = 0
    } -AllowBlank $true
    $hiddenNav = Capture-Case -BaseRunDir $runDir -Name "hidden-nav" -Config @{
        bShowGroup = 0
        bShowTag = 0
    }
    $hiddenTitleItems = Capture-Case -BaseRunDir $runDir -Name "hidden-title-items" -Config @{
        bShowDate = 0
        bShowBtnSearch = 0
        bShowBtnMenu = 0
        bShowBtnSkin = 0
    }
    $rightNav = Capture-Case -BaseRunDir $runDir -Name "right-nav" -Config @{
        bGroupRight = 1
        bTagRight = 1
        nGroupWidth = 112
        nTagWidth = 136
    }
    $tagRightAlign = Capture-Case -BaseRunDir $runDir -Name "tag-right-align" -Config @{
        TagAlign = "right"
    }
    $noRunCount = Capture-Case -BaseRunDir $runDir -Name "no-run-count" -Config @{
        bRunCount = 0
    }
    $wrappedNames = Capture-Case -BaseRunDir $runDir -Name "wrapped-names" -Config @{
        bLnkNameSingleline = 0
    }

    Assert-ReportValue -CaseName "title-hidden" -Name "show_title" -Expected "0"
    Assert-ReportValue -CaseName "hidden-nav" -Name "show_group" -Expected "0"
    Assert-ReportValue -CaseName "hidden-nav" -Name "show_tag" -Expected "0"
    Assert-ReportValue -CaseName "hidden-title-items" -Name "show_date" -Expected "0"
    Assert-ReportValue -CaseName "hidden-title-items" -Name "show_search_button" -Expected "0"
    Assert-ReportValue -CaseName "hidden-title-items" -Name "show_menu_button" -Expected "0"
    Assert-ReportValue -CaseName "hidden-title-items" -Name "show_skin_button" -Expected "0"
    Assert-ReportValue -CaseName "right-nav" -Name "group_right" -Expected "1"
    Assert-ReportValue -CaseName "right-nav" -Name "tag_right" -Expected "1"
    Assert-ReportValue -CaseName "right-nav" -Name "group_width" -Expected "112"
    Assert-ReportValue -CaseName "right-nav" -Name "tag_width" -Expected "136"
    Assert-ReportValue -CaseName "tag-right-align" -Name "tag_align" -Expected "right"
    Assert-ReportValue -CaseName "no-run-count" -Name "show_run_count" -Expected "0"
    Assert-ReportValue -CaseName "wrapped-names" -Name "link_name_single_line" -Expected "0"
    Assert-ReportValue -CaseName "all-visible" -Name "show_tooltip" -Expected "1"

    Assert-RegionChanged -LeftPath $all -RightPath $hiddenNav -Name "group_tag_visibility" -X 0 -Y 32 -Width 430 -Height 520 -MinChanged 500
    Assert-RegionChanged -LeftPath $all -RightPath $hiddenTitleItems -Name "title_date_and_buttons_visibility" -X 130 -Y 4 -Width 290 -Height 30 -MinChanged 12
    Assert-RegionChanged -LeftPath $all -RightPath $rightNav -Name "right_side_group_and_tag_layout" -X 0 -Y 34 -Width 430 -Height 500 -MinChanged 500
    Assert-RegionChanged -LeftPath $all -RightPath $tagRightAlign -Name "tag_text_alignment" -X 0 -Y 74 -Width 130 -Height 420 -MinChanged 18
    Assert-RegionChanged -LeftPath $all -RightPath $noRunCount -Name "run_count_visibility" -X 335 -Y 72 -Width 80 -Height 220 -MinChanged 8

    "display_settings_tests=passed"
    "screenshot_all_visible=$all"
    "screenshot_title_hidden=$titleHidden"
    "screenshot_hidden_nav=$hiddenNav"
    "screenshot_hidden_title_items=$hiddenTitleItems"
    "screenshot_right_nav=$rightNav"
    "screenshot_tag_right_align=$tagRightAlign"
    "screenshot_no_run_count=$noRunCount"
    "screenshot_wrapped_names=$wrappedNames"
} finally {
    if ($null -eq $previousNoFocus) {
        Remove-Item Env:\QUATTRO_TEST_NO_FOCUS -ErrorAction SilentlyContinue
    } else {
        $env:QUATTRO_TEST_NO_FOCUS = $previousNoFocus
    }
    Remove-Item -LiteralPath $runDir -Recurse -Force -ErrorAction SilentlyContinue
}


