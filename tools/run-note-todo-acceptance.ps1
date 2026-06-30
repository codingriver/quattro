param(
    [string]$Configuration = "Debug"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Assert-Contains {
    param(
        [string]$Path,
        [string]$Needle,
        [string]$Name
    )
    $fullPath = Join-Path $root $Path
    $text = Get-Content -LiteralPath $fullPath -Raw -Encoding UTF8
    if (!$text.Contains($Needle)) {
        throw "Missing acceptance wiring: $Name"
    }
}

function Assert-CountAtLeast {
    param(
        [string]$Path,
        [string]$Needle,
        [int]$Minimum,
        [string]$Name
    )
    $fullPath = Join-Path $root $Path
    $text = Get-Content -LiteralPath $fullPath -Raw -Encoding UTF8
    $count = ([regex]::Matches($text, [regex]::Escape($Needle))).Count
    if ($count -lt $Minimum) {
        throw "Acceptance wiring count failed: $Name expected >= $Minimum actual $count"
    }
}

function Assert-BlockNotContains {
    param(
        [string]$Path,
        [string]$StartNeedle,
        [string]$EndNeedle,
        [string[]]$BlockedNeedles,
        [string]$Name
    )
    $fullPath = Join-Path $root $Path
    $text = Get-Content -LiteralPath $fullPath -Raw -Encoding UTF8
    $start = $text.IndexOf($StartNeedle)
    if ($start -lt 0) {
        throw "Acceptance block start missing: $Name"
    }
    $end = $text.IndexOf($EndNeedle, $start)
    if ($end -lt 0) {
        throw "Acceptance block end missing: $Name"
    }
    $block = $text.Substring($start, $end - $start)
    foreach ($needle in $BlockedNeedles) {
        if ($block.Contains($needle)) {
            throw "Blocked wiring found in ${Name}: $needle"
        }
    }
}

$todoDialogEnableText = "$([char]0x542F)$([char]0x7528)"
$todoEnableText = "$([char]0x542F)$([char]0x7528)$([char]0x5F85)$([char]0x529E)$([char]0x4E8B)$([char]0x9879)"
$todoDisableText = "$([char]0x7981)$([char]0x7528)$([char]0x5F85)$([char]0x529E)$([char]0x4E8B)$([char]0x9879)"
$recommendedDueSortText = "$([char]0x6309)$([char]0x63D0)$([char]0x9192)$([char]0x65F6)$([char]0x95F4)$([char]0xFF08)$([char]0x63A8)$([char]0x8350)$([char]0xFF09)"
$disabledText = "$([char]0x5DF2)$([char]0x7981)$([char]0x7528)"

if (!(Test-Path (Join-Path $root "build\CMakeCache.txt"))) {
    cmake -S $root -B (Join-Path $root "build") -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed."
    }
}

cmake --build build --target Quattro QuattroTests QuattroNoteTodoAcceptance --config $Configuration -- /m
if ($LASTEXITCODE -ne 0) {
    throw "Build failed."
}

$testExe = Join-Path $root "build\$Configuration\QuattroTests.exe"
if (!(Test-Path $testExe)) {
    throw "Unit test executable not found: $testExe"
}
& $testExe
if ($LASTEXITCODE -ne 0) {
    throw "Unit tests failed."
}

$acceptanceExe = Join-Path $root "build\$Configuration\QuattroNoteTodoAcceptance.exe"
if (!(Test-Path $acceptanceExe)) {
    throw "Acceptance executable not found: $acceptanceExe"
}
& $acceptanceExe
if ($LASTEXITCODE -ne 0) {
    throw "Note/todo acceptance failed."
}

