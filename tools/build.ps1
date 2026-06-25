param(
    [ValidateSet("all", "x86", "x64")]
    [string]$Platform = "x64",
    [switch]$All,
    [string]$Configuration = "Release",
    [switch]$Clean,
    [switch]$SkipTests,
    [switch]$NoZip,
    [switch]$FullPackage,
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
  Builds x64 only, runs unit tests, creates Quattro.exe and a zip archive.

Options:
  --all, -All              Build both x86 and x64 packages.
  --help, -Help            Show this help text.
  -Platform x86|x64        Build a single platform. Default: x64.
  -Configuration <name>    Build configuration. Default: Release.
  -Clean                   Remove the selected build directory before configure.
  -SkipTests               Skip unit tests before packaging.
  -NoZip                   Create package directories only, without zip archives.
  -FullPackage             Include external resource folders beside the exe.

Examples:
  powershell -ExecutionPolicy Bypass -File .\tools\$scriptName
  powershell -ExecutionPolicy Bypass -File .\tools\$scriptName --all
  powershell -ExecutionPolicy Bypass -File .\tools\$scriptName -Platform x86
"@
}

if ($Help) {
    Show-PackageHelp
    return
}

function New-ArchitectureList {
    param([string]$Requested)

    $all = @(
        [pscustomobject]@{ Name = "x86"; CMakePlatform = "Win32"; BuildDir = "build-x86" },
        [pscustomobject]@{ Name = "x64"; CMakePlatform = "x64"; BuildDir = "build-x64" }
    )

    if ($Requested -eq "all") {
        return $all
    }
    return $all | Where-Object { $_.Name -eq $Requested }
}

function Ensure-Configured {
    param(
        [string]$BuildDir,
        [string]$CMakePlatform
    )

    $cache = Join-Path $BuildDir "CMakeCache.txt"
    if (Test-Path $cache) {
        $cacheText = Get-Content -Path $cache -Raw
        $expected = "CMAKE_GENERATOR_PLATFORM:INTERNAL=$CMakePlatform"
        $expectedSource = "CMAKE_HOME_DIRECTORY:INTERNAL=$($root.Replace('\', '/'))"
        if ($cacheText -notmatch [regex]::Escape($expected) -or
            $cacheText -notmatch [regex]::Escape($expectedSource)) {
            Remove-Item -LiteralPath $BuildDir -Recurse -Force
        }
    }

    if (!(Test-Path $cache)) {
        cmake -S $root -B $BuildDir -G "Visual Studio 17 2022" -A $CMakePlatform
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

function Publish-Package {
    param(
        [string]$BuildDir,
        [string]$DistName,
        [string]$SingleExeDirectory = "",
        [switch]$SingleExe,
        [switch]$Zip
    )

    $source = Join-Path $BuildDir $Configuration
    $dist = Join-Path $distRoot $DistName

    Remove-LegacyBuildOutputs -OutputDir $source
    if (!(Test-Path (Join-Path $source "Quattro.exe"))) {
        cmake --build $BuildDir --config $Configuration -- /m
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
        $singleExePath = Join-Path $singleExeDirectoryPath "Quattro.exe"
        Copy-Item -LiteralPath (Join-Path $source "Quattro.exe") -Destination $singleExePath -Force
        "single-exe: $singleExePath"
    } else {
        if (Test-Path $dist) {
            Remove-Item -LiteralPath $dist -Recurse -Force
        }

        New-Item -ItemType Directory -Force -Path $dist | Out-Null
        Copy-Item -LiteralPath (Join-Path $source "Quattro.exe") -Destination $dist
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

if (($architectures | Where-Object { $_.Name -eq "x64" }) -and -not [System.Environment]::Is64BitOperatingSystem) {
    throw "x64 package requires a 64-bit Windows host."
}

$distRoot = Join-Path $root "dist"
if (Test-Path $distRoot) {
    Remove-Item -LiteralPath $distRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $distRoot | Out-Null

$outputs = New-Object System.Collections.Generic.List[string]
foreach ($arch in $architectures) {
    $buildDir = Join-Path $root $arch.BuildDir
    if ($Clean -and (Test-Path $buildDir)) {
        Remove-Item -LiteralPath $buildDir -Recurse -Force
    }

    Remove-LegacyBuildOutputs -OutputDir (Join-Path $buildDir $Configuration)
    Ensure-Configured -BuildDir $buildDir -CMakePlatform $arch.CMakePlatform
    cmake --build $buildDir --config $Configuration -- /m
    Remove-LegacyBuildOutputs -OutputDir (Join-Path $buildDir $Configuration)

    if (!$SkipTests) {
        $testExe = Join-Path $buildDir "$Configuration\QuattroTests.exe"
        if (!(Test-Path $testExe)) {
            throw "Unit test executable not found: $testExe"
        }
        & $testExe
    }

    $singleDefaultX64 = !$All -and $architectures.Count -eq 1 -and $arch.Name -eq "x64"
    $distName = if ($singleDefaultX64) { "Quattro" } else { "Quattro-$($arch.Name)" }

    $packageArgs = @{
        BuildDir = $buildDir
        DistName = $distName
    }
    if (!$FullPackage) {
        $packageArgs.SingleExe = $true
        if (!$singleDefaultX64) {
            $packageArgs.SingleExeDirectory = $arch.Name
        }
    }
    if (!$NoZip) {
        $packageArgs.Zip = $true
    }
    Publish-Package @packageArgs

    if (!$FullPackage) {
        $singleExeOutput = if ($singleDefaultX64) {
            Join-Path $distRoot "Quattro.exe"
        } else {
            Join-Path (Join-Path $distRoot $arch.Name) "Quattro.exe"
        }
        $outputs.Add($singleExeOutput) | Out-Null
    } else {
        $outputs.Add((Join-Path $distRoot $distName)) | Out-Null
    }
    if (!$NoZip) {
        $outputs.Add((Join-Path $distRoot "$distName.zip")) | Out-Null
    }
}

"package complete"
"artifacts:"
foreach ($output in $outputs) {
    if (Test-Path $output) {
        (Resolve-Path -LiteralPath $output).Path
    }
}
