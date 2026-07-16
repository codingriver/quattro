param(
    [ValidateSet("all", "x86", "x64", "--all", "--help")]
    [string]$Platform = "x64",
    [switch]$All,
    [string]$Configuration = "Release",
    [switch]$Clean,
    [switch]$Test,
    [switch]$SkipTests,
    [switch]$NoZip,
    [switch]$FullPackage,
    [switch]$FlatPackage,
    [switch]$Upx,
    [switch]$NoUpx,
    [string]$UpxPath = "upx",
    [switch]$PlanOnly,
    [switch]$Release,
    [switch]$OfficialBuild,
    [string]$Version = "",
    [string]$ReleaseRepoUrl = "https://github.com/codingriver/quattro",
    [string]$ReleaseTag = "",
    [ValidateSet("vcpkg", "classic")]
    [string]$Backend = "vcpkg",
    [switch]$Help
)

$ErrorActionPreference = "Stop"
$platformWasExplicit = $PSBoundParameters.ContainsKey("Platform")
$backendWasExplicit = $PSBoundParameters.ContainsKey("Backend")
$upxWasExplicit = $PSBoundParameters.ContainsKey("Upx")

if ($Upx -and $NoUpx) {
    throw "-Upx and -NoUpx cannot be used together."
}

# PowerShell 脚本参数原生使用单短横线。保留项目文档既有的 GNU 风格入口，
# 使 .\tools\build.ps1 --all / --help 与 -All / -Help 行为一致。
if ($Platform -eq "--all") {
    $All = $true
    $Platform = "x64"
} elseif ($Platform -eq "--help") {
    $Help = $true
    $Platform = "x64"
}

if ($Release) {
    if ($backendWasExplicit -and $Backend -ne "vcpkg") {
        throw "-Release uses the same vcpkg backend as GitHub Actions and cannot be combined with -Backend classic."
    }
    $Backend = "vcpkg"
    $OfficialBuild = $true
    $NoZip = $true
    if (!$NoUpx) {
        $Upx = $true
    }
    if ($Platform -eq "all") {
        $All = $true
    } elseif (!$platformWasExplicit -and !$All) {
        $All = $true
    }
}

if ($Upx -and !$PlanOnly) {
    $resolvedUpx = Get-Command $UpxPath -ErrorAction SilentlyContinue
    if ($resolvedUpx) {
        $UpxPath = $resolvedUpx.Source
    } elseif ($Release -and !$upxWasExplicit) {
        Write-Warning "Release defaults to UPX, but UPX is unavailable; continuing without executable compression."
        $Upx = $false
    } else {
        throw "UPX was requested but is unavailable: $UpxPath"
    }
}

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$effectiveVersion = if ([string]::IsNullOrWhiteSpace($Version)) { "0.1.0" } else { $Version }
$formalBuildDefaults = $OfficialBuild -or $env:GITHUB_ACTIONS -eq "true"
$defaultLoggingEnabled = if ($formalBuildDefaults) { "OFF" } else { "ON" }
$defaultTopMostEnabled = if ($formalBuildDefaults) { "ON" } else { "OFF" }
$bundleOptionalExecutables = if ($All) { "ON" } else { "OFF" }
$officialBuildEnabled = if ($OfficialBuild) { "ON" } else { "OFF" }
$compressEmbeddedAssets = if ($OfficialBuild) { "ON" } else { "OFF" }
$buildMarker = if ($OfficialBuild) { "none" } elseif ($All) { "DEBUG-All" } else { "DEBUG" }
$buildScope = if ($All) { "complete" } else { "minimal" }
$buildProfile = if ($OfficialBuild) {
    "official-$buildScope"
} elseif ($Test) {
    "tests-$buildScope"
} elseif ($All) {
    "complete"
} else {
    "minimal"
}

