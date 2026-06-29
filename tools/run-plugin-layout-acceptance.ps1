param(
    [string]$ExePath = "E:\work\quattro\dist\x64\Quattro.exe",
    [string]$OutDir = "E:\work\quattro\build-vcpkg-preset\layout-acceptance"
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$runDir = Join-Path ([System.IO.Path]::GetTempPath()) ("QuattroLayoutAcceptance_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
Copy-Item -LiteralPath $ExePath -Destination (Join-Path $runDir 'Quattro.exe') -Force

$ini = @"
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
"@
Set-Content -Path (Join-Path $runDir 'conf.ini') -Value $ini -Encoding UTF8

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class NativeLayoutAcceptance {
  public delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr hwnd, EnumWindowsProc lpEnumFunc, IntPtr lParam);
  [DllImport("user32.dll", SetLastError=true)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int count);
  [DllImport("user32.dll", SetLastError=true)] public static extern int GetClassName(IntPtr hWnd, StringBuilder text, int count);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT rect);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref POINT point);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr hwnd, int id);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr hwnd, int x, int y, int w, int h, bool repaint);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
}
"@

function Get-Text([IntPtr]$h) {
    $sb = [Text.StringBuilder]::new(512)
    [NativeLayoutAcceptance]::GetWindowText($h, $sb, $sb.Capacity) | Out-Null
    $sb.ToString()
}

function Get-Class([IntPtr]$h) {
    $sb = [Text.StringBuilder]::new(256)
    [NativeLayoutAcceptance]::GetClassName($h, $sb, $sb.Capacity) | Out-Null
    $sb.ToString()
}

function Wait-Window([int]$ProcessId, [string]$ClassLike, [string]$TitleLike, [int]$TimeoutMs = 10000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        $script:found = [IntPtr]::Zero
        [NativeLayoutAcceptance]::EnumWindows({
            param($hwnd, $lParam)
            $wpid = 0
            [NativeLayoutAcceptance]::GetWindowThreadProcessId($hwnd, [ref]$wpid) | Out-Null
            if ($wpid -eq $ProcessId -and [NativeLayoutAcceptance]::IsWindowVisible($hwnd)) {
                $class = Get-Class $hwnd
                $title = Get-Text $hwnd
                if ($class -like $ClassLike -and $title -like $TitleLike) {
                    $script:found = $hwnd
                    return $false
                }
            }
            return $true
        }, [IntPtr]::Zero) | Out-Null
        if ($script:found -ne [IntPtr]::Zero) {
            return $script:found
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Window not found: class=$ClassLike title=$TitleLike"
}

function Get-Rect([IntPtr]$h) {
    $r = New-Object NativeLayoutAcceptance+RECT
    [NativeLayoutAcceptance]::GetWindowRect($h, [ref]$r) | Out-Null
    $r
}

function Get-ClientScreenRect([IntPtr]$h) {
    $r = New-Object NativeLayoutAcceptance+RECT
    [NativeLayoutAcceptance]::GetClientRect($h, [ref]$r) | Out-Null
    $p = New-Object NativeLayoutAcceptance+POINT
    [NativeLayoutAcceptance]::ClientToScreen($h, [ref]$p) | Out-Null
    [pscustomobject]@{
        Left = $p.X
        Top = $p.Y
        Right = $p.X + $r.Right
        Bottom = $p.Y + $r.Bottom
        Width = $r.Right
        Height = $r.Bottom
    }
}

function Child-ByText([IntPtr]$Parent, [string]$Text) {
    $script:child = [IntPtr]::Zero
    [NativeLayoutAcceptance]::EnumChildWindows($Parent, {
        param($h, $lParam)
        if ((Get-Text $h) -eq $Text) {
            $script:child = $h
            return $false
        }
        return $true
    }, [IntPtr]::Zero) | Out-Null
    $script:child
}

function Child-ByTextPrefix([IntPtr]$Parent, [string]$Prefix) {
    $script:childPrefix = [IntPtr]::Zero
    [NativeLayoutAcceptance]::EnumChildWindows($Parent, {
        param($h, $lParam)
        if ((Get-Text $h).StartsWith($Prefix)) {
            $script:childPrefix = $h
            return $false
        }
        return $true
    }, [IntPtr]::Zero) | Out-Null
    $script:childPrefix
}

function Capture-Window([IntPtr]$Hwnd, [string]$Path) {
    $r = Get-Rect $Hwnd
    $bmp = [Drawing.Bitmap]::new($r.Right - $r.Left, $r.Bottom - $r.Top)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
    $bmp.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose()
    $bmp.Dispose()
}

$env:QUATTRO_TEST_NO_FOCUS = '1'
$proc = Start-Process -FilePath (Join-Path $runDir 'Quattro.exe') -WorkingDirectory $runDir -PassThru -WindowStyle Normal
try {
    $main = Wait-Window -ProcessId $proc.Id -ClassLike 'QuattroMainWindow' -TitleLike '*Quattro*'
    [NativeLayoutAcceptance]::MoveWindow($main, 80, 80, 520, 620, $true) | Out-Null
    Start-Sleep -Milliseconds 500
    [NativeLayoutAcceptance]::PostMessage($main, 0x0111, [IntPtr]40059, [IntPtr]::Zero) | Out-Null
    $dlg = Wait-Window -ProcessId $proc.Id -ClassLike 'QuattroPluginManagerDialog_*' -TitleLike '*'
    [NativeLayoutAcceptance]::MoveWindow($dlg, 8, 8, 760, 840, $true) | Out-Null
    Start-Sleep -Milliseconds 800

    $shot = Join-Path $OutDir 'plugin-store-layout.png'
    Capture-Window $dlg $shot

    $client = Get-ClientScreenRect $dlg
    $refresh = Get-Rect ([NativeLayoutAcceptance]::GetDlgItem($dlg, 6106))
    $list = Get-Rect ([NativeLayoutAcceptance]::GetDlgItem($dlg, 6101))
    $sort = Get-Rect ([NativeLayoutAcceptance]::GetDlgItem($dlg, 6116))
    $view = Get-Rect ([NativeLayoutAcceptance]::GetDlgItem($dlg, 6117))
    $summaryH = Child-ByTextPrefix $dlg '共 '
    $sourceH = Child-ByTextPrefix $dlg '来源'
    $summary = Get-Rect $summaryH
    $source = Get-Rect $sourceH

    $report = @(
        "screenshot=$shot",
        "client=$($client.Width)x$($client.Height)",
        "topPad=$($refresh.Top - $client.Top)",
        "toolbarControlHeight=$($refresh.Bottom - $refresh.Top)",
        "listGap=$($list.Top - $refresh.Bottom)",
        "listHeight=$($list.Bottom - $list.Top)",
        "pagerControlHeight=$($sort.Bottom - $sort.Top)",
        "pagerStatusGap=$($summary.Top - $sort.Bottom)",
        "statusTextHeight=$($summary.Bottom - $summary.Top)",
        "bottomPad=$($client.Bottom - $summary.Bottom)",
        "sortText=$(Get-Text ([NativeLayoutAcceptance]::GetDlgItem($dlg, 6116)))",
        "viewText=$(Get-Text ([NativeLayoutAcceptance]::GetDlgItem($dlg, 6117)))",
        "sourceText=$(Get-Text $sourceH)"
    )
    $reportPath = Join-Path $OutDir 'plugin-store-layout-report.txt'
    $report | Set-Content -Path $reportPath -Encoding UTF8
    $report
}
finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
    Remove-Item -LiteralPath $runDir -Recurse -Force -ErrorAction SilentlyContinue
}
