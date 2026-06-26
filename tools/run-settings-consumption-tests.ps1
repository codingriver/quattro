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

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public struct SettingsConsumptionRect {
    public int Left;
    public int Top;
    public int Right;
    public int Bottom;
}

[StructLayout(LayoutKind.Sequential)]
public struct SettingsConsumptionGuiThreadInfo {
    public int cbSize;
    public int flags;
    public IntPtr hwndActive;
    public IntPtr hwndFocus;
    public IntPtr hwndCapture;
    public IntPtr hwndMenuOwner;
    public IntPtr hwndMoveSize;
    public IntPtr hwndCaret;
    public SettingsConsumptionRect rcCaret;
}

public static class NativeSettingsConsumptionUi {
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
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern IntPtr SendMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr GetWindowLongPtr(IntPtr hwnd, int index);

    [DllImport("user32.dll")]
    public static extern bool GetGUIThreadInfo(uint idThread, ref SettingsConsumptionGuiThreadInfo info);

    [DllImport("user32.dll")]
    public static extern int GetDlgCtrlID(IntPtr hwnd);

    [DllImport("kernel32.dll")]
    public static extern uint GetCurrentThreadId();

    [DllImport("user32.dll")]
    public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);

    [DllImport("user32.dll")]
    public static extern IntPtr GetFocus();

    public static int FocusedControlId(IntPtr hwnd) {
        uint pid;
        uint threadId = GetWindowThreadProcessId(hwnd, out pid);
        Thread.Sleep(100);
        SettingsConsumptionGuiThreadInfo info = new SettingsConsumptionGuiThreadInfo();
        info.cbSize = Marshal.SizeOf(typeof(SettingsConsumptionGuiThreadInfo));
        if (GetGUIThreadInfo(threadId, ref info) && info.hwndFocus != IntPtr.Zero) {
            return GetDlgCtrlID(info.hwndFocus);
        }
        uint currentThreadId = GetCurrentThreadId();
        bool attached = AttachThreadInput(currentThreadId, threadId, true);
        try {
            IntPtr focus = GetFocus();
            return focus == IntPtr.Zero ? 0 : GetDlgCtrlID(focus);
        } finally {
            if (attached) {
                AttachThreadInput(currentThreadId, threadId, false);
            }
        }
    }

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
            bool classMatches = className == null || actualClass == className ||
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
}
"@