function Show-PackageHelp {
    $scriptName = Split-Path -Leaf $PSCommandPath
    @"
Quattro 中文构建与打包工具

用法：
  powershell -ExecutionPolicy Bypass -File .\tools\$scriptName [options]

默认行为：
  构建本地开发版 x64 最小包，使用 vcpkg 后端和 Release 配置。
  默认只打包 Quattro，不构建、不打包 AppLaunchLocker 和 QuattroUpdater。
  主窗口显示红色（DEBUG）标记，默认生成：
    dist\Quattro-x64.exe
    dist\Quattro-x64.zip
    dist\SHA256SUMS.txt
    dist\latest.json

参数：
  --help, -Help
      显示本帮助，不执行构建。

  --all, -All
      构建完整的 x86 和 x64 包，并包含 AppLaunchLocker 和 QuattroUpdater。
      本地主窗口显示红色（DEBUG-All）标记。

  -Platform x86|x64
      选择最小包的目标架构，默认 x64。与 -All 一起使用时由 -All 构建全部架构。

  -Configuration <名称>
      指定 CMake/MSBuild 配置，默认 Release。

  -Clean
      构建前删除本次所选构建目录，执行全新构建。日常增量打包不建议使用。

  -Test
      构建本地测试/辅助工具并在打包前运行相关单元测试，需要本地 tests/ 源码。
      此参数不会自动包含可选程序；测试 AppLaunchLocker 或 QuattroUpdater 时必须使用 -All -Test。

  -SkipTests
      兼容参数。默认本来就不运行测试，除非显式指定 -Test。

  -NoZip
      不生成 ZIP，只生成单文件 EXE、校验和及更新清单，可用于快速验收。

  -FullPackage
      在 EXE 旁保留外部资源目录，生成完整目录式包。

  -FlatPackage
      使用旧版默认的 x64 单文件平铺打包流程；不能与 -All、非 x64 或 -FullPackage 组合。

  -Upx
      打包前使用 UPX 压缩 Quattro.exe；启用后无法复用未变化的单文件产物。

  -NoUpx
      禁止 UPX 压缩。主要用于覆盖 -Release 默认启用的 UPX，对应 GitHub Actions 的 use_upx=false。

  -UpxPath <路径>
      指定 UPX 可执行文件路径，默认从 PATH 中查找 upx。

  -PlanOnly
      仅输出解析后的架构、构建身份、可选组件状态和构建目录，不编译、不打包。

  -OfficialBuild
      构建正式版，不显示 DEBUG 标记，主要供 GitHub Actions 正式发布使用。
      正式完整包通常组合使用 -OfficialBuild -All。
      正式版会压缩内置主题和图标；本地 DEBUG/DEBUG-All 直接嵌入原始资源以缩短构建时间。

  -Release
      使用与 GitHub Actions Package 步骤一致的正式打包参数：vcpkg、OfficialBuild、NoZip。
      未指定平台时默认构建 x86+x64 完整组件包；可用 -Platform x64 或 x86 构建单平台精简包。
      默认启用 UPX；未安装 UPX 时给出警告并继续。可使用 -NoUpx 关闭，对应 use_upx=false。

  -Version <版本号>
      指定编译和更新清单版本，例如 1.2.3；默认 0.1.0。

  -ReleaseRepoUrl <URL>
      指定 latest.json 使用的 GitHub 仓库地址。
      默认：https://github.com/codingriver/quattro

  -ReleaseTag <标签>
      指定 latest.json 使用的发布标签；默认 v<Version>。

  -Backend vcpkg|classic
      指定依赖构建后端，默认 vcpkg；classic 仅用于兼容旧构建流程。

常用示例：
  # 本地 x64 开发版最小包（不包含 AppLaunchLocker、QuattroUpdater）
  powershell -ExecutionPolicy Bypass -File .\tools\$scriptName

  # 本地完整包，包含 AppLaunchLocker、QuattroUpdater，构建 x86 和 x64
  powershell -ExecutionPolicy Bypass -File .\tools\$scriptName --all

  # 测试 AppLaunchLocker、QuattroUpdater 相关内容
  powershell -ExecutionPolicy Bypass -File .\tools\$scriptName -All -Test

  # 只运行 Quattro 最小构建范围的相关测试
  powershell -ExecutionPolicy Bypass -File .\tools\$scriptName -Test

  # 快速构建但不生成 ZIP
  powershell -ExecutionPolicy Bypass -File .\tools\$scriptName -NoZip

  # 查看将使用的构建目录和组件范围
  powershell -ExecutionPolicy Bypass -File .\tools\$scriptName -All -PlanOnly

  # 构建指定版本的 GitHub 正式完整包
  powershell -ExecutionPolicy Bypass -File .\tools\$scriptName -Release -Version 1.2.3

  # 构建与 GitHub Actions 单平台选项一致的 x64 正式精简包
  powershell -ExecutionPolicy Bypass -File .\tools\$scriptName -Release -Platform x64 -Version 1.2.3

  # 构建不使用 UPX 的正式包
  powershell -ExecutionPolicy Bypass -File .\tools\$scriptName -Release -NoUpx -Version 1.2.3

  # 仅构建 x86 最小包
  powershell -ExecutionPolicy Bypass -File .\tools\$scriptName -Platform x86
"@
}

