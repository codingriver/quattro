param(
    [ValidateSet("all", "x86", "x64")]
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
    [string]$UpxPath = "upx",
    [string]$Version = "",
    [string]$ReleaseRepoUrl = "https://github.com/codingriver/quattro",
    [string]$ReleaseTag = "",
    [ValidateSet("vcpkg", "classic")]
    [string]$Backend = "vcpkg",
    [switch]$Help
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$effectiveVersion = if ([string]::IsNullOrWhiteSpace($Version)) { "0.1.0" } else { $Version }
$defaultLoggingEnabled = if ($env:GITHUB_ACTIONS -eq "true") { "OFF" } else { "ON" }

function Show-PackageHelp {
    $scriptName = Split-Path -Leaf $PSCommandPath
    @"
Quattro package tool

Usage:
  powershell -ExecutionPolicy Bypass -File .\tools\$scriptName [options]

Defaults:
  Builds and packages x64 Quattro with the vcpkg backend, creates dist\Quattro-x64.exe and Quattro-x64.zip.

Options:
  --all, -All              Build both x86 and x64 packages.
  --help, -Help            Show this help text.
  -Platform x86|x64        Build a single platform. Default: x64.
  -Configuration <name>    Build configuration. Default: Release.
  -Clean                   Remove the selected build directory before configure.
  -Test                    Build local test/helper tools and run unit tests before packaging. Requires local tests/ sources.
  -SkipTests               Compatibility no-op; tests are skipped unless -Test is specified.
  -NoZip                   Create package directories only, without zip archives.
  -FullPackage             Include external resource folders beside the exe.
  -FlatPackage             Run the previous default flat x64 single-exe package flow.
  -Upx                     Compress packaged Quattro.exe with UPX before zipping.
  -UpxPath <path>          UPX executable path. Default: upx.
  -Version <semver>        Override the compiled app version, for example 1.2.3. Default: 0.1.0.
  -ReleaseRepoUrl <url>    GitHub repository URL used in latest.json. Default: https://github.com/codingriver/quattro.
  -ReleaseTag <tag>        Release tag used in latest.json. Default: v<Version>.
  -Backend vcpkg|classic   Build backend. Default: vcpkg. Use classic for the legacy non-vcpkg build.

Examples:
  powershell -ExecutionPolicy Bypass -File .\tools\$scriptName
  powershell -ExecutionPolicy Bypass -File .\tools\$scriptName -Test
  powershell -ExecutionPolicy Bypass -File .\tools\$scriptName -FlatPackage
  powershell -ExecutionPolicy Bypass -File .\tools\$scriptName --all
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

function Ensure-Configured {
    param(
        [string]$BuildDir,
        [string]$CMakePlatform,
        [string]$VcpkgTriplet = "",
        [switch]$UseVcpkg,
        [switch]$BuildTests
    )

    $cache = Join-Path $BuildDir "CMakeCache.txt"
    if (Test-Path $cache) {
        $cacheText = Get-Content -Path $cache -Raw
        $expected = "CMAKE_GENERATOR_PLATFORM:INTERNAL=$CMakePlatform"
        $expectedSource = "CMAKE_HOME_DIRECTORY:INTERNAL=$($root.Replace('\', '/'))"
        $expectedToolchain = "CMAKE_TOOLCHAIN_FILE:FILEPATH=$($root.Replace('\', '/'))/.vcpkg-root/scripts/buildsystems/vcpkg.cmake"
        $expectedTriplet = "VCPKG_TARGET_TRIPLET:STRING=$VcpkgTriplet"
        $expectedTestOption = "QUATTRO_BUILD_TESTS:BOOL=ON"
        $expectedVersion = "QUATTRO_VERSION:STRING=$effectiveVersion"
        $expectedLogging = "QUATTRO_DEFAULT_LOGGING_ENABLED:BOOL=$defaultLoggingEnabled"
        if ($cacheText -notmatch [regex]::Escape($expected) -or
            $cacheText -notmatch [regex]::Escape($expectedSource) -or
            $cacheText -notmatch [regex]::Escape($expectedVersion) -or
            $cacheText -notmatch [regex]::Escape($expectedLogging) -or
            ($BuildTests -and $cacheText -notmatch [regex]::Escape($expectedTestOption)) -or
            ($UseVcpkg -and ($cacheText -notmatch [regex]::Escape($expectedToolchain) -or
                             $cacheText -notmatch [regex]::Escape($expectedTriplet)))) {
            Remove-DirectoryRobust -Path $BuildDir
        }
    }

    if (!(Test-Path $cache)) {
        $configureArgs = @("-S", $root, "-B", $BuildDir, "-G", "Visual Studio 17 2022", "-A", $CMakePlatform)
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
        if ($BuildTests) {
            $configureArgs += "-DQUATTRO_BUILD_TESTS=ON"
        }
        $configureArgs += "-DQUATTRO_VERSION=$effectiveVersion"
        $configureArgs += "-DQUATTRO_DEFAULT_LOGGING_ENABLED=$defaultLoggingEnabled"
        & cmake @configureArgs
    }
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

function Remove-FileRobust {
    param(
        [string]$Path,
        [string]$Purpose = "file"
    )

    if (!(Test-Path $Path)) {
        return
    }

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
        throw "Cannot replace $Purpose because it is running: $Path ($processList). Close Quattro from the tray or stop these processes, then run the package command again."
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
    & cmake @buildArgs
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
    if (!(Test-Path (Join-Path $source "Quattro.exe"))) {
        Invoke-PackageBuild -BuildDir $BuildDir
        Remove-LegacyBuildOutputs -OutputDir $source
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
        if (Test-Path $singleExePath) {
            Remove-FileRobust -Path $singleExePath -Purpose "single-exe package"
        }
        Copy-Item -LiteralPath (Join-Path $source "Quattro.exe") -Destination $singleExePath -Force
        Invoke-UpxCompress -ExePath $singleExePath
        "single-exe: $singleExePath"
    } else {
        if (Test-Path $dist) {
            Remove-DirectoryRobust -Path $dist
        }

        New-Item -ItemType Directory -Force -Path $dist | Out-Null
        Copy-Item -LiteralPath (Join-Path $source "Quattro.exe") -Destination $dist
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

    if ($Zip) {
        $zipPath = Join-Path $distRoot "$DistName.zip"
        if (Test-Path $zipPath) {
            Remove-FileRobust -Path $zipPath -Purpose "zip package"
        }
        if ($SingleExe) {
            Compress-Archive -LiteralPath $singleExePath -DestinationPath $zipPath -Force
        } else {
            Compress-Archive -Path (Join-Path $dist "*") -DestinationPath $zipPath -Force
        }
        "archive: $zipPath"
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

$distRoot = Join-Path $root "dist"
New-Item -ItemType Directory -Force -Path $distRoot | Out-Null

$packageStartTime = Get-Date
"package date: $($packageStartTime.ToString("yyyy-MM-dd"))"
"package start: $(Format-PackageTimestamp -Time $packageStartTime)"

$outputs = New-Object System.Collections.Generic.List[string]
$releaseFiles = New-Object System.Collections.Generic.List[string]
foreach ($arch in $architectures) {
    $useVcpkg = $Backend -eq "vcpkg"
    $buildDirName = if ($useVcpkg) { "build-vcpkg-$($arch.Name)" } else { $arch.BuildDir }
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
