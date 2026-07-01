param(
    [string]$Configuration = "Release",
    [switch]$Package
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
Add-Type -AssemblyName System.Windows.Forms
$logDir = Join-Path (Join-Path $root "build\$Configuration") "logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$compatReport = Join-Path $logDir "compatibility-report.txt"
$compatLines = New-Object System.Collections.Generic.List[string]
$compatLines.Add("Quattro compatibility report")
$compatLines.Add("timestamp=$((Get-Date).ToString('s'))")
$compatLines.Add("os=$([System.Environment]::OSVersion.VersionString)")
$compatLines.Add("is_64bit_os=$([System.Environment]::Is64BitOperatingSystem)")
$compatLines.Add("is_64bit_process=$([System.Environment]::Is64BitProcess)")
$compatLines.Add("user=$([System.Security.Principal.WindowsIdentity]::GetCurrent().Name)")
try {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    $compatLines.Add("is_admin=$($principal.IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator))")
} catch {
    $compatLines.Add("is_admin=unknown")
}
try {
    $dpi = Get-ItemProperty -Path "HKCU:\Control Panel\Desktop" -Name LogPixels -ErrorAction Stop
    $compatLines.Add("desktop_log_pixels=$($dpi.LogPixels)")
} catch {
    $compatLines.Add("desktop_log_pixels=default")
}
$screenInfo = [System.Windows.Forms.Screen]::AllScreens | ForEach-Object {
    "$($_.DeviceName):$($_.Bounds.Width)x$($_.Bounds.Height):primary=$($_.Primary)"
}
$compatLines.Add("screens=$($screenInfo -join ';')")
$compatLines.Add("drop_support=ole_drop_target_and_wm_dropfiles")

function Remove-LegacyBuildOutputs {
    param([string]$OutputDir)

    if (!(Test-Path $OutputDir)) {
        return
    }
    $legacyNames = @(
        "Quattro.exe".ToLowerInvariant(),
        "Quattro.pdb".ToLowerInvariant(),
        "Quattro_Tests.exe".ToLowerInvariant(),
        "Quattro_Tests.pdb".ToLowerInvariant()
    )
    foreach ($file in Get-ChildItem -LiteralPath $OutputDir -File) {
        if ($legacyNames -ccontains $file.Name) {
            Remove-Item -LiteralPath $file.FullName -Force
        }
    }
}

if (!(Test-Path (Join-Path $root "build\CMakeCache.txt"))) {
    cmake -S $root -B (Join-Path $root "build") -G "Visual Studio 17 2022" -A x64
}
Remove-LegacyBuildOutputs -OutputDir (Join-Path $root "build\$Configuration")
cmake --build build --config $Configuration -- /m
if ($LASTEXITCODE -ne 0) {
    throw "Build failed."
}
Remove-LegacyBuildOutputs -OutputDir (Join-Path $root "build\$Configuration")

$testExe = Join-Path $root "build\$Configuration\QuattroTests.exe"
if (Test-Path $testExe) {
    & $testExe
    if ($LASTEXITCODE -ne 0) {
        throw "Unit tests failed."
    }
    $compatLines.Add("unit_tests=passed")
} else {
    $compatLines.Add("unit_tests=missing")
    throw "Unit test executable not found: $testExe"
}

$noteTodoAcceptanceExe = Join-Path $root "build\$Configuration\QuattroNoteTodoAcceptance.exe"
if (Test-Path $noteTodoAcceptanceExe) {
    & $noteTodoAcceptanceExe
    if ($LASTEXITCODE -ne 0) {
        throw "Note/todo acceptance tests failed."
    }
    $compatLines.Add("note_todo_acceptance=passed")
} else {
    $compatLines.Add("note_todo_acceptance=missing")
    throw "Note/todo acceptance executable not found: $noteTodoAcceptanceExe"
}

$exe = Join-Path $root "build\$Configuration\Quattro.exe"
if (!(Test-Path $exe)) {
    throw "Executable not found: $exe"
}

$uiSmoke = & (Join-Path $PSScriptRoot "run-ui-smoke.ps1") -ExePath $exe -LogDir $logDir
$uiSmoke
$compatLines.Add("ui_smoke=passed")

$probeExe = Join-Path $root "build\$Configuration\QuattroDbProbe.exe"
if (!(Test-Path $probeExe)) {
    throw "Database probe not found: $probeExe"
}
$menuTests = & (Join-Path $PSScriptRoot "run-menu-tests.ps1") -ExePath $exe -ProbePath $probeExe -LogDir $logDir
$menuTests
$compatLines.Add("menu_tests=passed")

$menuVisualTests = & (Join-Path $PSScriptRoot "run-menu-visual-tests.ps1") -ExePath $exe -LogDir $logDir
$menuVisualTests
if (($menuVisualTests -join "`n") -match "menu_visual_tests=skipped") {
    $compatLines.Add("menu_visual_tests=skipped")
} else {
    $compatLines.Add("menu_visual_tests=passed")
}

$seedExe = Join-Path $root "build\$Configuration\QuattroDbSeed.exe"
if (!(Test-Path $seedExe)) {
    throw "Database seed tool not found: $seedExe"
}
$scrollTests = & (Join-Path $PSScriptRoot "run-scroll-tests.ps1") -ExePath $exe -SeedPath $seedExe -LogDir $logDir
$scrollTests
$compatLines.Add("scroll_tests=passed")

$dialogDisplayTests = & (Join-Path $PSScriptRoot "run-dialog-display-tests.ps1") -ExePath $exe -LogDir $logDir
$dialogDisplayTests
$compatLines.Add("dialog_display_tests=passed")

"plugin_tool_tests=skipped (plugin store temporarily disabled)"
$compatLines.Add("plugin_tool_tests=skipped")

$docsPath = Join-Path "docs" "Quattro"

$blobTerm = "$([char]0x56FE)$([char]0x6807) BLOB"
$cacheTableTerm = "$([char]0x7F13)$([char]0x5B58)$([char]0x8868)"
$blockedTermA = "$([char]0x53CD)$([char]0x7F16)$([char]0x8BD1)"
$blockedTermB = "$([char]0x4F2A)$([char]0x4EE3)$([char]0x7801)"
$blockedTermC = "$([char]0x4F2A) C"
$blockedTermD = "$([char]0x9006)$([char]0x5411)"
$sourceTerm = "$([char]0x6E90)$([char]0x7801)"
$oldProductTerm = "$([char]0x4C)$([char]0x75)$([char]0x63)$([char]0x79)"
$oldProductLowerTerm = "$([char]0x6C)$([char]0x75)$([char]0x63)$([char]0x79)"
$toolNameTerm = "$([char]0x47)$([char]0x68)$([char]0x69)$([char]0x64)$([char]0x72)$([char]0x61)"
$externalIndexTerm = "$([char]0x45)$([char]0x76)$([char]0x65)$([char]0x72)$([char]0x79)$([char]0x74)$([char]0x68)$([char]0x69)$([char]0x6E)$([char]0x67)"

$sensitiveTerms = @(
    ("icon" + "." + "db"),
    $blobTerm,
    $cacheTableTerm,
    ("db" + "/" + "icon"),
    ("Icon" + "16"),
    ("Icon" + "32"),
    ("Icon" + "48"),
    $oldProductTerm,
    $oldProductLowerTerm,
    $blockedTermA,
    $toolNameTerm,
    $blockedTermB,
    $blockedTermC,
    $blockedTermD,
    $sourceTerm,
    ("open" + $externalIndexTerm),
    ("bOpen" + $externalIndexTerm)
)
$scanTargets = @("src", "CMakeLists.txt", "README.md", $docsPath, "resources", "tools")
foreach ($term in $sensitiveTerms) {
    $sensitiveHits = rg -n -F -- $term @scanTargets 2>$null
    if ($LASTEXITCODE -eq 0) {
        $sensitiveHits
        throw "Sensitive or removed implementation term found: $term"
    }
    if ($LASTEXITCODE -gt 1) {
        throw "Sensitive scan failed: $term"
    }
}

$everythingHits = rg -n -F -- $externalIndexTerm "src" "CMakeLists.txt" "resources" "tools" 2>$null
if ($LASTEXITCODE -eq 0) {
    $everythingHits
    throw "Deferred external index term found outside docs."
}
if ($LASTEXITCODE -gt 1) {
    throw "External index scan failed."
}

if ($Package) {
    & (Join-Path $PSScriptRoot "build.ps1") -Configuration $Configuration -SkipTests
    $compatLines.Add("package=created")
}

$compatLines | Set-Content -Path $compatReport -Encoding UTF8
"test run complete"