if ($Help) {
    Show-PackageHelp
    return
}

function Format-PackageTimestamp {
    param([datetime]$Time)

    return $Time.ToString("yyyy-MM-dd HH:mm:ss")
}

function New-ArchitectureList {
    param([string]$Requested)

    $all = @(
        [pscustomobject]@{ Name = "x86"; CMakePlatform = "Win32"; BuildDir = "build-x86"; VcpkgTriplet = "x86-windows-static-md" },
        [pscustomobject]@{ Name = "x64"; CMakePlatform = "x64"; BuildDir = "build-x64"; VcpkgTriplet = "x64-windows-static-md" }
    )

    if ($Requested -eq "all") {
        return $all
    }
    return $all | Where-Object { $_.Name -eq $Requested }
}

function Get-BuildDirectoryName {
    param(
        [string]$Architecture,
        [string]$ClassicBuildDirectory,
        [bool]$UseVcpkg
    )

    $baseName = if ($UseVcpkg) { "build-vcpkg-$Architecture" } else { $ClassicBuildDirectory }
    if ($buildProfile -eq "minimal") {
        return $baseName
    }
    return "$baseName-$buildProfile"
}

function Invoke-NativeCommand {
    param(
        [string]$Description,
        [string]$Command,
        [string[]]$Arguments
    )

    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE."
    }
}

