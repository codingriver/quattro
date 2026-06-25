param(
    [string]$ExePath = "",
    [string]$ProbePath = "",
    [string]$Configuration = "Release",
    [string]$LogDir = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $ExePath = Join-Path $root "build\$Configuration\Quattro.exe"
}
if ([string]::IsNullOrWhiteSpace($ProbePath)) {
    $ProbePath = Join-Path (Split-Path -Parent $ExePath) "QuattroDbProbe.exe"
}
if ([string]::IsNullOrWhiteSpace($LogDir)) {
    $LogDir = Join-Path (Join-Path $root "build\$Configuration") "logs"
}
if (!(Test-Path $ExePath)) {
    throw "Executable not found: $ExePath"
}
if (!(Test-Path $ProbePath)) {
    throw "Database probe not found: $ProbePath"
}
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$stepLog = Join-Path $LogDir "menu-test-steps.txt"
Remove-Item -LiteralPath $stepLog -Force -ErrorAction SilentlyContinue

function Write-Step {
    param([string]$Name)
    $line = "$((Get-Date).ToString('HH:mm:ss.fff')) $Name"
    $line | Add-Content -Path $stepLog -Encoding UTF8
    Write-Output $line
}

if (-not ([System.Management.Automation.PSTypeName]'NativeMenuUi').Type) {
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public struct MenuRect {
    public int Left;
    public int Top;
    public int Right;
    public int Bottom;
}

public static class NativeMenuUi {
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

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowEx(IntPtr hwndParent, IntPtr hwndChildAfter, string lpszClass, string lpszWindow);

    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr hDlg, int nIDDlgItem);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool SetWindowText(IntPtr hWnd, string lpString);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern IntPtr SendMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessage")]
    public static extern IntPtr SendMessageString(IntPtr hWnd, uint Msg, IntPtr wParam, string lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessage")]
    public static extern IntPtr SendMessageText(IntPtr hWnd, uint Msg, IntPtr wParam, StringBuilder lParam);

    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

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

    public static void SetControlText(IntPtr hwnd, string value) {
        SendMessageString(hwnd, 0x000C, IntPtr.Zero, value);
    }

    public static string ControlText(IntPtr hwnd) {
        int length = (int)SendMessage(hwnd, 0x000E, IntPtr.Zero, IntPtr.Zero);
        var text = new StringBuilder(length + 1);
        SendMessageText(hwnd, 0x000D, (IntPtr)text.Capacity, text);
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

    public static string ChildWindowSummary(IntPtr parent) {
        var output = new StringBuilder();
        int index = 0;
        EnumChildWindows(parent, (hWnd, lParam) => {
            output.Append(index++);
            output.Append("|");
            output.Append(ClassName(hWnd));
            output.Append("|");
            output.Append(ControlText(hWnd));
            output.Append("\r\n");
            return true;
        }, IntPtr.Zero);
        return output.ToString();
    }

    public static IntPtr FindChildByClass(IntPtr parent, string className) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, (hWnd, lParam) => {
            if (ClassName(hWnd) == className) {
                found = hWnd;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
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
        $handle = [NativeMenuUi]::FindTopWindow([uint32]$Process.Id, $ClassName, $(if ($TitleContains) { $TitleContains } else { $null }))
        if ($handle -ne [IntPtr]::Zero -and [NativeMenuUi]::IsWindow($handle)) {
            return $handle
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for window: $ClassName / $TitleContains"
}

function Wait-WindowClosed {
    param([IntPtr]$Hwnd, [int]$TimeoutMs = 5000)

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (![NativeMenuUi]::IsWindow($Hwnd)) {
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Window did not close."
}

function Wait-ChildWindow {
    param(
        [IntPtr]$Parent,
        [string]$ClassName,
        [int]$ControlId = 0,
        [int]$TimeoutMs = 5000
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (![NativeMenuUi]::IsWindow($Parent)) {
            throw "Parent window closed before child appeared: $ClassName"
        }
        if ($ControlId -ne 0) {
            $handle = [NativeMenuUi]::GetDlgItem($Parent, $ControlId)
        } else {
            $handle = [NativeMenuUi]::FindChildByClass($Parent, $ClassName)
        }
        if ($handle -ne [IntPtr]::Zero -and [NativeMenuUi]::IsWindow($handle)) {
            return $handle
        }
        Start-Sleep -Milliseconds 50
    }
    $summary = [NativeMenuUi]::ChildWindowSummary($Parent)
    if (![string]::IsNullOrWhiteSpace($LogDir)) {
        $summary | Set-Content -Path (Join-Path $LogDir "child-window-timeout.txt") -Encoding UTF8
    }
    throw "Timed out waiting for child window: $ClassName / $ControlId"
}

function Invoke-CommandImmediate {
    param([IntPtr]$MainWindow, [int]$Command)
    Write-Step "command $Command"
    [NativeMenuUi]::SendMessage($MainWindow, 0x0111, [IntPtr]$Command, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 120
}

function Invoke-TextCommand {
    param(
        [System.Diagnostics.Process]$Process,
        [IntPtr]$MainWindow,
        [int]$Command,
        [string]$DialogTitle,
        [string]$Text
    )

    Write-Step "text command $Command"
    [NativeMenuUi]::PostMessage($MainWindow, 0x0111, [IntPtr]$Command, [IntPtr]::Zero) | Out-Null
    $dialog = Wait-ProcessWindow -Process $Process -ClassName "QuattroTextInputDialog_*" -TitleContains $DialogTitle
    $edit = Wait-ChildWindow -Parent $dialog -ClassName "Edit"
    if ($edit -eq [IntPtr]::Zero) {
        throw "Text edit control not found: $DialogTitle"
    }
    [NativeMenuUi]::SetControlText($edit, $Text)
    [NativeMenuUi]::PostMessage($dialog, 0x0111, [IntPtr]1, [IntPtr]::Zero) | Out-Null
    Wait-WindowClosed -Hwnd $dialog
}

function Invoke-LinkDialogCommand {
    param(
        [System.Diagnostics.Process]$Process,
        [IntPtr]$MainWindow,
        [int]$Command,
        [string]$DialogTitle,
        [string]$Name,
        [string]$Path,
        [string]$Remark = ""
    )

    Write-Step "link dialog command $Command"
    [NativeMenuUi]::PostMessage($MainWindow, 0x0111, [IntPtr]$Command, [IntPtr]::Zero) | Out-Null
    $dialog = Wait-ProcessWindow -Process $Process -ClassName "QuattroLinkEditDialog" -TitleContains $DialogTitle
    $nameEdit = Wait-ChildWindow -Parent $dialog -ClassName "Edit" -ControlId 1003
    $pathEdit = Wait-ChildWindow -Parent $dialog -ClassName "Edit" -ControlId 1004
    $remarkEdit = Wait-ChildWindow -Parent $dialog -ClassName "Edit" -ControlId 1014
    if ($nameEdit -eq [IntPtr]::Zero -or $pathEdit -eq [IntPtr]::Zero) {
        throw "Link dialog controls not found: $DialogTitle"
    }
    [NativeMenuUi]::SetControlText($nameEdit, $Name)
    [NativeMenuUi]::SetControlText($pathEdit, $Path)
    if ($remarkEdit -ne [IntPtr]::Zero) {
        [NativeMenuUi]::SetControlText($remarkEdit, $Remark)
    }
    [NativeMenuUi]::PostMessage($dialog, 0x0111, [IntPtr]1018, [IntPtr]::Zero) | Out-Null
    Wait-WindowClosed -Hwnd $dialog
}

function Invoke-UrlDialogCommand {
    param(
        [System.Diagnostics.Process]$Process,
        [IntPtr]$MainWindow,
        [int]$Command,
        [string]$Name,
        [string]$Url,
        [string]$Remark = ""
    )

    Write-Step "url dialog command $Command"
    [NativeMenuUi]::PostMessage($MainWindow, 0x0111, [IntPtr]$Command, [IntPtr]::Zero) | Out-Null
    $dialog = Wait-ProcessWindow -Process $Process -ClassName "QuattroUrlEditDialog" -TitleContains ""
    $nameEdit = Wait-ChildWindow -Parent $dialog -ClassName "Edit" -ControlId 1001
    $urlEdit = Wait-ChildWindow -Parent $dialog -ClassName "Edit" -ControlId 1002
    $remarkEdit = Wait-ChildWindow -Parent $dialog -ClassName "Edit" -ControlId 1003
    [NativeMenuUi]::SetControlText($nameEdit, $Name)
    [NativeMenuUi]::SetControlText($urlEdit, $Url)
    [NativeMenuUi]::SetControlText($remarkEdit, $Remark)
    [NativeMenuUi]::PostMessage($dialog, 0x0111, [IntPtr]1004, [IntPtr]::Zero) | Out-Null
    Wait-WindowClosed -Hwnd $dialog
}

function Invoke-SystemFunctionDialogCommand {
    param(
        [System.Diagnostics.Process]$Process,
        [IntPtr]$MainWindow,
        [int]$Command
    )

    Write-Step "system function command $Command"
    [NativeMenuUi]::PostMessage($MainWindow, 0x0111, [IntPtr]$Command, [IntPtr]::Zero) | Out-Null
    $dialog = Wait-ProcessWindow -Process $Process -ClassName "QuattroSystemFunctionDialog" -TitleContains ""
    [NativeMenuUi]::PostMessage($dialog, 0x0111, [IntPtr]1002, [IntPtr]::Zero) | Out-Null
    Wait-WindowClosed -Hwnd $dialog
}

function Invoke-MessageCommand {
    param(
        [System.Diagnostics.Process]$Process,
        [IntPtr]$MainWindow,
        [int]$Command,
        [string]$Title,
        [int]$Response = 1
    )

    Write-Step "message command $Command"
    [NativeMenuUi]::PostMessage($MainWindow, 0x0111, [IntPtr]$Command, [IntPtr]::Zero) | Out-Null
    $dialog = Wait-ProcessWindow -Process $Process -ClassName "#32770" -TitleContains $Title
    if ($Response -eq 1) {
        [NativeMenuUi]::PostMessage($dialog, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    } else {
        [NativeMenuUi]::PostMessage($dialog, 0x0111, [IntPtr]$Response, [IntPtr]::Zero) | Out-Null
    }
    Wait-WindowClosed -Hwnd $dialog
}

function Close-DialogCommand {
    param(
        [System.Diagnostics.Process]$Process,
        [IntPtr]$MainWindow,
        [int]$Command,
        [string]$ClassName,
        [string]$Title
    )

    Write-Step "close dialog command $Command"
    [NativeMenuUi]::PostMessage($MainWindow, 0x0111, [IntPtr]$Command, [IntPtr]::Zero) | Out-Null
    $dialog = Wait-ProcessWindow -Process $Process -ClassName $ClassName -TitleContains $Title
    [NativeMenuUi]::PostMessage($dialog, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Wait-WindowClosed -Hwnd $dialog
}

function Read-Probe {
    param([string]$Directory)
    $output = & $ProbePath $Directory
    if ($LASTEXITCODE -ne 0) {
        $output | Out-String | Write-Host
        throw "Database probe failed."
    }
    return @($output)
}

function Assert-ProbeContains {
    param([string[]]$Lines, [string]$Pattern, [string]$Name)
    if (!($Lines | Where-Object { $_ -match $Pattern } | Select-Object -First 1)) {
        $Lines | Set-Content -Path (Join-Path $LogDir "menu-test-probe-failed.txt") -Encoding UTF8
        throw "Probe assertion failed: $Name"
    }
}

$runDir = Join-Path ([System.IO.Path]::GetTempPath()) ("QuattroMenuTests_" + [Guid]::NewGuid().ToString("N"))
$process = $null

try {
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    Copy-Item -LiteralPath $ExePath -Destination (Join-Path $runDir "Quattro.exe") -Force
    Copy-Item -LiteralPath $ProbePath -Destination (Join-Path $runDir "QuattroDbProbe.exe") -Force

    $targetA = Join-Path $runDir "target-a.txt"
    $targetB = Join-Path $runDir "target-b.txt"
    "A" | Set-Content -Path $targetA -Encoding UTF8
    "B" | Set-Content -Path $targetB -Encoding UTF8

    $process = Start-Process -FilePath (Join-Path $runDir "Quattro.exe") -WorkingDirectory $runDir -PassThru -WindowStyle Normal
    $main = Wait-ProcessWindow -Process $process -ClassName "QuattroMainWindow" -TitleContains "Quattro"

    Invoke-CommandImmediate -MainWindow $main -Command 40009
    Invoke-TextCommand -Process $process -MainWindow $main -Command 40010 -DialogTitle "" -Text "MenuGroup"
    Invoke-CommandImmediate -MainWindow $main -Command 40012
    Invoke-TextCommand -Process $process -MainWindow $main -Command 40013 -DialogTitle "" -Text "MenuTag"

    Invoke-LinkDialogCommand -Process $process -MainWindow $main -Command 40001 -DialogTitle "" -Name "MenuLink" -Path $targetA -Remark "InitialRemark"
    Invoke-LinkDialogCommand -Process $process -MainWindow $main -Command 40002 -DialogTitle "" -Name "EditedLink" -Path $targetA -Remark "EditedRemark"
    Invoke-CommandImmediate -MainWindow $main -Command 40023
    Invoke-CommandImmediate -MainWindow $main -Command 40025
    Invoke-CommandImmediate -MainWindow $main -Command 40008
    Invoke-MessageCommand -Process $process -MainWindow $main -Command 40003 -Title "" -Response 6

    Invoke-LinkDialogCommand -Process $process -MainWindow $main -Command 40001 -DialogTitle "" -Name "SecondLink" -Path $targetB
    Invoke-CommandImmediate -MainWindow $main -Command 40021
    Invoke-MessageCommand -Process $process -MainWindow $main -Command 40003 -Title "" -Response 6

    Invoke-CommandImmediate -MainWindow $main -Command 40020
    Invoke-CommandImmediate -MainWindow $main -Command 44001
    Invoke-CommandImmediate -MainWindow $main -Command 44004

    Invoke-CommandImmediate -MainWindow $main -Command 40012
    Invoke-MessageCommand -Process $process -MainWindow $main -Command 40014 -Title "" -Response 6
    Invoke-CommandImmediate -MainWindow $main -Command 40009
    Invoke-MessageCommand -Process $process -MainWindow $main -Command 40011 -Title "" -Response 6

    Invoke-MessageCommand -Process $process -MainWindow $main -Command 40026 -Title ""
    Invoke-MessageCommand -Process $process -MainWindow $main -Command 40027 -Title ""
    Invoke-MessageCommand -Process $process -MainWindow $main -Command 40029 -Title ""
    Invoke-MessageCommand -Process $process -MainWindow $main -Command 40032 -Title ""
    Invoke-MessageCommand -Process $process -MainWindow $main -Command 40033 -Title ""

    Remove-Item -LiteralPath (Join-Path $runDir "docs") -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $runDir "README.md") -Force -ErrorAction SilentlyContinue
    Invoke-MessageCommand -Process $process -MainWindow $main -Command 40028 -Title ""

    Close-DialogCommand -Process $process -MainWindow $main -Command 40015 -ClassName "QuattroSearchDialog" -Title ""
    Close-DialogCommand -Process $process -MainWindow $main -Command 40016 -ClassName "QuattroSettingsDialog" -Title ""

    Invoke-CommandImmediate -MainWindow $main -Command 43001
    Invoke-CommandImmediate -MainWindow $main -Command 43000
    Invoke-UrlDialogCommand -Process $process -MainWindow $main -Command 40036 -Name "MenuUrl" -Url "www.example.com" -Remark "UrlRemark"
    Invoke-SystemFunctionDialogCommand -Process $process -MainWindow $main -Command 40037

    [NativeMenuUi]::PostMessage($main, 0x0111, [IntPtr]40005, [IntPtr]::Zero) | Out-Null
    if (!$process.WaitForExit(5000)) {
        throw "Process did not exit."
    }

    $probeLines = Read-Probe -Directory $runDir
    $probeLines | Set-Content -Path (Join-Path $LogDir "menu-test-probe.txt") -Encoding UTF8
    Assert-ProbeContains -Lines $probeLines -Pattern "GROUP.*parent=0.*name=MenuGroup" -Name "edited group persisted"
    Assert-ProbeContains -Lines $probeLines -Pattern "GROUP.*parent=[1-9].*sort=2.*layout=1.*iconSize=48.*name=MenuTag" -Name "edited tag settings persisted"
    Assert-ProbeContains -Lines $probeLines -Pattern "LINK.*name=EditedLink.*path=.*target-a.txt.*remark=EditedRemark" -Name "edited link persisted"
    Assert-ProbeContains -Lines $probeLines -Pattern "LINK.*type=2.*name=MenuUrl.*path=https://www.example.com.*remark=UrlRemark" -Name "url link persisted"
    Assert-ProbeContains -Lines $probeLines -Pattern "LINK.*type=3.*path=::\{20D04FE0-3AEA-1069-A2D8-08002B30309D\}" -Name "system function persisted"

    $probeReport = Join-Path $LogDir "menu-test-probe.txt"
    Write-Output 'menu_tests=passed'
    Write-Output ('probe=' + $probeReport)
} finally {
    if (![string]::IsNullOrWhiteSpace($runDir)) {
        $runDir | Set-Content -Path (Join-Path $LogDir "menu-test-run-dir.txt") -Encoding UTF8
        $appLog = Join-Path $runDir "logs\app.log"
        if (Test-Path $appLog) {
            Copy-Item -LiteralPath $appLog -Destination (Join-Path $LogDir "menu-test-app.log") -Force
        }
    }
    if ($process -and !$process.HasExited) {
        try {
            [NativeMenuUi]::PostMessage($process.MainWindowHandle, 0x0111, [IntPtr]40005, [IntPtr]::Zero) | Out-Null
            if (!$process.WaitForExit(2000)) {
                Stop-Process -Id $process.Id -Force
            }
        } catch {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
    }
    Remove-Item -LiteralPath $runDir -Recurse -Force -ErrorAction SilentlyContinue
}
