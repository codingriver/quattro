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

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

if (-not ([System.Management.Automation.PSTypeName]'NativePluginToolUi').Type) {
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public struct PluginToolRect {
    public int Left;
    public int Top;
    public int Right;
    public int Bottom;
}

public static class NativePluginToolUi {
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
    public static extern bool SetWindowText(IntPtr hWnd, string lpString);

    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool IsWindowEnabled(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool MoveWindow(IntPtr hWnd, int x, int y, int width, int height, bool repaint);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hwnd, out PluginToolRect rect);

    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdcBlt, uint nFlags);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern IntPtr SendMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessage")]
    public static extern IntPtr SendMessageString(IntPtr hWnd, uint Msg, IntPtr wParam, string lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessage")]
    public static extern IntPtr SendMessageText(IntPtr hWnd, uint Msg, IntPtr wParam, StringBuilder lParam);

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

    public static string ControlText(IntPtr hwnd) {
        int length = (int)SendMessage(hwnd, 0x000E, IntPtr.Zero, IntPtr.Zero);
        var text = new StringBuilder(Math.Max(length + 1, 512));
        SendMessageText(hwnd, 0x000D, (IntPtr)text.Capacity, text);
        return text.ToString();
    }

    public static void SetControlText(IntPtr hwnd, string value) {
        SendMessageString(hwnd, 0x000C, IntPtr.Zero, value);
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

    public static string OutOfBoundsVisibleChildren(IntPtr parent, int padding) {
        PluginToolRect parentRect;
        if (!GetWindowRect(parent, out parentRect)) {
            return "parent-rect-unavailable";
        }
        var output = new StringBuilder();
        EnumChildWindows(parent, (hWnd, lParam) => {
            if (!IsWindowVisible(hWnd)) {
                return true;
            }
            PluginToolRect childRect;
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
                output.Append(ControlText(hWnd));
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

    public static void StartClickTopWindowButtonWhenAppears(uint pid, string className, string titleContains, int buttonId, int timeoutMs, int settleMs) {
        ThreadPool.QueueUserWorkItem(_ => {
            DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
            while (DateTime.UtcNow < deadline) {
                IntPtr dialog = FindTopWindow(pid, className, titleContains);
                if (dialog != IntPtr.Zero && IsWindow(dialog)) {
                    Thread.Sleep(Math.Max(0, settleMs));
                    IntPtr button = GetDlgItem(dialog, buttonId);
                    if (button != IntPtr.Zero && IsWindow(button)) {
                        SendMessage(button, 0x00F5, IntPtr.Zero, IntPtr.Zero);
                    }
                    if (IsWindow(dialog)) {
                        SendMessage(dialog, 0x0111, (IntPtr)buttonId, IntPtr.Zero);
                    }
                    return;
                }
                Thread.Sleep(50);
            }
        });
    }
}
'@
}

$screenshots = New-Object System.Collections.Generic.List[string]
$WM_COMMAND = 0x0111
$WM_CLOSE = 0x0010
$WM_TIMER = 0x0113
$WM_TOOL_AUTOMATION = 0x8080
$WM_TOOL_TIMER_AUTOMATION = 0x8081
$LB_GETCOUNT = 0x018B
$LB_GETTEXT = 0x0189
$LB_SETCURSEL = 0x0186
$LBN_SELCHANGE = 1
$CB_SETCURSEL = 0x014E
$BM_GETCHECK = 0x00F0
$BM_SETCHECK = 0x00F1
$BM_CLICK = 0x00F5
$BST_UNCHECKED = 0
$IDYES = 6

function Make-WParam {
    param([int]$Id, [int]$Notify = 0)
    return [IntPtr](($Notify -shl 16) -bor ($Id -band 0xffff))
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
        $handle = [NativePluginToolUi]::FindTopWindow([uint32]$Process.Id, $ClassName, $(if ($TitleContains) { $TitleContains } else { $null }))
        if ($handle -ne [IntPtr]::Zero -and [NativePluginToolUi]::IsWindow($handle)) {
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
        if (![NativePluginToolUi]::IsWindow($Hwnd)) {
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Window did not close."
}

function Wait-Control {
    param([IntPtr]$Parent, [int]$Id, [int]$TimeoutMs = 5000)

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (![NativePluginToolUi]::IsWindow($Parent)) {
            throw "Parent window closed while waiting for control: $Id"
        }
        $control = [NativePluginToolUi]::GetDlgItem($Parent, $Id)
        if ($control -ne [IntPtr]::Zero -and [NativePluginToolUi]::IsWindow($control)) {
            return $control
        }
        Start-Sleep -Milliseconds 50
    }
    [NativePluginToolUi]::ChildWindowSummary($Parent) | Set-Content -Path (Join-Path $LogDir "plugin-tool-missing-control.txt") -Encoding UTF8
    throw "Control not found: $Id"
}

function Assert-ControlVisible {
    param([IntPtr]$Parent, [int]$Id, [bool]$Visible, [string]$Name)

    $control = Wait-Control -Parent $Parent -Id $Id
    $actual = [NativePluginToolUi]::IsWindowVisible($control)
    if ($actual -ne $Visible) {
        throw "Unexpected visibility for $Name control $Id. Expected $Visible, got $actual."
    }
}

function Assert-ControlText {
    param([IntPtr]$Parent, [int]$Id, [string]$Expected, [string]$Name)

    $control = Wait-Control -Parent $Parent -Id $Id
    $actual = [NativePluginToolUi]::ControlText($control)
    if ($actual -ne $Expected) {
        throw "Unexpected text for $Name control $Id. Expected '$Expected', got '$actual'."
    }
}

function Post-Command {
    param([IntPtr]$Parent, [int]$Id, [int]$Notify = 0)
    [NativePluginToolUi]::PostMessage($Parent, $WM_COMMAND, (Make-WParam -Id $Id -Notify $Notify), [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 150
}

function Invoke-ControlClick {
    param([IntPtr]$Parent, [int]$Id)
    $control = Wait-Control -Parent $Parent -Id $Id
    $handled = [NativePluginToolUi]::SendMessage($Parent, $WM_TOOL_AUTOMATION, [IntPtr]$Id, [IntPtr]::Zero)
    if ($handled -eq [IntPtr]::Zero) {
        throw "Tool automation command was not handled: $Id"
    }
    Start-Sleep -Milliseconds 150
}

function Capture-Window {
    param([IntPtr]$Hwnd, [string]$Path, [string]$Name)

    [PluginToolRect]$rect = New-Object PluginToolRect
    if (![NativePluginToolUi]::GetWindowRect($Hwnd, [ref]$rect)) {
        throw "Unable to read window rectangle: $Name"
    }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -le 0 -or $height -le 0) {
        throw "Invalid window rectangle: $Name"
    }

    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $hdc = $graphics.GetHdc()
    try {
        [NativePluginToolUi]::PrintWindow($Hwnd, $hdc, 2) | Out-Null
    } finally {
        $graphics.ReleaseHdc($hdc)
        $graphics.Dispose()
    }
    $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    $screenshots.Add($Path) | Out-Null
}

function Assert-UsefulImage {
    param([string]$Path, [string]$Name)

    if (!(Test-Path $Path)) {
        throw "Screenshot missing: $Name"
    }
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

function Assert-Surface {
    param([IntPtr]$Hwnd, [string]$Name, [string]$Path, [int[]]$RequiredControlIds = @())

    Start-Sleep -Milliseconds 250
    foreach ($id in $RequiredControlIds) {
        Wait-Control -Parent $Hwnd -Id $id | Out-Null
    }
    $outside = [NativePluginToolUi]::OutOfBoundsVisibleChildren($Hwnd, 4)
    if (![string]::IsNullOrWhiteSpace($outside)) {
        $outside | Set-Content -Path (Join-Path $LogDir ("plugin-tool-" + $Name + "-bounds.txt")) -Encoding UTF8
        throw "Visible child control outside parent in $Name"
    }
    Capture-Window -Hwnd $Hwnd -Path $Path -Name $Name
    Assert-UsefulImage -Path $Path -Name $Name
}

function Get-ListItems {
    param([IntPtr]$List)

    $count = [int][NativePluginToolUi]::SendMessage($List, $LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
    $items = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $count; ++$i) {
        $buffer = New-Object System.Text.StringBuilder 1024
        [NativePluginToolUi]::SendMessageText($List, $LB_GETTEXT, [IntPtr]$i, $buffer) | Out-Null
        $items.Add($buffer.ToString()) | Out-Null
    }
    return @($items)
}

function Select-ListItemContaining {
    param([IntPtr]$Parent, [IntPtr]$List, [string]$Text)

    $items = Get-ListItems -List $List
    for ($i = 0; $i -lt $items.Count; ++$i) {
        if ($items[$i].Contains($Text)) {
            [NativePluginToolUi]::SendMessage($List, $LB_SETCURSEL, [IntPtr]$i, [IntPtr]::Zero) | Out-Null
            [NativePluginToolUi]::SendMessage($Parent, $WM_COMMAND, (Make-WParam -Id 6101 -Notify $LBN_SELCHANGE), $List) | Out-Null
            Start-Sleep -Milliseconds 200
            return $i
        }
    }
    $items | Set-Content -Path (Join-Path $LogDir "plugin-store-list-items.txt") -Encoding UTF8
    throw "List item not found: $Text"
}

function Assert-ListContains {
    param([IntPtr]$List, [string]$Text)
    $items = Get-ListItems -List $List
    if (!($items | Where-Object { $_.Contains($Text) } | Select-Object -First 1)) {
        $items | Set-Content -Path (Join-Path $LogDir "plugin-store-list-items.txt") -Encoding UTF8
        throw "Plugin list missing: $Text"
    }
}

function Assert-ControlEnabled {
    param([IntPtr]$Parent, [int]$Id, [bool]$Expected, [string]$Name)
    $control = Wait-Control -Parent $Parent -Id $Id
    $actual = [NativePluginToolUi]::IsWindowEnabled($control)
    if ($actual -ne $Expected) {
        throw "Unexpected enabled state for ${Name}: expected=$Expected actual=$actual"
    }
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
        $Lines | Set-Content -Path (Join-Path $LogDir "plugin-tool-probe-failed.txt") -Encoding UTF8
        throw "Probe assertion failed: $Name"
    }
}

function Assert-ProbeNotContains {
    param([string[]]$Lines, [string]$Pattern, [string]$Name)
    if ($Lines | Where-Object { $_ -match $Pattern } | Select-Object -First 1) {
        $Lines | Set-Content -Path (Join-Path $LogDir "plugin-tool-probe-failed.txt") -Encoding UTF8
        throw "Probe negative assertion failed: $Name"
    }
}

function Set-EditText {
    param([IntPtr]$Parent, [int]$Id, [string]$Text)
    $edit = Wait-Control -Parent $Parent -Id $Id
    [NativePluginToolUi]::SetControlText($edit, $Text)
}

function Set-Check {
    param([IntPtr]$Parent, [int]$Id, [bool]$Checked)
    $box = Wait-Control -Parent $Parent -Id $Id
    [NativePluginToolUi]::SendMessage($box, $BM_SETCHECK, [IntPtr]($(if ($Checked) { 1 } else { $BST_UNCHECKED })), [IntPtr]::Zero) | Out-Null
}

function Assert-ClipboardContains {
    param([string]$Text)
    $deadline = [DateTime]::UtcNow.AddSeconds(3)
    while ([DateTime]::UtcNow -lt $deadline) {
        $clip = [System.Windows.Forms.Clipboard]::GetText()
        if ($clip.Contains($Text)) {
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Clipboard text missing: $Text"
}

function Invoke-MessageBoxButton {
    param([IntPtr]$Dialog, [int]$ButtonId)

    [NativePluginToolUi]::SendMessage($Dialog, $WM_COMMAND, (Make-WParam -Id $ButtonId), [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 150
    if ([NativePluginToolUi]::IsWindow($Dialog)) {
        $button = [NativePluginToolUi]::GetDlgItem($Dialog, $ButtonId)
        if ($button -ne [IntPtr]::Zero -and [NativePluginToolUi]::IsWindow($button)) {
            [NativePluginToolUi]::SendMessage($button, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        } else {
            [NativePluginToolUi]::PostMessage($Dialog, $WM_COMMAND, [IntPtr]$ButtonId, [IntPtr]::Zero) | Out-Null
        }
    }
    if ($ButtonId -eq 1 -and [NativePluginToolUi]::IsWindow($Dialog)) {
        [NativePluginToolUi]::PostMessage($Dialog, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    }
}

function Write-TestStore {
    param([string]$RunDir)

    $storeDir = Join-Path $RunDir "plugins\store"
    $packageDir = Join-Path $storeDir "packages"
    $assetDir = Join-Path $packageDir "assets"
    New-Item -ItemType Directory -Force -Path $assetDir | Out-Null
    "plugin acceptance asset" | Set-Content -Path (Join-Path $assetDir "readme.txt") -Encoding UTF8

    $packageJson = @'
{
  "id": "quattro.acceptance.package",
  "name": "UI验收入口包",
  "version": "1.0.0",
  "category": "link-pack",
  "kind": "link-pack",
  "description": "自动化验收使用的本地包插件。",
  "author": "Quattro Tests",
  "license": "MIT",
  "permissions": ["create-groups", "create-links", "copy-files"],
  "files": [
    {"from": "assets/readme.txt", "to": "plugins/installed/quattro.acceptance.package/readme.txt"}
  ],
  "groups": [
    {
      "name": "验收工具",
      "tags": [
        {
          "name": "入口",
          "links": [
            {"name": "验收记事本", "path": "notepad.exe", "type": 0},
            {"name": "验收网址", "path": "https://example.com", "type": 2}
          ]
        }
      ]
    }
  ]
}
'@
    [System.IO.File]::WriteAllText((Join-Path $packageDir "acceptance-package.json"), $packageJson, [System.Text.UTF8Encoding]::new($false))

    $indexJson = @'
{
  "schema": 1,
  "name": "Quattro Acceptance Store",
  "plugins": [
    {
      "id": "quattro.acceptance.package",
      "name": "UI验收入口包",
      "version": "1.0.0",
      "category": "link-pack",
      "kind": "link-pack",
      "description": "从本地自建插件商店下载安装的验收包。",
      "author": "Quattro Tests",
      "license": "MIT",
      "permissions": ["create-groups", "create-links", "copy-files"],
      "packageUrl": "packages/acceptance-package.json"
    },
    {
      "id": "quattro.acceptance.direct",
      "name": "UI验收直装包",
      "version": "1.0.0",
      "category": "link-pack",
      "kind": "link-pack",
      "description": "直接在索引中声明内容的验收插件。",
      "author": "Quattro Tests",
      "license": "MIT",
      "permissions": ["create-groups", "create-links"],
      "groups": [
        {
          "name": "直装工具",
          "tags": [
            {
              "name": "入口",
              "links": [
                {"name": "直装计算器", "path": "calc.exe", "type": 0}
              ]
            }
          ]
        }
      ]
    }
  ]
}
'@
    [System.IO.File]::WriteAllText((Join-Path $storeDir "index.json"), $indexJson, [System.Text.UTF8Encoding]::new($false))
}

    $runDir = Join-Path ([System.IO.Path]::GetTempPath()) ("QuattroPluginToolTests_" + [Guid]::NewGuid().ToString("N"))
$process = $null
$main = [IntPtr]::Zero
$exportLogPath = ""
$previousNoFocus = $env:QUATTRO_TEST_NO_FOCUS
$previousExport = $env:QUATTRO_TEST_STOPWATCH_EXPORT

try {
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    Copy-Item -LiteralPath $ExePath -Destination (Join-Path $runDir "Quattro.exe") -Force
    Copy-Item -LiteralPath $ProbePath -Destination (Join-Path $runDir "QuattroDbProbe.exe") -Force
    Write-TestStore -RunDir $runDir

    $exportPath = Join-Path $runDir "stopwatch-export.txt"
    $env:QUATTRO_TEST_NO_FOCUS = '1'
    $env:QUATTRO_TEST_STOPWATCH_EXPORT = $exportPath
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
nWidth=520
nHeight=620
nPosX=80
nPosY=80
Theme=default
PluginStoreUrl=plugins/store/index.json
"@ | Set-Content -Path (Join-Path $runDir "conf.ini") -Encoding UTF8

    $process = Start-Process -FilePath (Join-Path $runDir "Quattro.exe") -WorkingDirectory $runDir -PassThru -WindowStyle Normal
    $main = Wait-ProcessWindow -Process $process -ClassName "QuattroMainWindow" -TitleContains "Quattro"
    [NativePluginToolUi]::MoveWindow($main, 80, 80, 520, 620, $true) | Out-Null
    Start-Sleep -Milliseconds 500

    [NativePluginToolUi]::PostMessage($main, $WM_COMMAND, [IntPtr]40059, [IntPtr]::Zero) | Out-Null
    $pluginDialog = Wait-ProcessWindow -Process $process -ClassName "QuattroPluginManagerDialog_*" -TitleContains "插件商店"
    $pluginList = Wait-Control -Parent $pluginDialog -Id 6101
    Assert-Surface -Hwnd $pluginDialog -Name "plugin-store-initial" -Path (Join-Path $LogDir "plugin-store-initial.png") -RequiredControlIds @(6101,6102,6103,6104,6105,6106)
    Assert-ListContains -List $pluginList -Text "连点器"
    Assert-ListContains -List $pluginList -Text "计时器"
    Assert-ListContains -List $pluginList -Text "秒表"
    Assert-ListContains -List $pluginList -Text "UI验收入口包"
    Assert-ListContains -List $pluginList -Text "UI验收直装包"

    Select-ListItemContaining -Parent $pluginDialog -List $pluginList -Text "连点器" | Out-Null
    Assert-ControlEnabled -Parent $pluginDialog -Id 6102 -Expected $true -Name "clicker enable"
    Assert-ControlEnabled -Parent $pluginDialog -Id 6103 -Expected $false -Name "clicker disable"
    Assert-Surface -Hwnd $pluginDialog -Name "plugin-store-clicker-disabled" -Path (Join-Path $LogDir "plugin-store-clicker-disabled.png") -RequiredControlIds @(6101,6102,6103)
    Post-Command -Parent $pluginDialog -Id 6102
    Select-ListItemContaining -Parent $pluginDialog -List $pluginList -Text "连点器" | Out-Null
    Assert-ControlEnabled -Parent $pluginDialog -Id 6102 -Expected $false -Name "clicker enable after enable"
    Assert-ControlEnabled -Parent $pluginDialog -Id 6103 -Expected $true -Name "clicker disable after enable"
    Assert-Surface -Hwnd $pluginDialog -Name "plugin-store-clicker-enabled" -Path (Join-Path $LogDir "plugin-store-clicker-enabled.png") -RequiredControlIds @(6101,6102,6103)
    Post-Command -Parent $pluginDialog -Id 6103
    Select-ListItemContaining -Parent $pluginDialog -List $pluginList -Text "连点器" | Out-Null
    Assert-Surface -Hwnd $pluginDialog -Name "plugin-store-clicker-disabled-again" -Path (Join-Path $LogDir "plugin-store-clicker-disabled-again.png") -RequiredControlIds @(6101,6102,6103)
    Post-Command -Parent $pluginDialog -Id 6102

    Post-Command -Parent $pluginDialog -Id 6106
    $refreshBox = Wait-ProcessWindow -Process $process -ClassName "#32770" -TitleContains "刷新插件商店"
    Assert-Surface -Hwnd $refreshBox -Name "plugin-store-refresh-message" -Path (Join-Path $LogDir "plugin-store-refresh-message.png")
    Invoke-MessageBoxButton -Dialog $refreshBox -ButtonId 1
    Wait-WindowClosed -Hwnd $refreshBox

    Select-ListItemContaining -Parent $pluginDialog -List $pluginList -Text "UI验收入口包" | Out-Null
    Assert-ControlEnabled -Parent $pluginDialog -Id 6104 -Expected $true -Name "store package install"
    Assert-Surface -Hwnd $pluginDialog -Name "plugin-store-package-uninstalled" -Path (Join-Path $LogDir "plugin-store-package-uninstalled.png") -RequiredControlIds @(6101,6104,6105)
    Post-Command -Parent $pluginDialog -Id 6104
    Start-Sleep -Milliseconds 800
    Select-ListItemContaining -Parent $pluginDialog -List $pluginList -Text "UI验收入口包" | Out-Null
    Assert-ControlEnabled -Parent $pluginDialog -Id 6105 -Expected $true -Name "store package remove"
    Assert-Surface -Hwnd $pluginDialog -Name "plugin-store-package-installed" -Path (Join-Path $LogDir "plugin-store-package-installed.png") -RequiredControlIds @(6101,6104,6105)

    $probeAfterInstall = Read-Probe -Directory $runDir
    $probeAfterInstall | Set-Content -Path (Join-Path $LogDir "plugin-tool-probe-installed.txt") -Encoding UTF8
    Assert-ProbeContains -Lines $probeAfterInstall -Pattern "PLUGIN.*id=quattro\.acceptance\.package.*enabled=1.*installed=1" -Name "package plugin installed"
    Assert-ProbeContains -Lines $probeAfterInstall -Pattern "GROUP.*name=验收工具" -Name "package group installed"
    Assert-ProbeContains -Lines $probeAfterInstall -Pattern "LINK.*name=验收记事本.*path=notepad\.exe" -Name "package link installed"
    if (!(Test-Path (Join-Path $runDir "plugins\installed\quattro.acceptance.package\readme.txt"))) {
        throw "Installed plugin file was not copied."
    }

    Post-Command -Parent $pluginDialog -Id 6105
    $removeBox = Wait-ProcessWindow -Process $process -ClassName "#32770" -TitleContains "删除插件"
    Assert-Surface -Hwnd $removeBox -Name "plugin-store-remove-confirm" -Path (Join-Path $LogDir "plugin-store-remove-confirm.png")
    Invoke-MessageBoxButton -Dialog $removeBox -ButtonId $IDYES
    Wait-WindowClosed -Hwnd $removeBox
    Start-Sleep -Milliseconds 800
    Select-ListItemContaining -Parent $pluginDialog -List $pluginList -Text "UI验收入口包" | Out-Null
    Assert-Surface -Hwnd $pluginDialog -Name "plugin-store-package-removed" -Path (Join-Path $LogDir "plugin-store-package-removed.png") -RequiredControlIds @(6101,6104,6105)

    $probeAfterRemove = Read-Probe -Directory $runDir
    $probeAfterRemove | Set-Content -Path (Join-Path $LogDir "plugin-tool-probe-removed.txt") -Encoding UTF8
    Assert-ProbeContains -Lines $probeAfterRemove -Pattern "PLUGIN.*id=quattro\.acceptance\.package.*enabled=0.*installed=0" -Name "package plugin removed"
    Assert-ProbeNotContains -Lines $probeAfterRemove -Pattern "LINK.*name=验收记事本" -Name "package link removed"
    if (Test-Path (Join-Path $runDir "plugins\installed\quattro.acceptance.package\readme.txt")) {
        throw "Installed plugin file was not removed."
    }

    [NativePluginToolUi]::PostMessage($pluginDialog, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Wait-WindowClosed -Hwnd $pluginDialog
    Start-Sleep -Milliseconds 500

    $seenTools = @{}
    for ($command = 47000; $command -lt 47003; ++$command) {
        [NativePluginToolUi]::PostMessage($main, $WM_COMMAND, [IntPtr]$command, [IntPtr]::Zero) | Out-Null
        $tool = Wait-ProcessWindow -Process $process -ClassName "QuattroBuiltinTool_*" -TitleContains "" -TimeoutMs 8000
        $title = [NativePluginToolUi]::WindowText($tool)
        $seenTools[$title] = $true

        if ($title -eq "连点器") {
            Assert-Surface -Hwnd $tool -Name "tool-clicker-ready" -Path (Join-Path $LogDir "tool-clicker-ready.png") -RequiredControlIds @(7001,7003,7004,7005,7006,7007,7013,7014,7017,7019)
            [PluginToolRect]$toolRect = New-Object PluginToolRect
            [NativePluginToolUi]::GetWindowRect($tool, [ref]$toolRect) | Out-Null
            Set-EditText -Parent $tool -Id 7001 -Text (([string]($toolRect.Left + 32)) + ", " + ([string]($toolRect.Top + 210)))
            Set-EditText -Parent $tool -Id 7003 -Text "1"
            Set-EditText -Parent $tool -Id 7004 -Text "100"
            Set-EditText -Parent $tool -Id 7013 -Text "0"
            [NativePluginToolUi]::SendMessage((Wait-Control -Parent $tool -Id 7005), $CB_SETCURSEL, [IntPtr]0, [IntPtr]::Zero) | Out-Null
            Assert-Surface -Hwnd $tool -Name "tool-clicker-configured" -Path (Join-Path $LogDir "tool-clicker-configured.png") -RequiredControlIds @(7001,7003,7004,7007,7019)
            Invoke-ControlClick -Parent $tool -Id 7007
            Start-Sleep -Milliseconds 700
            Assert-Surface -Hwnd $tool -Name "tool-clicker-complete" -Path (Join-Path $LogDir "tool-clicker-complete.png") -RequiredControlIds @(7001,7003,7004,7007,7019)
            Set-EditText -Parent $tool -Id 7003 -Text "0"
            Assert-Surface -Hwnd $tool -Name "tool-clicker-infinite-configured" -Path (Join-Path $LogDir "tool-clicker-infinite-configured.png") -RequiredControlIds @(7001,7003,7004,7007,7019)
            Invoke-ControlClick -Parent $tool -Id 7007
            Start-Sleep -Milliseconds 350
            Invoke-ControlClick -Parent $tool -Id 7007
            Assert-Surface -Hwnd $tool -Name "tool-clicker-infinite-stopped" -Path (Join-Path $LogDir "tool-clicker-infinite-stopped.png") -RequiredControlIds @(7001,7003,7004,7007,7019)
            [NativePluginToolUi]::PostMessage($tool, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
            Wait-WindowClosed -Hwnd $tool
        } elseif ($title -eq "计时器") {
            Assert-Surface -Hwnd $tool -Name "tool-timer-ready" -Path (Join-Path $LogDir "tool-timer-ready.png") -RequiredControlIds @(7101,7107,7108,7102,7104,7109,7110)
            Assert-ControlVisible -Parent $tool -Id 7102 -Visible $true -Name "timer-ready-start"
            Assert-ControlVisible -Parent $tool -Id 7103 -Visible $false -Name "timer-ready-pause"
            Assert-ControlText -Parent $tool -Id 7102 -Expected "开始(&S)" -Name "timer-ready-start"
            Set-EditText -Parent $tool -Id 7101 -Text "0"
            Set-EditText -Parent $tool -Id 7107 -Text "0"
            Set-EditText -Parent $tool -Id 7108 -Text "1"
            Set-Check -Parent $tool -Id 7109 -Checked $false
            Set-Check -Parent $tool -Id 7110 -Checked $false
            Assert-Surface -Hwnd $tool -Name "tool-timer-configured" -Path (Join-Path $LogDir "tool-timer-configured.png") -RequiredControlIds @(7101,7107,7108,7102,7104)
            Invoke-ControlClick -Parent $tool -Id 7102
            Start-Sleep -Milliseconds 350
            Assert-Surface -Hwnd $tool -Name "tool-timer-running" -Path (Join-Path $LogDir "tool-timer-running.png") -RequiredControlIds @(7101,7107,7108,7103,7104)
            Assert-ControlVisible -Parent $tool -Id 7102 -Visible $false -Name "timer-running-start"
            Assert-ControlVisible -Parent $tool -Id 7103 -Visible $true -Name "timer-running-pause"
            Invoke-ControlClick -Parent $tool -Id 7103
            Assert-Surface -Hwnd $tool -Name "tool-timer-paused" -Path (Join-Path $LogDir "tool-timer-paused.png") -RequiredControlIds @(7101,7107,7108,7102,7104)
            Assert-ControlVisible -Parent $tool -Id 7102 -Visible $true -Name "timer-paused-start"
            Assert-ControlVisible -Parent $tool -Id 7103 -Visible $false -Name "timer-paused-pause"
            Assert-ControlText -Parent $tool -Id 7102 -Expected "继续(&S)" -Name "timer-paused-start"
            Invoke-ControlClick -Parent $tool -Id 7102
            [NativePluginToolUi]::StartClickTopWindowButtonWhenAppears([uint32]$process.Id, "#32770", "", 1, 5000, 700)
            $timerHandled = [NativePluginToolUi]::SendMessage($tool, $WM_TOOL_TIMER_AUTOMATION, [IntPtr]7106, [IntPtr]::Zero)
            if ($timerHandled -eq [IntPtr]::Zero) {
                throw "Timer automation tick was not handled."
            }
            Start-Sleep -Milliseconds 300
            Assert-Surface -Hwnd $tool -Name "tool-timer-finished" -Path (Join-Path $LogDir "tool-timer-finished.png") -RequiredControlIds @(7101,7107,7108,7102,7104)
            Assert-ControlVisible -Parent $tool -Id 7102 -Visible $true -Name "timer-finished-start"
            Assert-ControlVisible -Parent $tool -Id 7103 -Visible $false -Name "timer-finished-pause"
            Invoke-ControlClick -Parent $tool -Id 7104
            Assert-Surface -Hwnd $tool -Name "tool-timer-reset" -Path (Join-Path $LogDir "tool-timer-reset.png") -RequiredControlIds @(7101,7107,7108,7102,7104)
            Assert-ControlText -Parent $tool -Id 7102 -Expected "开始(&S)" -Name "timer-reset-start"
            [NativePluginToolUi]::PostMessage($tool, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
            Wait-WindowClosed -Hwnd $tool
        } elseif ($title -eq "秒表") {
            Assert-Surface -Hwnd $tool -Name "tool-stopwatch-ready" -Path (Join-Path $LogDir "tool-stopwatch-ready.png") -RequiredControlIds @(7201,7203,7204,7205,7207,7209)
            Invoke-ControlClick -Parent $tool -Id 7201
            Start-Sleep -Milliseconds 350
            Invoke-ControlClick -Parent $tool -Id 7204
            Start-Sleep -Milliseconds 200
            Invoke-ControlClick -Parent $tool -Id 7202
            Assert-Surface -Hwnd $tool -Name "tool-stopwatch-lap-paused" -Path (Join-Path $LogDir "tool-stopwatch-lap-paused.png") -RequiredControlIds @(7201,7203,7204,7205,7207,7209)
            Invoke-ControlClick -Parent $tool -Id 7205
            Assert-ClipboardContains -Text "当前时间"
            Invoke-ControlClick -Parent $tool -Id 7209
            $deadline = [DateTime]::UtcNow.AddSeconds(5)
            while (!(Test-Path $exportPath) -and [DateTime]::UtcNow -lt $deadline) {
                Start-Sleep -Milliseconds 100
            }
            if (!(Test-Path $exportPath)) {
                throw "Stopwatch export file was not created."
            }
            $exportText = Get-Content -LiteralPath $exportPath -Raw -Encoding UTF8
            if (!$exportText.Contains("当前时间") -or !$exportText.Contains("1.")) {
                throw "Stopwatch export content is incomplete."
            }
            $exportLogPath = Join-Path $LogDir "tool-stopwatch-export.txt"
            Copy-Item -LiteralPath $exportPath -Destination $exportLogPath -Force
            Assert-Surface -Hwnd $tool -Name "tool-stopwatch-exported" -Path (Join-Path $LogDir "tool-stopwatch-exported.png") -RequiredControlIds @(7201,7203,7204,7205,7207,7209)
            Invoke-ControlClick -Parent $tool -Id 7203
            Assert-Surface -Hwnd $tool -Name "tool-stopwatch-reset" -Path (Join-Path $LogDir "tool-stopwatch-reset.png") -RequiredControlIds @(7201,7203,7204,7205,7207,7209)
            [NativePluginToolUi]::PostMessage($tool, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
            Wait-WindowClosed -Hwnd $tool
        } else {
            [NativePluginToolUi]::ChildWindowSummary($tool) | Set-Content -Path (Join-Path $LogDir "unknown-tool-window.txt") -Encoding UTF8
            throw "Unexpected builtin tool title: $title"
        }
        Start-Sleep -Milliseconds 300
    }

    foreach ($required in @("连点器", "计时器", "秒表")) {
        if (!$seenTools.ContainsKey($required)) {
            throw "Builtin tool was not opened: $required"
        }
    }

    [NativePluginToolUi]::PostMessage($main, $WM_COMMAND, [IntPtr]40005, [IntPtr]::Zero) | Out-Null
    if (!$process.WaitForExit(5000)) {
        throw "Process did not exit."
    }

    $finalProbe = Read-Probe -Directory $runDir
    $finalProbe | Set-Content -Path (Join-Path $LogDir "plugin-tool-probe-final.txt") -Encoding UTF8
    Assert-ProbeContains -Lines $finalProbe -Pattern "PLUGIN.*id=quattro\.builtin\.clicker.*enabled=1.*installed=1" -Name "clicker enabled persisted"
    Assert-ProbeContains -Lines $finalProbe -Pattern "PLUGIN_SETTING.*plugin=quattro\.builtin\.clicker.*key=count.*value=0" -Name "clicker infinite count persisted"
    Assert-ProbeContains -Lines $finalProbe -Pattern "PLUGIN_SETTING.*plugin=quattro\.builtin\.clicker.*key=interval.*value=100" -Name "clicker interval persisted"
    Assert-ProbeContains -Lines $finalProbe -Pattern "PLUGIN_SETTING.*plugin=quattro\.builtin\.timer.*key=secondsPart.*value=1" -Name "timer seconds persisted"
    Assert-ProbeContains -Lines $finalProbe -Pattern "PLUGIN_SETTING.*plugin=quattro\.builtin\.stopwatch.*key=laps" -Name "stopwatch lap setting persisted"

    "plugin_tool_tests=passed"
    "screenshots=$($screenshots -join ';')"
    "probe=$(Join-Path $LogDir "plugin-tool-probe-final.txt")"
    "stopwatch_export=$exportLogPath"
} finally {
    if ($null -eq $previousNoFocus) {
        Remove-Item Env:\QUATTRO_TEST_NO_FOCUS -ErrorAction SilentlyContinue
    } else {
        $env:QUATTRO_TEST_NO_FOCUS = $previousNoFocus
    }
    if ($null -eq $previousExport) {
        Remove-Item Env:\QUATTRO_TEST_STOPWATCH_EXPORT -ErrorAction SilentlyContinue
    } else {
        $env:QUATTRO_TEST_STOPWATCH_EXPORT = $previousExport
    }
    if (![string]::IsNullOrWhiteSpace($runDir)) {
        $runDir | Set-Content -Path (Join-Path $LogDir "plugin-tool-run-dir.txt") -Encoding UTF8
        $appLog = Join-Path $runDir "logs\app.log"
        if (Test-Path $appLog) {
            Copy-Item -LiteralPath $appLog -Destination (Join-Path $LogDir "plugin-tool-app.log") -Force
        }
    }
    if ($process -and !$process.HasExited) {
        try {
            if ($main -ne [IntPtr]::Zero) {
                [NativePluginToolUi]::PostMessage($main, $WM_COMMAND, [IntPtr]40005, [IntPtr]::Zero) | Out-Null
            }
            if (!$process.WaitForExit(2000)) {
                Stop-Process -Id $process.Id -Force
            }
        } catch {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
    }
    Remove-Item -LiteralPath $runDir -Recurse -Force -ErrorAction SilentlyContinue
}