function Get-CMakeVisualStudioGenerator {
    $help = & cmake --help 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to query CMake generators."
    }

    $generators = @()
    foreach ($line in $help) {
        if ($line -match '^\s*\*?\s*(Visual Studio\s+(\d+)\s+(\d+))\s*=') {
            $generators += [pscustomobject]@{
                Name = $matches[1]
                Major = [int]$matches[2]
                Year = [int]$matches[3]
            }
        }
    }

    foreach ($generator in ($generators | Sort-Object Major -Descending)) {
        $roots = @(
            (Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\$($generator.Year)"),
            (Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\$($generator.Major)")
        )
        if (![string]::IsNullOrWhiteSpace(${env:ProgramFiles(x86)})) {
            $roots += @(
                (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\$($generator.Year)"),
                (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\$($generator.Major)")
            )
        }

        foreach ($vsRoot in $roots) {
            if (![string]::IsNullOrWhiteSpace($vsRoot) -and (Test-Path -LiteralPath $vsRoot)) {
                return $generator.Name
            }
        }
    }

    $available = ($generators | ForEach-Object { $_.Name }) -join ", "
    if ([string]::IsNullOrWhiteSpace($available)) {
        throw "No Visual Studio CMake generators were reported by CMake."
    }
    throw "No installed Visual Studio instance matched CMake generators: $available"
}

function Get-CMakeCacheValue {
    param(
        [string]$CacheText,
        [string]$Name
    )

    $match = [regex]::Match(
        $CacheText,
        "(?m)^" + [regex]::Escape($Name) + ":[^=]*=(.*)$")
    if ($match.Success) {
        return $match.Groups[1].Value.Trim()
    }
    return ""
}

function Ensure-Configured {
    param(
        [string]$BuildDir,
        [string]$CMakePlatform,
        [string]$VcpkgTriplet = "",
        [switch]$UseVcpkg,
        [switch]$BuildTests
    )

    $generator = Get-CMakeVisualStudioGenerator
    $cache = Join-Path $BuildDir "CMakeCache.txt"
    if (Test-Path $cache) {
        $cacheText = Get-Content -Path $cache -Raw
        $expectedSource = $root.Replace('\', '/')
        $expectedToolchain = "$expectedSource/.vcpkg-root/scripts/buildsystems/vcpkg.cmake"
        $incompatibleReasons = New-Object System.Collections.Generic.List[string]
        if ((Get-CMakeCacheValue -CacheText $cacheText -Name "CMAKE_GENERATOR") -ne $generator) {
            $incompatibleReasons.Add("generator") | Out-Null
        }
        if ((Get-CMakeCacheValue -CacheText $cacheText -Name "CMAKE_GENERATOR_PLATFORM") -ne $CMakePlatform) {
            $incompatibleReasons.Add("platform") | Out-Null
        }
        if ((Get-CMakeCacheValue -CacheText $cacheText -Name "CMAKE_HOME_DIRECTORY") -ne $expectedSource) {
            $incompatibleReasons.Add("source") | Out-Null
        }
        if ($UseVcpkg -and
            (Get-CMakeCacheValue -CacheText $cacheText -Name "CMAKE_TOOLCHAIN_FILE") -ne $expectedToolchain) {
            $incompatibleReasons.Add("toolchain") | Out-Null
        }
        if ($UseVcpkg -and
            (Get-CMakeCacheValue -CacheText $cacheText -Name "VCPKG_TARGET_TRIPLET") -ne $VcpkgTriplet) {
            $incompatibleReasons.Add("triplet") | Out-Null
        }
        if ($incompatibleReasons.Count -gt 0) {
            "recreating incompatible build directory $BuildDir`: $($incompatibleReasons -join ', ')"
            Remove-DirectoryRobust -Path $BuildDir
        } else {
            "reconfiguring compatible build directory in place: $BuildDir"
        }
    }

    $configureArgs = @("-S", $root, "-B", $BuildDir, "-G", $generator, "-A", $CMakePlatform)
    if ($UseVcpkg) {
        $toolchain = Join-Path $root ".vcpkg-root\scripts\buildsystems\vcpkg.cmake"
        if (!(Test-Path $toolchain)) {
            throw "vcpkg backend requested but toolchain was not found: $toolchain. Run .\.vcpkg-root\bootstrap-vcpkg.bat or use -Backend classic."
        }
        $configureArgs += @(
            "-DCMAKE_TOOLCHAIN_FILE=$($toolchain.Replace('\', '/'))",
            "-DVCPKG_TARGET_TRIPLET=$VcpkgTriplet"
        )
    }
    $configureArgs += "-DQUATTRO_BUILD_TESTS=$(if ($BuildTests) { 'ON' } else { 'OFF' })"
    $configureArgs += "-DQUATTRO_VERSION=$effectiveVersion"
    $configureArgs += "-DQUATTRO_DEFAULT_LOGGING_ENABLED=$defaultLoggingEnabled"
    $configureArgs += "-DQUATTRO_DEFAULT_TOP_MOST_ENABLED=$defaultTopMostEnabled"
    $configureArgs += "-DQUATTRO_BUNDLE_OPTIONAL_EXECUTABLES=$bundleOptionalExecutables"
    $configureArgs += "-DQUATTRO_OFFICIAL_BUILD=$officialBuildEnabled"
    $configureArgs += "-DQUATTRO_COMPRESS_EMBEDDED_ASSETS=$compressEmbeddedAssets"
    Invoke-NativeCommand -Description "CMake configure" -Command "cmake" -Arguments $configureArgs
}

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
            Remove-FileRobust -Path $file.FullName -Purpose "legacy build output"
        }
    }
}

function Remove-DirectoryRobust {
    param([string]$Path)

    if (!(Test-Path $Path)) {
        return
    }
    Get-ChildItem -LiteralPath $Path -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object {
        try {
            $_.Attributes = [System.IO.FileAttributes]::Normal
        } catch {
        }
    }
    Remove-Item -LiteralPath $Path -Recurse -Force
}

function Get-ProcessesByExecutablePath {
    param([string]$Path)

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object {
            ![string]::IsNullOrWhiteSpace($_.ExecutablePath) -and
            ([System.IO.Path]::GetFullPath($_.ExecutablePath) -ieq $fullPath)
        }
}

function Format-ProcessList {
    param($Processes)

    $items = @($Processes)
    if ($items.Count -eq 0) {
        return ""
    }

    return ($items | ForEach-Object { "$($_.Name) pid=$($_.ProcessId)" }) -join ", "
}

function Stop-ProcessesByExecutablePath {
    param(
        [string]$Path,
        [string]$Purpose = "file"
    )

    $processes = @(Get-ProcessesByExecutablePath -Path $Path)
    if ($processes.Count -eq 0) {
        return
    }

    $processList = Format-ProcessList -Processes $processes
    "stopping running $Purpose`: $processList"
    foreach ($process in $processes) {
        try {
            Stop-Process -Id $process.ProcessId -Force -ErrorAction Stop
        } catch {
            $remaining = @(Get-ProcessesByExecutablePath -Path $Path)
            if ($remaining.Count -gt 0) {
                throw "Cannot stop running $Purpose`: $Path ($(Format-ProcessList -Processes $remaining)). $($_.Exception.Message)"
            }
        }
    }

    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        $remaining = @(Get-ProcessesByExecutablePath -Path $Path)
        if ($remaining.Count -eq 0) {
            return
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)

    throw "Cannot stop running $Purpose`: $Path ($(Format-ProcessList -Processes $remaining))."
}

function Remove-FileRobust {
    param(
        [string]$Path,
        [string]$Purpose = "file"
    )

    if (!(Test-Path $Path)) {
        return
    }

    Stop-ProcessesByExecutablePath -Path $Path -Purpose $Purpose

    try {
        (Get-Item -LiteralPath $Path -Force).Attributes = [System.IO.FileAttributes]::Normal
    } catch {
    }

    $lastError = $null
    for ($attempt = 1; $attempt -le 5; $attempt++) {
        try {
            Remove-Item -LiteralPath $Path -Force
            return
        } catch {
            $lastError = $_
            Start-Sleep -Milliseconds (200 * $attempt)
        }
    }

    $processList = Format-ProcessList -Processes (Get-ProcessesByExecutablePath -Path $Path)
    if (![string]::IsNullOrWhiteSpace($processList)) {
        throw "Cannot replace $Purpose because it is still running: $Path ($processList)."
    }

    throw "Cannot remove $Purpose`: $Path. $($lastError.Exception.Message)"
}

function Invoke-PackageBuild {
    param(
        [string]$BuildDir,
        [switch]$IncludeTestTools
    )

    $targets = @("Quattro")
    if ($IncludeTestTools) {
        $targets += @("QuattroTests", "QuattroDbProbe", "QuattroDbSeed")
    }

    $buildArgs = @("--build", $BuildDir, "--config", $Configuration, "--target") + $targets + @("--", "/m")
    Invoke-NativeCommand -Description "CMake build" -Command "cmake" -Arguments $buildArgs
}

function Invoke-UpxCompress {
    param([string]$ExePath)

    if (!$Upx) {
        return
    }
    if (!(Test-Path $ExePath)) {
        throw "UPX target not found: $ExePath"
    }

    $before = (Get-Item -LiteralPath $ExePath).Length
    & $UpxPath "--best" "--lzma" $ExePath
    if ($LASTEXITCODE -ne 0) {
        throw "UPX failed for: $ExePath"
    }
    $after = (Get-Item -LiteralPath $ExePath).Length
    $saved = $before - $after
    $percent = if ($before -gt 0) { [math]::Round(($saved * 100.0) / $before, 1) } else { 0 }
    "upx: $ExePath ($before -> $after bytes, saved $percent%)"
}

function Test-FilesHaveSameContent {
    param(
        [string]$First,
        [string]$Second
    )

    if (!(Test-Path -LiteralPath $First) -or !(Test-Path -LiteralPath $Second)) {
        return $false
    }
    $firstItem = Get-Item -LiteralPath $First
    $secondItem = Get-Item -LiteralPath $Second
    if ($firstItem.Length -ne $secondItem.Length) {
        return $false
    }
    $firstHash = (Get-FileHash -LiteralPath $First -Algorithm SHA256).Hash
    $secondHash = (Get-FileHash -LiteralPath $Second -Algorithm SHA256).Hash
    return $firstHash -eq $secondHash
}

function ConvertTo-JsonStringLiteral {
    param([string]$Value)

    return ($Value | ConvertTo-Json -Compress)
}

function Write-ReleaseMetadata {
    param(
        [System.Collections.Generic.List[string]]$ReleaseFiles,
        [string]$OutputDir,
        [string]$Version,
        [string]$RepoUrl,
        [string]$Tag
    )

    if ($ReleaseFiles.Count -eq 0) {
        return @()
    }

    $normalizedRepoUrl = $RepoUrl.TrimEnd("/")
    $effectiveTag = if ([string]::IsNullOrWhiteSpace($Tag)) { "v$Version" } else { $Tag }
    $releaseUrl = "$normalizedRepoUrl/releases/tag/$effectiveTag"
    $downloadBaseUrl = "$normalizedRepoUrl/releases/download/$effectiveTag"
    $checksumPath = Join-Path $OutputDir "SHA256SUMS.txt"
    $manifestPath = Join-Path $OutputDir "latest.json"
    $checksumLines = New-Object System.Collections.Generic.List[string]
    $assetJsonItems = New-Object System.Collections.Generic.List[string]

    foreach ($file in $ReleaseFiles) {
        if (!(Test-Path -LiteralPath $file)) {
            continue
        }
        $item = Get-Item -LiteralPath $file
        $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
        $checksumLines.Add("$hash  $($item.Name)") | Out-Null
        $assetJsonItems.Add(
            "    {" +
            "`"name`": $(ConvertTo-JsonStringLiteral $item.Name), " +
            "`"url`": $(ConvertTo-JsonStringLiteral "$downloadBaseUrl/$($item.Name)"), " +
            "`"size`": $($item.Length), " +
            "`"sha256`": $(ConvertTo-JsonStringLiteral $hash)" +
            "}") | Out-Null
    }

    if ($assetJsonItems.Count -eq 0) {
        return @()
    }

    [System.IO.File]::WriteAllText($checksumPath, (($checksumLines -join "`r`n") + "`r`n"), [System.Text.UTF8Encoding]::new($false))

    $manifest = @(
        "{",
        "  `"version`": $(ConvertTo-JsonStringLiteral $Version),",
        "  `"releaseUrl`": $(ConvertTo-JsonStringLiteral $releaseUrl),",
        "  `"notes`": `"`",",
        "  `"checksumUrl`": $(ConvertTo-JsonStringLiteral "$downloadBaseUrl/SHA256SUMS.txt"),",
        "  `"assets`": [",
        ($assetJsonItems -join ",`r`n"),
        "  ]",
        "}"
    ) -join "`r`n"
    [System.IO.File]::WriteAllText($manifestPath, ($manifest + "`r`n"), [System.Text.UTF8Encoding]::new($false))

    return @($checksumPath, $manifestPath)
}

function Publish-Package {
    param(
        [string]$BuildDir,
        [string]$DistName,
        [string]$SingleExeDirectory = "",
        [string]$SingleExeFileName = "Quattro.exe",
        [switch]$SingleExe,
        [switch]$Zip
    )

    $source = Join-Path $BuildDir $Configuration
    $dist = Join-Path $distRoot $DistName

    Remove-LegacyBuildOutputs -OutputDir $source
    $sourceQuattro = Join-Path $source "Quattro.exe"
    if (!(Test-Path $sourceQuattro)) {
        Invoke-PackageBuild -BuildDir $BuildDir
        Remove-LegacyBuildOutputs -OutputDir $source
    }
    if (!(Test-Path $sourceQuattro)) {
        throw "Quattro executable not found after build: $sourceQuattro"
    }
    if ($SingleExe) {
        if ([string]::IsNullOrWhiteSpace($SingleExeDirectory)) {
            $singleExeDirectoryPath = $distRoot
        } elseif ([System.IO.Path]::IsPathRooted($SingleExeDirectory)) {
            $singleExeDirectoryPath = $SingleExeDirectory
        } else {
            $singleExeDirectoryPath = Join-Path $distRoot $SingleExeDirectory
        }
        New-Item -ItemType Directory -Force -Path $singleExeDirectoryPath | Out-Null
        $singleExePath = Join-Path $singleExeDirectoryPath $SingleExeFileName
        $reuseExisting = !$Upx -and (Test-FilesHaveSameContent -First $sourceQuattro -Second $singleExePath)
        if ($reuseExisting) {
            "single-exe unchanged: $singleExePath"
        } else {
            if (Test-Path $singleExePath) {
                Remove-FileRobust -Path $singleExePath -Purpose "single-exe package"
            }
            Copy-Item -LiteralPath $sourceQuattro -Destination $singleExePath -Force
            Invoke-UpxCompress -ExePath $singleExePath
            "single-exe: $singleExePath"
        }
    } else {
        if (Test-Path $dist) {
            Remove-DirectoryRobust -Path $dist
        }

        New-Item -ItemType Directory -Force -Path $dist | Out-Null
        Copy-Item -LiteralPath $sourceQuattro -Destination $dist
        Invoke-UpxCompress -ExePath (Join-Path $dist "Quattro.exe")
        Copy-Item -LiteralPath (Join-Path $source "db") -Destination $dist -Recurse -ErrorAction SilentlyContinue
        Copy-Item -LiteralPath (Join-Path $source "theme") -Destination $dist -Recurse -ErrorAction SilentlyContinue
        Copy-Item -LiteralPath (Join-Path $source "icons") -Destination $dist -Recurse -ErrorAction SilentlyContinue
        $packagedIcons = Join-Path $dist "icons"
        if (Test-Path $packagedIcons) {
            Get-ChildItem -LiteralPath $packagedIcons -Recurse -File |
                Where-Object { $_.Name -ieq "README.md" -or $_.Name -ieq "LICENSE" -or $_.Extension -ieq ".md" } |
                Remove-Item -Force
        }
    }

    $zipPath = Join-Path $distRoot "$DistName.zip"
    if ($Zip) {
        if (Test-Path $zipPath) {
            Remove-FileRobust -Path $zipPath -Purpose "zip package"
        }
        if ($SingleExe) {
            Compress-Archive -LiteralPath $singleExePath -DestinationPath $zipPath -Force
        } else {
            Compress-Archive -Path (Join-Path $dist "*") -DestinationPath $zipPath -Force
        }
        "archive: $zipPath"
    } elseif (Test-Path $zipPath) {
        Remove-FileRobust -Path $zipPath -Purpose "stale no-zip package"
        "removed stale archive: $zipPath"
    }

    if (!$SingleExe) {
        "packaged: $dist"
    }
}

$requestedPlatform = if ($All) { "all" } else { $Platform }
$architectures = @(New-ArchitectureList -Requested $requestedPlatform)
if (!$architectures -or $architectures.Count -eq 0) {
    throw "No platform selected."
}

if ($Test -and $SkipTests) {
    throw "-Test and -SkipTests cannot be used together."
}

if ($Test -and !(Test-Path (Join-Path $root "tests\UnitTests.cpp"))) {
    throw "-Test requires local tests/ sources, which are intentionally not part of the public repository."
}

if ($FlatPackage -and ($All -or $Platform -ne "x64" -or $FullPackage)) {
    throw "-FlatPackage can only be used with the default x64 single-exe package flow."
}

if (($architectures | Where-Object { $_.Name -eq "x64" }) -and -not [System.Environment]::Is64BitOperatingSystem) {
    throw "x64 package requires a 64-bit Windows host."
}

if ($PlanOnly) {
    "release=$(if ($Release) { 'ON' } else { 'OFF' })"
    "architectures=$((@($architectures | ForEach-Object { $_.Name }) -join ','))"
    "bundle_optional_executables=$bundleOptionalExecutables"
    "official_build=$officialBuildEnabled"
    "embedded_assets=$(if ($compressEmbeddedAssets -eq 'ON') { 'xpress' } else { 'raw' })"
    "configuration=$Configuration"
    "backend=$Backend"
    "no_zip=$(if ($NoZip) { 'ON' } else { 'OFF' })"
    "use_upx=$(if ($Upx) { 'ON' } else { 'OFF' })"
    "default_logging=$defaultLoggingEnabled"
    "default_top_most=$defaultTopMostEnabled"
    "build_marker=$buildMarker"
    "build_profile=$buildProfile"
    $useVcpkg = $Backend -eq "vcpkg"
    "build_directories=$((@($architectures | ForEach-Object { Get-BuildDirectoryName -Architecture $_.Name -ClassicBuildDirectory $_.BuildDir -UseVcpkg $useVcpkg }) -join ','))"
    return
}

$distRoot = Join-Path $root "dist"
New-Item -ItemType Directory -Force -Path $distRoot | Out-Null
Get-ChildItem -LiteralPath $distRoot -Filter "AppLaunchLocker*.exe" -File -ErrorAction SilentlyContinue | ForEach-Object {
    Remove-FileRobust -Path $_.FullName -Purpose "obsolete external AppLaunchLocker package"
}

$packageStartTime = Get-Date
"package date: $($packageStartTime.ToString("yyyy-MM-dd"))"
"package start: $(Format-PackageTimestamp -Time $packageStartTime)"

$outputs = New-Object System.Collections.Generic.List[string]
$releaseFiles = New-Object System.Collections.Generic.List[string]
foreach ($arch in $architectures) {
    $useVcpkg = $Backend -eq "vcpkg"
    $buildDirName = Get-BuildDirectoryName -Architecture $arch.Name -ClassicBuildDirectory $arch.BuildDir -UseVcpkg $useVcpkg
    $buildDir = Join-Path $root $buildDirName
    if ($Clean -and (Test-Path $buildDir)) {
        Remove-DirectoryRobust -Path $buildDir
    }

    Remove-LegacyBuildOutputs -OutputDir (Join-Path $buildDir $Configuration)
    Ensure-Configured -BuildDir $buildDir -CMakePlatform $arch.CMakePlatform -VcpkgTriplet $arch.VcpkgTriplet -UseVcpkg:$useVcpkg -BuildTests:$Test
    Invoke-PackageBuild -BuildDir $buildDir -IncludeTestTools:$Test
    Remove-LegacyBuildOutputs -OutputDir (Join-Path $buildDir $Configuration)

    if ($Test) {
        $testExe = Join-Path $buildDir "$Configuration\QuattroTests.exe"
        if (!(Test-Path $testExe)) {
            throw "Unit test executable not found: $testExe"
        }
        & $testExe
    }

    $flatSingleExeX64 = $FlatPackage -and !$All -and $architectures.Count -eq 1 -and $arch.Name -eq "x64"
    $distName = if ($flatSingleExeX64) { "Quattro" } else { "Quattro-$($arch.Name)" }

    $packageArgs = @{
        BuildDir = $buildDir
        DistName = $distName
    }
    if (!$FullPackage) {
        $packageArgs.SingleExe = $true
        if (!$flatSingleExeX64) {
            $packageArgs.SingleExeFileName = "$distName.exe"
        }
    }
    if (!$NoZip) {
        $packageArgs.Zip = $true
    }
    Publish-Package @packageArgs

    if (!$FullPackage) {
        $singleExeOutput = if ($flatSingleExeX64) {
            Join-Path $distRoot "Quattro.exe"
        } else {
            Join-Path $distRoot "$distName.exe"
        }
        $outputs.Add($singleExeOutput) | Out-Null
        if (Test-Path $singleExeOutput) {
            $releaseFiles.Add($singleExeOutput) | Out-Null
        }
    } else {
        $outputs.Add((Join-Path $distRoot $distName)) | Out-Null
    }
    if (!$NoZip) {
        $outputs.Add((Join-Path $distRoot "$distName.zip")) | Out-Null
    }
}

$metadataOutputs = Write-ReleaseMetadata -ReleaseFiles $releaseFiles -OutputDir $distRoot -Version $effectiveVersion -RepoUrl $ReleaseRepoUrl -Tag $ReleaseTag
foreach ($metadataOutput in $metadataOutputs) {
    $outputs.Add($metadataOutput) | Out-Null
}

$packageEndTime = Get-Date
"package end: $(Format-PackageTimestamp -Time $packageEndTime)"
"package duration: $([int][math]::Round(($packageEndTime - $packageStartTime).TotalSeconds))s"
"package complete"
"artifacts:"
foreach ($output in $outputs) {
    if (Test-Path $output) {
        (Resolve-Path -LiteralPath $output).Path
    }
}
