param(
    [string]$BuildRoot = "build-identity-acceptance"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$resolvedRoot = [System.IO.Path]::GetFullPath($root).TrimEnd('\')
$resolvedBuildRoot = [System.IO.Path]::GetFullPath((Join-Path $root $BuildRoot))
if (!$resolvedBuildRoot.StartsWith($resolvedRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "BuildRoot must stay inside the repository: $resolvedBuildRoot"
}

function Invoke-Native {
    param([string]$Description, [string]$Command, [string[]]$Arguments)
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE."
    }
}

function Assert-Plan {
    param(
        [string[]]$Arguments,
        [string]$ExpectedOfficial,
        [string]$ExpectedMarker,
        [string]$ExpectedProfile,
        [string]$ExpectedDirectories)
    $parameters = @{ PlanOnly = $true }
    if ($Arguments -contains "-All") { $parameters.All = $true }
    if ($Arguments -contains "-OfficialBuild") { $parameters.OfficialBuild = $true }
    $lines = @(& (Join-Path $PSScriptRoot "build.ps1") @parameters)
    if ($lines -notcontains "official_build=$ExpectedOfficial" -or
        $lines -notcontains "build_marker=$ExpectedMarker" -or
        $lines -notcontains "build_profile=$ExpectedProfile" -or
        $lines -notcontains "build_directories=$ExpectedDirectories") {
        throw "Unexpected build plan: $($lines -join '; ')"
    }
}

function Assert-GeneratedIdentity {
    param(
        [string]$Name,
        [bool]$BundleOptionalExecutables,
        [bool]$OfficialBuild,
        [string]$ExpectedMarker,
        [int]$ExpectedOfficial
    )

    $buildDirectory = Join-Path $resolvedBuildRoot $Name
    if (Test-Path -LiteralPath $buildDirectory) {
        Remove-Item -LiteralPath $buildDirectory -Recurse -Force
    }
    $bundleValue = if ($BundleOptionalExecutables) { "ON" } else { "OFF" }
    $officialValue = if ($OfficialBuild) { "ON" } else { "OFF" }
    Invoke-Native -Description "$Name configure" -Command "cmake" -Arguments @(
        "-S", $root,
        "-B", $buildDirectory,
        "-DQUATTRO_BUILD_TESTS=OFF",
        "-DQUATTRO_BUNDLE_OPTIONAL_EXECUTABLES=$bundleValue",
        "-DQUATTRO_OFFICIAL_BUILD=$officialValue"
    )
    $header = Get-Content -LiteralPath (Join-Path $buildDirectory "generated\Version.h") -Raw
    if ($header -notmatch [regex]::Escape("#define QUATTRO_BUILD_MARKER_TEXT L`"$ExpectedMarker`"") -or
        $header -notmatch [regex]::Escape("#define QUATTRO_OFFICIAL_BUILD $ExpectedOfficial")) {
        throw "$Name generated an unexpected build identity."
    }
}

New-Item -ItemType Directory -Force -Path $resolvedBuildRoot | Out-Null
Assert-Plan -Arguments @() -ExpectedOfficial "OFF" -ExpectedMarker "DEBUG" `
    -ExpectedProfile "minimal" -ExpectedDirectories "build-vcpkg-x64"
Assert-Plan -Arguments @("-All") -ExpectedOfficial "OFF" -ExpectedMarker "DEBUG-All" `
    -ExpectedProfile "complete" -ExpectedDirectories "build-vcpkg-x86-complete,build-vcpkg-x64-complete"
Assert-Plan -Arguments @("-All", "-OfficialBuild") -ExpectedOfficial "ON" -ExpectedMarker "none" `
    -ExpectedProfile "official-complete" -ExpectedDirectories "build-vcpkg-x86-official-complete,build-vcpkg-x64-official-complete"
Assert-GeneratedIdentity -Name "debug" -BundleOptionalExecutables $false -OfficialBuild $false -ExpectedMarker "DEBUG" -ExpectedOfficial 0
Assert-GeneratedIdentity -Name "debug-all" -BundleOptionalExecutables $true -OfficialBuild $false -ExpectedMarker "DEBUG-All" -ExpectedOfficial 0
Assert-GeneratedIdentity -Name "official" -BundleOptionalExecutables $true -OfficialBuild $true -ExpectedMarker "" -ExpectedOfficial 1
"build identity acceptance passed"