Assert-Contains -Path "src\Models.h" -Needle "bool enabled = true;" -Name "TodoItem enabled default"
Assert-Contains -Path "src\Storage.cpp" -Needle "Enabled INTEGER DEFAULT 1" -Name "TodoItems Enabled schema"
Assert-Contains -Path "src\Storage.cpp" -Needle "ALTER TABLE TodoItems ADD COLUMN Enabled INTEGER DEFAULT 1;" -Name "TodoItems Enabled migration"
Assert-Contains -Path "src\TodoEditDialog.cpp" -Needle "IdEnabled" -Name "Todo edit enabled checkbox id"
Assert-Contains -Path "src\TodoEditDialog.cpp" -Needle "CreateCheckBox" -Name "Todo edit enabled checkbox control"
Assert-Contains -Path "src\TodoEditDialog.cpp" -Needle $todoDialogEnableText -Name "Todo edit enabled checkbox text"
Assert-Contains -Path "src\TodoEditDialog.cpp" -Needle "draft_.enabled = SendMessageW(enabledCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;" -Name "Todo edit enabled save"
Assert-Contains -Path "src\MainWindow.cpp" -Needle "ShowTodoReminderPanel(item);" -Name "Reminder panel"
Assert-Contains -Path "src\MainWindow.cpp" -Needle "ShowTodoSystemNotification(item);" -Name "System notification"
Assert-CountAtLeast -Path "src\MainWindow.cpp" -Needle "if (!item.enabled || !item.completedAt.empty() || item.nextDueAt.empty())" -Minimum 2 -Name "Disabled todo suppresses overdue and reminder"
Assert-Contains -Path "src\MainWindow.cpp" -Needle "ID_MENU_TOGGLE_TODO_ENABLED" -Name "Todo enabled menu command"
Assert-Contains -Path "src\MainWindow.cpp" -Needle $todoDisableText -Name "Todo right-click disable label"
Assert-Contains -Path "src\MainWindow.cpp" -Needle $todoEnableText -Name "Todo right-click enable label"
Assert-Contains -Path "src\MainWindow.cpp" -Needle $recommendedDueSortText -Name "Recommended due-time sort"
Assert-Contains -Path "src\MainWindow.cpp" -Needle "ID_MENU_TODO_SORT_CREATED" -Name "Created-time sort"
Assert-Contains -Path "src\MainWindow.cpp" -Needle "ID_MENU_TODO_SORT_TITLE" -Name "Title sort"
Assert-Contains -Path "src\MainWindow.cpp" -Needle "ID_MENU_TODO_SORT_STATUS" -Name "Status sort"
Assert-Contains -Path "src\MainWindow.cpp" -Needle "if (left->enabled != right->enabled)" -Name "Disabled todo sorted after enabled"
Assert-Contains -Path "src\MainWindow.cpp" -Needle $disabledText -Name "Disabled todo list label"
Assert-Contains -Path "src\MainWindow.cpp" -Needle "bool IsNoteTag(const Group& tag)" -Name "Note tag detection"
Assert-Contains -Path "src\MainWindow.cpp" -Needle "bool IsTodoItemsTag(const Group& tag)" -Name "Todo item tag detection"
Assert-Contains -Path "src\MainWindow.cpp" -Needle "bool IsTodoTag(const Group& tag)" -Name "Legacy todo filter still exists"
Assert-Contains -Path "src\MenuCatalog.h" -Needle "ID_MENU_TOGGLE_TODO_ENABLED" -Name "Todo enabled command catalog"
Assert-Contains -Path "src\MenuCatalog.cpp" -Needle "MenuIconEyeOff" -Name "Todo disable icon"
Assert-Contains -Path "CMakeLists.txt" -Needle "QuattroNoteTodoAcceptance" -Name "Acceptance target registered"
Assert-BlockNotContains `
    -Path "src\MainWindow.cpp" `
    -StartNeedle "void MainWindow::ShowTodoMenu" `
    -EndNeedle "void MainWindow::CreateTooltip" `
    -BlockedNeedles @("ID_MENU_MOVE_UP", "ID_MENU_MOVE_DOWN") `
    -Name "Todo menu has no manual ordering"

$logDir = Join-Path (Join-Path $root "build\$Configuration") "logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$report = Join-Path $logDir "note-todo-acceptance.txt"
@(
    "note_todo_acceptance=passed"
    "configuration=$Configuration"
    "timestamp=$((Get-Date).ToString('s'))"
    "unit_tests=passed"
    "acceptance_exe=passed"
    "source_wiring=passed"
) | Set-Content -LiteralPath $report -Encoding UTF8

"note_todo_acceptance=passed"