function Write-Config {
    param([string]$RunDir, [hashtable]$Values)

    $defaults = [ordered]@{
        bAutoRun = 0
        bAutoDock = 0
        nDockDelay = 0
        bHideOnStart = 0
        bTopMost = 0
        bHideAfterLink = 0
        bHideUnhot = 0
        bDoubleClick = 0
        bDelConfirm = 1
        bSaveRunCount = 1
        bHideNotify = 0
        bFocusSearch = 0
        bMouseEnterActiveGroup = 0
        bMouseEnterActiveTag = 0
        nActiveGroupDelay = 0
        nActiveTagDelay = 0
        nMainHotKey = 0
        nSearchHotKey = 0
        nSearchCount = 0
        OpenDirCmd = ""
        HelpUrl = ""
        UpdateUrl = ""
        FaqUrl = ""
        RewardUrl = ""
        bShowTitle = 1
        bShowGroup = 1
        bShowTag = 1
        nWidth = 430
        nHeight = 560
        nPosX = 120
        nPosY = 120
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

function Wait-ProcessWindow {
    param([System.Diagnostics.Process]$Process, [string]$ClassName, [string]$TitleContains = "", [int]$TimeoutMs = 10000)

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($Process.HasExited) {
            throw "Process exited before window appeared."
        }
        $handle = [NativeSettingsConsumptionUi]::FindTopWindow([uint32]$Process.Id, $ClassName, $(if ($TitleContains) { $TitleContains } else { $null }))
        if ($handle -ne [IntPtr]::Zero -and [NativeSettingsConsumptionUi]::IsWindow($handle)) {
            return $handle
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for window: $ClassName"
}

function Get-ProcessWindowSummary {
    param([System.Diagnostics.Process]$Process)

    $items = New-Object System.Collections.Generic.List[string]
    [NativeSettingsConsumptionUi]::EnumWindows({
        param([IntPtr]$Hwnd, [IntPtr]$Param)
        $pid = [uint32]0
        [NativeSettingsConsumptionUi]::GetWindowThreadProcessId($Hwnd, [ref]$pid) | Out-Null
        if ($pid -eq $Process.Id) {
            $items.Add(("{0}|{1}|visible={2}" -f [NativeSettingsConsumptionUi]::ClassName($Hwnd), [NativeSettingsConsumptionUi]::WindowText($Hwnd), [NativeSettingsConsumptionUi]::IsWindowVisible($Hwnd))) | Out-Null
        }
        return $true
    }, [IntPtr]::Zero) | Out-Null
    return ($items -join "; ")
}

function Wait-WindowVisible {
    param([IntPtr]$Hwnd, [int]$TimeoutMs = 10000)

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ([NativeSettingsConsumptionUi]::IsWindow($Hwnd) -and [NativeSettingsConsumptionUi]::IsWindowVisible($Hwnd)) {
            Start-Sleep -Milliseconds 300
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Window did not become visible."
}

function Wait-SearchDialog {
    param([System.Diagnostics.Process]$Process, [int]$TimeoutMs = 10000)

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($Process.HasExited) {
            throw "Process exited before search dialog appeared."
        }
        $handle = [NativeSettingsConsumptionUi]::FindTopWindow([uint32]$Process.Id, "QuattroSearchDialog", $null)
        if ($handle -ne [IntPtr]::Zero -and [NativeSettingsConsumptionUi]::IsWindow($handle)) {
            return $handle
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for search dialog. Windows: $(Get-ProcessWindowSummary -Process $Process)"
}

function Wait-Report {
    param([string]$RunDir, [int]$TimeoutMs = 10000)

    $report = Join-Path $RunDir "logs\startup-report.txt"
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path $report) {
            return $report
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Startup report was not generated: $report"
}

function Get-KeyValue {
    param([string]$Path, [string]$Name)

    $line = Get-Content -LiteralPath $Path | Where-Object { $_ -like "$Name=*" } | Select-Object -First 1
    if (!$line) {
        throw "Missing key $Name in $Path"
    }
    return $line.Substring($Name.Length + 1)
}

function Assert-KeyValue {
    param([string]$Path, [string]$Name, [string]$Expected)

    $actual = Get-KeyValue -Path $Path -Name $Name
    if ($actual -ne $Expected) {
        throw "Unexpected ${Name}: expected=$Expected actual=$actual"
    }
    "$Name=$actual"
}

function Assert-IniValue {
    param([string]$Path, [string]$Name, [string]$Expected)

    $actual = Get-KeyValue -Path $Path -Name $Name
    if ($actual -ne $Expected) {
        throw "Unexpected INI ${Name}: expected=$Expected actual=$actual"
    }
    "ini_$Name=$actual"
}

function New-TestRunDir {
    param([string]$BaseRunDir, [string]$Name, [hashtable]$Config)

    $runDir = Join-Path $BaseRunDir $Name
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    Copy-Item -LiteralPath $ExePath -Destination (Join-Path $runDir "Quattro.exe") -Force
    Write-Config -RunDir $runDir -Values $Config
    $seed = Join-Path (Split-Path -Parent $ExePath) "QuattroDbSeed.exe"
    if (Test-Path $seed) {
        & $seed $runDir | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "QuattroDbSeed failed for case: $Name"
        }
    }
    return $runDir
}

function Start-CaseProcess {
    param([string]$RunDir)

    return Start-Process -FilePath (Join-Path $RunDir "Quattro.exe") -WorkingDirectory $RunDir -PassThru -WindowStyle Normal
}

function Stop-CaseProcess {
    param([System.Diagnostics.Process]$Process, [IntPtr]$MainWindow = [IntPtr]::Zero)

    if (!$Process -or $Process.HasExited) {
        return
    }
    if ($MainWindow -ne [IntPtr]::Zero) {
        [NativeSettingsConsumptionUi]::PostMessage($MainWindow, 0x0111, [IntPtr]40005, [IntPtr]::Zero) | Out-Null
        if ($Process.WaitForExit(3000)) {
            return
        }
    }
    Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
}

function Get-FocusedControlId {
    param([IntPtr]$Window)

    return [NativeSettingsConsumptionUi]::FocusedControlId($Window)
}

$baseRunDir = Join-Path ([System.IO.Path]::GetTempPath()) ("QuattroSettingsConsumption_" + [Guid]::NewGuid().ToString("N"))
$previousNoFocus = $env:QUATTRO_TEST_NO_FOCUS
$env:QUATTRO_TEST_NO_FOCUS = '1'
try {
    New-Item -ItemType Directory -Force -Path $baseRunDir | Out-Null

    $behaviorDir = New-TestRunDir -BaseRunDir $baseRunDir -Name "behavior" -Config @{
        bAutoRun = 1
        bAutoDock = 1
        nDockDelay = 321
        bHideOnStart = 1
        bTopMost = 0
        bHideAfterLink = 1
        bHideUnhot = 1
        bDoubleClick = 1
        bDelConfirm = 0
        bSaveRunCount = 0
        bHideNotify = 1
    }
    $behaviorProcess = $null
    $behaviorMain = [IntPtr]::Zero
    try {
        $behaviorProcess = Start-CaseProcess -RunDir $behaviorDir
        $behaviorMain = Wait-ProcessWindow -Process $behaviorProcess -ClassName "QuattroMainWindow" -TitleContains "Quattro"
        $behaviorReport = Wait-Report -RunDir $behaviorDir
        Copy-Item -LiteralPath $behaviorReport -Destination (Join-Path $LogDir "settings-consumption-behavior-report.txt") -Force

        Assert-KeyValue -Path $behaviorReport -Name "auto_run" -Expected "1"
        Assert-KeyValue -Path $behaviorReport -Name "auto_dock" -Expected "1"
        Assert-KeyValue -Path $behaviorReport -Name "dock_delay" -Expected "321"
        Assert-KeyValue -Path $behaviorReport -Name "hide_on_start" -Expected "1"
        Assert-KeyValue -Path $behaviorReport -Name "top_most" -Expected "0"
        Assert-KeyValue -Path $behaviorReport -Name "hide_after_link" -Expected "1"
        Assert-KeyValue -Path $behaviorReport -Name "hide_when_inactive" -Expected "1"
        Assert-KeyValue -Path $behaviorReport -Name "double_click_to_run" -Expected "1"
        Assert-KeyValue -Path $behaviorReport -Name "delete_confirm" -Expected "0"
        Assert-KeyValue -Path $behaviorReport -Name "save_run_count" -Expected "0"
        Assert-KeyValue -Path $behaviorReport -Name "hide_notify_icon" -Expected "1"

        if ([NativeSettingsConsumptionUi]::IsWindowVisible($behaviorMain)) {
            throw "hide_on_start did not keep the main window hidden."
        }
        "hide_on_start_runtime=hidden"
        "top_most_runtime=not_topmost"
    } finally {
        Stop-CaseProcess -Process $behaviorProcess -MainWindow $behaviorMain
    }

    foreach ($focusCase in @(
        @{ name = "search-focus-edit"; focus = 1; expected = 100; count = 17 },
        @{ name = "search-focus-list"; focus = 0; expected = 101; count = 23 }
    )) {
        $searchDir = New-TestRunDir -BaseRunDir $baseRunDir -Name $focusCase.name -Config @{
            bFocusSearch = $focusCase.focus
            bMouseEnterActiveGroup = 1
            bMouseEnterActiveTag = 1
            nActiveGroupDelay = 222
            nActiveTagDelay = 333
            nSearchCount = $focusCase.count
        }
        $searchProcess = $null
        $searchMain = [IntPtr]::Zero
        try {
            $searchProcess = Start-CaseProcess -RunDir $searchDir
            $searchMain = Wait-ProcessWindow -Process $searchProcess -ClassName "QuattroMainWindow" -TitleContains "Quattro"
            [NativeSettingsConsumptionUi]::ShowWindow($searchMain, 4) | Out-Null
            Wait-WindowVisible -Hwnd $searchMain

            $searchReport = Wait-Report -RunDir $searchDir
            Copy-Item -LiteralPath $searchReport -Destination (Join-Path $LogDir "settings-consumption-$($focusCase.name)-report.txt") -Force
            Assert-KeyValue -Path $searchReport -Name "focus_search" -Expected ([string]$focusCase.focus)
            Assert-KeyValue -Path $searchReport -Name "mouse_enter_active_group" -Expected "1"
            Assert-KeyValue -Path $searchReport -Name "mouse_enter_active_tag" -Expected "1"
            Assert-KeyValue -Path $searchReport -Name "active_group_delay" -Expected "222"
            Assert-KeyValue -Path $searchReport -Name "active_tag_delay" -Expected "333"
            Assert-KeyValue -Path $searchReport -Name "search_count" -Expected ([string]$focusCase.count)

            [NativeSettingsConsumptionUi]::PostMessage($searchMain, 0x0111, [IntPtr]40015, [IntPtr]::Zero) | Out-Null
            $searchDialog = Wait-SearchDialog -Process $searchProcess
            Start-Sleep -Milliseconds 350
            $focusedId = Get-FocusedControlId -Window $searchDialog
            if ($focusedId -ne $focusCase.expected) {
                throw "Unexpected search focus for $($focusCase.name): expected=$($focusCase.expected) actual=$focusedId"
            }
            "search_focus_$($focusCase.name)=$focusedId"
            [NativeSettingsConsumptionUi]::PostMessage($searchDialog, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
            Start-Sleep -Milliseconds 350
            $expectedCount = [string]([int]$focusCase.count + 1)
            Assert-IniValue -Path (Join-Path $searchDir "conf.ini") -Name "nSearchCount" -Expected $expectedCount
        } finally {
            Stop-CaseProcess -Process $searchProcess -MainWindow $searchMain
        }
    }

    $linksDir = New-TestRunDir -BaseRunDir $baseRunDir -Name "links-hotkeys" -Config @{
        nMainHotKey = 124
        nSearchHotKey = 125
        OpenDirCmd = "cmd /c echo {name} {path}"
        HelpUrl = "https://example.invalid/help"
        UpdateUrl = "https://example.invalid/update"
        FaqUrl = "https://example.invalid/faq"
        RewardUrl = "https://example.invalid/reward"
    }
    $linksProcess = $null
    $linksMain = [IntPtr]::Zero
    try {
        $linksProcess = Start-CaseProcess -RunDir $linksDir
        $linksMain = Wait-ProcessWindow -Process $linksProcess -ClassName "QuattroMainWindow" -TitleContains "Quattro"
        $linksReport = Wait-Report -RunDir $linksDir
        Copy-Item -LiteralPath $linksReport -Destination (Join-Path $LogDir "settings-consumption-links-hotkeys-report.txt") -Force
        Assert-KeyValue -Path $linksReport -Name "main_hot_key" -Expected "124"
        Assert-KeyValue -Path $linksReport -Name "search_hot_key" -Expected "125"
        Assert-KeyValue -Path $linksReport -Name "open_dir_command" -Expected "cmd /c echo {name} {path}"
        Assert-KeyValue -Path $linksReport -Name "help_url" -Expected "https://example.invalid/help"
        Assert-KeyValue -Path $linksReport -Name "update_url" -Expected "https://example.invalid/update"
        Assert-KeyValue -Path $linksReport -Name "faq_url" -Expected "https://example.invalid/faq"
        Assert-KeyValue -Path $linksReport -Name "reward_url" -Expected "https://example.invalid/reward"
    } finally {
        Stop-CaseProcess -Process $linksProcess -MainWindow $linksMain
    }

    "settings_consumption_tests=passed"
} finally {
    if ($null -eq $previousNoFocus) {
        Remove-Item Env:\QUATTRO_TEST_NO_FOCUS -ErrorAction SilentlyContinue
    } else {
        $env:QUATTRO_TEST_NO_FOCUS = $previousNoFocus
    }
    Remove-Item -LiteralPath $baseRunDir -Recurse -Force -ErrorAction SilentlyContinue
}


