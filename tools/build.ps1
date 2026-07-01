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
    [ValidateSet("vcpkg", "classic")]
    [string]$Backend = "vcpkg",
    [switch]$Help
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

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
  -Test                    Build test/helper tools and run unit tests before packaging.
  -SkipTests               Compatibility no-op; tests are skipped unless -Test is specified.
  -NoZip                   Create package directories only, without zip archives.
  -FullPackage             Include external resource folders beside the exe.
  -FlatPackage             Run the previous default flat x64 single-exe package flow.
  -Upx                     Compress packaged Quattro.exe with UPX before zipping.
  -UpxPath <path>          UPX executable path. Default: upx.
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
        [switch]$UseVcpkg
    )

    $cache = Join-Path $BuildDir "CMakeCache.txt"
    if (Test-Path $cache) {
        $cacheText = Get-Content -Path $cache -Raw
        $expected = "CMAKE_GENERATOR_PLATFORM:INTERNAL=$CMakePlatform"
        $expectedSource = "CMAKE_HOME_DIRECTORY:INTERNAL=$($root.Replace('\', '/'))"
        $expectedToolchain = "CMAKE_TOOLCHAIN_FILE:FILEPATH=$($root.Replace('\', '/'))/.vcpkg-root/scripts/buildsystems/vcpkg.cmake"
        $expectedTriplet = "VCPKG_TARGET_TRIPLET:STRING=$VcpkgTriplet"
        if ($cacheText -notmatch [regex]::Escape($expected) -or
            $cacheText -notmatch [regex]::Escape($expectedSource) -or
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
            Remove-Item -LiteralPath $file.FullName -Force
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
            Remove-Item -LiteralPath $singleExePath -Force
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
        Copy-Item -LiteralPath (Join-Path $root "README.md") -Destination $dist -ErrorAction SilentlyContinue
        Copy-Item -LiteralPath (Join-Path $root "docs") -Destination $dist -Recurse -ErrorAction SilentlyContinue
    }

    if ($Zip) {
        $zipPath = Join-Path $distRoot "$DistName.zip"
        if (Test-Path $zipPath) {
            Remove-Item -LiteralPath $zipPath -Force
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
foreach ($arch in $architectures) {
    $useVcpkg = $Backend -eq "vcpkg"
    $buildDirName = if ($useVcpkg) { "build-vcpkg-$($arch.Name)" } else { $arch.BuildDir }
    $buildDir = Join-Path $root $buildDirName
    if ($Clean -and (Test-Path $buildDir)) {
        Remove-DirectoryRobust -Path $buildDir
    }

    Remove-LegacyBuildOutputs -OutputDir (Join-Path $buildDir $Configuration)
    Ensure-Configured -BuildDir $buildDir -CMakePlatform $arch.CMakePlatform -VcpkgTriplet $arch.VcpkgTriplet -UseVcpkg:$useVcpkg
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
    } else {
        $outputs.Add((Join-Path $distRoot $distName)) | Out-Null
    }
    if (!$NoZip) {
        $outputs.Add((Join-Path $distRoot "$distName.zip")) | Out-Null
    }
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
