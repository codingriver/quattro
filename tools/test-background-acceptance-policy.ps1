$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$backgroundScripts = @(
    "run-ui-smoke.ps1",
    "run-menu-tests.ps1",
    "run-menu-visual-tests.ps1",
    "run-scroll-tests.ps1",
    "run-dialog-display-tests.ps1",
    "run-display-settings-tests.ps1",
    "run-settings-consumption-tests.ps1",
    "run-plugin-tool-tests.ps1"
)

$forbidden = @(
    "Get-Process\s+Quattro",
    "SetForegroundWindow",
    "SetActiveWindow",
    "SetFocus",
    "BringWindowToTop",
    "SwitchToThisWindow",
    "SetCursorPos",
    "GetCursorPos",
    "GetCursorInfo",
    "GetMessagePos",
    "SendInput",
    "mouse_event",
    "CopyFromScreen",
    "BitBlt\(",
    "HWND_TOPMOST",
    "Start-Process\s+-FilePath.*Quattro\.exe"
)

foreach ($name in $backgroundScripts) {
    $path = Join-Path $PSScriptRoot $name
    if (!(Test-Path -LiteralPath $path)) {
        throw "Background acceptance script is missing: $name"
    }
    $text = Get-Content -LiteralPath $path -Raw
    if ($text -notmatch "Start-QuattroTestProcess") {
        throw "Background acceptance bypasses the isolated launcher: $name"
    }
    foreach ($pattern in $forbidden) {
        if ($text -match $pattern) {
            throw "Background acceptance contains forbidden desktop/process operation: $name pattern=$pattern"
        }
    }
}

$harness = Get-Content -LiteralPath (Join-Path $PSScriptRoot "QuattroTestHarness.ps1") -Raw
foreach ($required in @(
    "QUATTRO_USER_CONFIG_DIR",
    "QUATTRO_TEST_MODE",
    "QUATTRO_TEST_NO_FOCUS",
    "QUATTRO_ACCEPTANCE_MODE",
    "QUATTRO_TEST_RUN_ID",
    "Stop-Process -Id"
)) {
    if ($harness -notmatch [regex]::Escape($required)) {
        throw "Quattro test harness is missing required isolation behavior: $required"
    }
}

$uiAcceptance = Get-Content -LiteralPath (Join-Path $root "tests\UiScreenshotAcceptance.cpp") -Raw
foreach ($pattern in @(
    "SetForegroundWindow",
    "SetActiveWindow",
    "SetFocus\(",
    "BringWindowToTop",
    "SwitchToThisWindow",
    "SetCursorPos",
    "GetCursorPos",
    "GetCursorInfo",
    "GetMessagePos",
    "SendInput",
    "mouse_event",
    "CopyFromScreen",
    "BitBlt\(",
    "HWND_TOPMOST"
)) {
    if ($uiAcceptance -match $pattern) {
        throw "C++ screenshot acceptance contains foreground/screen capture operation: $pattern"
    }
}
foreach ($required in @(
    "QUATTRO_ACCEPTANCE_MODE",
    "SW_SHOWNOACTIVATE",
    "HWND_BOTTOM",
    "ShowTooltip",
    "PrintWindow",
    "BitmapHasVisualContent"
)) {
    if ($uiAcceptance -notmatch [regex]::Escape($required)) {
        throw "C++ screenshot acceptance is missing background-safe behavior: $required"
    }
}

$menuVisual = Get-Content -LiteralPath (Join-Path $PSScriptRoot "run-menu-visual-tests.ps1") -Raw
foreach ($required in @("#32768", "PrintWindow", "Assert-MenuScreenshot")) {
    if ($menuVisual -notmatch [regex]::Escape($required)) {
        throw "Menu visual acceptance is missing popup HWND capture behavior: $required"
    }
}

$agentRules = Get-Content -LiteralPath (Join-Path $root "AGENTS.md") -Raw
foreach ($required in @(
    "SW_SHOWNOACTIVATE",
    "HWND_BOTTOM",
    "GetCursorPos",
    "ShowTooltip",
    "PrintWindow",
    "#32768",
    "CopyFromScreen",
    "独立测试运行根目录"
)) {
    if ($agentRules -notmatch [regex]::Escape($required)) {
        throw "AGENTS.md is missing a required background acceptance rule: $required"
    }
}

foreach ($name in @(
    "capture-theme-menu-icon-state.ps1",
    "capture-sort-run-asc-menu.ps1",
    "capture-sort-acceptance.ps1"
)) {
    $text = Get-Content -LiteralPath (Join-Path $PSScriptRoot $name) -Raw
    if ($text -notmatch "InteractiveVisual" -or $text -notmatch "isolated interactive Windows session") {
        throw "Legacy cursor capture is not isolated-interactive gated: $name"
    }
}

"background_acceptance_policy=passed"
