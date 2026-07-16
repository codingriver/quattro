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
    "默认包含 Quattro、AppLaunchLocker 和 QuattroUpdater",
    "验收 AppLaunchLocker、QuattroUpdater 或完整发布链路时仍必须使用 -All -Test",
    "-OfficialBuild",
    "-Release",
    "-Complete",
    "-Minimal",
    "-NoUpx",
    "-CompressEmbeddedAssets",
    "-PlanOnly",
    "-NoZip",
    "-Backend vcpkg|classic",
    "主窗口显示红色（DEBUG）标记",
    "主窗口显示红色（DEBUG-All）标记",
    "所有构建默认直接嵌入原始资源",
    "正式版与本地 DEBUG/DEBUG-All 均默认直接嵌入原始主题和图标资源",
    "默认启用 UPX"
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
    !$allPlan.Contains("embedded_assets=raw") -or
    !$allPlan.Contains("build_marker=DEBUG-All")) {
    throw "--all 未正确启用完整开发版构建计划。"
}

$completePlan = (& $buildScript -Complete -PlanOnly) -join "`n"
if (!$completePlan.Contains("architectures=x64") -or
    $completePlan.Contains("architectures=x86,x64") -or
    !$completePlan.Contains("bundle_optional_executables=ON") -or
    !$completePlan.Contains("build_marker=DEBUG-All") -or
    !$completePlan.Contains("build_profile=complete") -or
    !$completePlan.Contains("build_directories=build-vcpkg-x64-complete")) {
    throw "-Complete 未正确启用 x64 完整开发版构建计划。"
}

$defaultPlan = (& $buildScript -PlanOnly) -join "`n"
if (!$defaultPlan.Contains("architectures=x64") -or
    $defaultPlan.Contains("architectures=x86,x64") -or
    !$defaultPlan.Contains("bundle_optional_executables=ON") -or
    !$defaultPlan.Contains("build_marker=DEBUG-All") -or
    !$defaultPlan.Contains("build_profile=complete") -or
    !$defaultPlan.Contains("build_directories=build-vcpkg-x64-complete")) {
    throw "默认构建未正确启用 x64 完整开发版计划。"
}

$minimalPlan = (& $buildScript -Minimal -PlanOnly) -join "`n"
if (!$minimalPlan.Contains("architectures=x64") -or
    !$minimalPlan.Contains("bundle_optional_executables=OFF") -or
    !$minimalPlan.Contains("build_marker=DEBUG") -or
    !$minimalPlan.Contains("build_profile=minimal") -or
    !$minimalPlan.Contains("build_directories=build-vcpkg-x64")) {
    throw "-Minimal 未正确启用 x64 精简开发版构建计划。"
}

try {
    & $buildScript -Complete -Minimal -PlanOnly | Out-Null
    throw "-Complete 与 -Minimal 被意外同时接受。"
} catch {
    if ($_.Exception.Message -notmatch "cannot be used together") {
        throw
    }
}

try {
    & $buildScript -All -Minimal -PlanOnly | Out-Null
    throw "-All 与 -Minimal 被意外同时接受。"
} catch {
    if ($_.Exception.Message -notmatch "complete multi-platform build") {
        throw
    }
}

try {
    & $buildScript -Release -Minimal -PlanOnly | Out-Null
    throw "未指定平台的 -Release -Minimal 被意外接受。"
} catch {
    if ($_.Exception.Message -notmatch "complete multi-platform build") {
        throw
    }
}

$officialPlan = (& $buildScript -OfficialBuild -PlanOnly) -join "`n"
if (!$officialPlan.Contains("official_build=ON") -or
    !$officialPlan.Contains("embedded_assets=raw") -or
    !$officialPlan.Contains("build_marker=none")) {
    throw "-OfficialBuild 未正确启用正式原始资源计划。"
}

$compressedPlan = (& $buildScript -OfficialBuild -CompressEmbeddedAssets -PlanOnly) -join "`n"
if (!$compressedPlan.Contains("official_build=ON") -or
    !$compressedPlan.Contains("embedded_assets=xpress") -or
    !$compressedPlan.Contains("build_marker=none")) {
    throw "-CompressEmbeddedAssets 未正确启用资源压缩计划。"
}

"build_help_acceptance=passed"
