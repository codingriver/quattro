$ErrorActionPreference = "Stop"

$buildScript = Join-Path $PSScriptRoot "build.ps1"
$buildScriptBytes = [System.IO.File]::ReadAllBytes($buildScript)
if ($buildScriptBytes.Length -lt 3 -or
    $buildScriptBytes[0] -ne 0xEF -or
    $buildScriptBytes[1] -ne 0xBB -or
    $buildScriptBytes[2] -ne 0xBF) {
    throw "包含中文帮助的 build.ps1 必须保存为 UTF-8 BOM，以兼容 Windows PowerShell 5.1。"
}

$helpText = (& $buildScript -Help) -join "`n"
$gnuHelpText = (& $buildScript --help) -join "`n"

$requiredText = @(
    "Quattro 中文构建与打包工具",
    "默认只打包 Quattro，不构建、不打包 AppLaunchLocker 和 QuattroUpdater",
    "测试 AppLaunchLocker 或 QuattroUpdater 时必须使用 -All -Test",
    "-OfficialBuild",
    "-PlanOnly",
    "-NoZip",
    "-Backend vcpkg|classic",
    "主窗口显示红色（DEBUG）标记",
    "主窗口显示红色（DEBUG-All）标记"
)

foreach ($text in $requiredText) {
    if (!$helpText.Contains($text)) {
        throw "构建帮助缺少必要说明：$text"
    }
}

if ($helpText -match "(?m)^Usage:|(?m)^Defaults:|(?m)^Options:|(?m)^Examples:") {
    throw "构建帮助仍包含旧版英文分区标题。"
}

if ($gnuHelpText -ne $helpText) {
    throw "--help 与 -Help 的输出不一致。"
}

$allPlan = (& $buildScript --all -PlanOnly) -join "`n"
if (!$allPlan.Contains("architectures=x86,x64") -or
    !$allPlan.Contains("bundle_optional_executables=ON") -or
    !$allPlan.Contains("build_marker=DEBUG-All")) {
    throw "--all 未正确启用完整开发版构建计划。"
}

"build_help_acceptance=passed"
