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
        [string]$ExpectedEmbeddedAssets,
        [string]$ExpectedProfile,
        [string]$ExpectedDirectories,
        [string]$ExpectedSubsetTablerFont = "OFF")
    $parameters = @{ PlanOnly = $true }
    if ($Arguments -contains "-All") { $parameters.All = $true }
    if ($Arguments -contains "-Complete") { $parameters.Complete = $true }
    if ($Arguments -contains "-Minimal") { $parameters.Minimal = $true }
    if ($Arguments -contains "-OfficialBuild") { $parameters.OfficialBuild = $true }
    if ($Arguments -contains "-CompressEmbeddedAssets") { $parameters.CompressEmbeddedAssets = $true }
    if ($Arguments -contains "-SubsetTablerFont") { $parameters.SubsetTablerFont = $true }
    $platformIndex = [Array]::IndexOf($Arguments, "-Platform")
    if ($platformIndex -ge 0 -and $platformIndex + 1 -lt $Arguments.Count) {
        $parameters.Platform = $Arguments[$platformIndex + 1]
    }
    $lines = @(& (Join-Path $PSScriptRoot "build.ps1") @parameters)
    if ($lines -notcontains "official_build=$ExpectedOfficial" -or
        $lines -notcontains "build_marker=$ExpectedMarker" -or
        $lines -notcontains "embedded_assets=$ExpectedEmbeddedAssets" -or
        $lines -notcontains "subset_tabler_font=$ExpectedSubsetTablerFont" -or
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
        [bool]$CompressEmbeddedAssets,
        [string]$ExpectedMarker,
        [int]$ExpectedOfficial,
        [int]$ExpectedCompressedAssets
    )

    $buildDirectory = Join-Path $resolvedBuildRoot $Name
    if (Test-Path -LiteralPath $buildDirectory) {
        Remove-Item -LiteralPath $buildDirectory -Recurse -Force
    }
    $bundleValue = if ($BundleOptionalExecutables) { "ON" } else { "OFF" }
    $officialValue = if ($OfficialBuild) { "ON" } else { "OFF" }
    $compressedAssetsValue = if ($CompressEmbeddedAssets) { "ON" } else { "OFF" }
    Invoke-Native -Description "$Name configure" -Command "cmake" -Arguments @(
        "-S", $root,
        "-B", $buildDirectory,
        "-DQUATTRO_BUILD_TESTS=OFF",
        "-DQUATTRO_BUNDLE_OPTIONAL_EXECUTABLES=$bundleValue",
        "-DQUATTRO_OFFICIAL_BUILD=$officialValue",
        "-DQUATTRO_COMPRESS_EMBEDDED_ASSETS=$compressedAssetsValue"
    )
    $header = Get-Content -LiteralPath (Join-Path $buildDirectory "generated\Version.h") -Raw
    if ($header -notmatch [regex]::Escape("#define QUATTRO_BUILD_MARKER_TEXT L`"$ExpectedMarker`"") -or
        $header -notmatch [regex]::Escape("#define QUATTRO_OFFICIAL_BUILD $ExpectedOfficial") -or
        $header -notmatch [regex]::Escape("#define QUATTRO_COMPRESSED_EMBEDDED_ASSETS $ExpectedCompressedAssets")) {
        throw "$Name generated an unexpected build identity."
    }
}

function Assert-ReleasePlan {
    $buildScript = Join-Path $PSScriptRoot "build.ps1"
    $lines = @(& $buildScript -Release -PlanOnly)
    $expected = @(
        "release=ON",
        "architectures=x86,x64",
        "bundle_optional_executables=ON",
        "official_build=ON",
        "embedded_assets=raw",
        "subset_tabler_font=ON",
        "configuration=Release",
        "backend=vcpkg",
        "no_zip=ON",
        "use_upx=ON",
        "default_logging=OFF",
        "default_top_most=ON",
        "build_marker=none",
        "build_profile=official-complete"
    )
    foreach ($value in $expected) {
        if ($lines -notcontains $value) {
            throw "Release plan is missing: $value. Actual: $($lines -join '; ')"
        }
    }

    $singlePlatform = @(& $buildScript -Release -Platform x64 -Configuration Debug -PlanOnly)
    if ($singlePlatform -notcontains "architectures=x64" -or
        $singlePlatform -notcontains "bundle_optional_executables=OFF" -or
        $singlePlatform -notcontains "configuration=Debug" -or
        $singlePlatform -notcontains "build_profile=official-minimal") {
        throw "Release single-platform plan is unexpected: $($singlePlatform -join '; ')"
    }

    $singlePlatformMinimal = @(& $buildScript -Release -Platform x64 -Minimal -PlanOnly)
    if ($singlePlatformMinimal -notcontains "architectures=x64" -or
        $singlePlatformMinimal -notcontains "bundle_optional_executables=OFF" -or
        $singlePlatformMinimal -notcontains "build_profile=official-minimal") {
        throw "Release single-platform minimal plan is unexpected: $($singlePlatformMinimal -join '; ')"
    }

    $singlePlatformComplete = @(& $buildScript -Release -Platform x64 -Complete -PlanOnly)
    if ($singlePlatformComplete -notcontains "architectures=x64" -or
        $singlePlatformComplete -notcontains "bundle_optional_executables=ON" -or
        $singlePlatformComplete -notcontains "build_profile=official-complete" -or
        $singlePlatformComplete -notcontains "build_directories=build-vcpkg-x64-official-complete") {
        throw "Release single-platform complete plan is unexpected: $($singlePlatformComplete -join '; ')"
    }

    $withoutUpx = @(& $buildScript -Release -NoUpx -PlanOnly)
    if ($withoutUpx -notcontains "use_upx=OFF") {
        throw "Release -NoUpx did not disable executable compression: $($withoutUpx -join '; ')"
    }

    $withCompressedAssets = @(& $buildScript -Release -CompressEmbeddedAssets -PlanOnly)
    if ($withCompressedAssets -notcontains "embedded_assets=xpress") {
        throw "Release -CompressEmbeddedAssets did not enable resource compression: $($withCompressedAssets -join '; ')"
    }

    try {
        & $buildScript -Release -Backend classic -PlanOnly | Out-Null
        throw "Release unexpectedly accepted the classic backend."
    } catch {
        if ($_.Exception.Message -notmatch "same vcpkg backend as GitHub Actions") {
            throw
        }
    }

    $workflow = Get-Content -LiteralPath (Join-Path $root ".github\workflows\package-release.yml") -Raw
    if ($workflow -notmatch '"-Release"' -or
        $workflow -notmatch '"-NoUpx"' -or
        $workflow -notmatch '"-CompressEmbeddedAssets"' -or
        $workflow -notmatch '(?s)compress_embedded_assets:.*?default:\s*false' -or
        $workflow -match '"-OfficialBuild"' -or
        $workflow -match '"-Backend", "vcpkg"') {
        throw "GitHub Actions Package step must use the shared -Release entry point."
    }

    $manifest = Get-Content -LiteralPath (Join-Path $root "vcpkg.json") -Raw | ConvertFrom-Json
    $expectedVcpkgCommit = [string]$manifest.'builtin-baseline'
    if ($expectedVcpkgCommit -notmatch '^[0-9a-f]{40}$') {
        throw "vcpkg.json must contain a full 40-character builtin-baseline commit."
    }
    if ($workflow -notmatch "VCPKG_COMMIT:\s*$expectedVcpkgCommit" -or
        $workflow -notmatch 'fetch --depth 1 origin "\$env:VCPKG_COMMIT"' -or
        $workflow -notmatch 'checkout --detach FETCH_HEAD') {
        throw "GitHub Actions must fetch and check out the same vcpkg commit as builtin-baseline."
    }
    $vcpkgCacheStep = [regex]::Match(
        $workflow,
        '(?s)- name: Cache vcpkg packages.*?(?=\r?\n\s{6}- name:)').Value
    if ([string]::IsNullOrWhiteSpace($vcpkgCacheStep) -or
        $vcpkgCacheStep -match '(?m)^\s*\.vcpkg-root\s*$' -or
        $vcpkgCacheStep -notmatch '\$\{\{ env\.VCPKG_COMMIT \}\}') {
        throw "The vcpkg cache must exclude the tool checkout and include the pinned commit in its key."
    }
    $httpDependency = @($manifest.dependencies) |
        Where-Object { $_.name -eq 'cpp-httplib' } |
        Select-Object -First 1
    if (!$httpDependency -or $httpDependency.'default-features' -ne $false) {
        throw "cpp-httplib default features must remain disabled to avoid an unused Brotli dependency."
    }
}

New-Item -ItemType Directory -Force -Path $resolvedBuildRoot | Out-Null
Assert-Plan -Arguments @() -ExpectedOfficial "OFF" -ExpectedMarker "DEBUG-All" `
    -ExpectedEmbeddedAssets "raw" `
    -ExpectedProfile "complete" -ExpectedDirectories "build-vcpkg-x64-complete"
Assert-Plan -Arguments @("-Complete") -ExpectedOfficial "OFF" -ExpectedMarker "DEBUG-All" `
    -ExpectedEmbeddedAssets "raw" `
    -ExpectedProfile "complete" -ExpectedDirectories "build-vcpkg-x64-complete"
Assert-Plan -Arguments @("-Platform", "x86", "-Complete") -ExpectedOfficial "OFF" -ExpectedMarker "DEBUG-All" `
    -ExpectedEmbeddedAssets "raw" `
    -ExpectedProfile "complete" -ExpectedDirectories "build-vcpkg-x86-complete"
Assert-Plan -Arguments @("-Minimal") -ExpectedOfficial "OFF" -ExpectedMarker "DEBUG" `
    -ExpectedEmbeddedAssets "raw" `
    -ExpectedProfile "minimal" -ExpectedDirectories "build-vcpkg-x64"
Assert-Plan -Arguments @("-Platform", "x86", "-Minimal") -ExpectedOfficial "OFF" -ExpectedMarker "DEBUG" `
    -ExpectedEmbeddedAssets "raw" `
    -ExpectedProfile "minimal" -ExpectedDirectories "build-vcpkg-x86"
Assert-Plan -Arguments @("-All") -ExpectedOfficial "OFF" -ExpectedMarker "DEBUG-All" `
    -ExpectedEmbeddedAssets "raw" `
    -ExpectedProfile "complete" -ExpectedDirectories "build-vcpkg-x86-complete,build-vcpkg-x64-complete"
Assert-Plan -Arguments @("-All", "-OfficialBuild") -ExpectedOfficial "ON" -ExpectedMarker "none" `
    -ExpectedEmbeddedAssets "raw" `
    -ExpectedProfile "official-complete" -ExpectedDirectories "build-vcpkg-x86-official-complete,build-vcpkg-x64-official-complete" `
    -ExpectedSubsetTablerFont "ON"
Assert-Plan -Arguments @("-All", "-OfficialBuild", "-CompressEmbeddedAssets") -ExpectedOfficial "ON" -ExpectedMarker "none" `
    -ExpectedEmbeddedAssets "xpress" `
    -ExpectedProfile "official-complete" -ExpectedDirectories "build-vcpkg-x86-official-complete,build-vcpkg-x64-official-complete" `
    -ExpectedSubsetTablerFont "ON"
Assert-Plan -Arguments @("-Platform", "x64", "-Complete", "-SubsetTablerFont") -ExpectedOfficial "OFF" -ExpectedMarker "DEBUG-All" `
    -ExpectedEmbeddedAssets "raw" `
    -ExpectedProfile "subset-complete" -ExpectedDirectories "build-vcpkg-x64-subset-complete" `
    -ExpectedSubsetTablerFont "ON"
Assert-ReleasePlan
Assert-GeneratedIdentity -Name "debug" -BundleOptionalExecutables $false -OfficialBuild $false -CompressEmbeddedAssets $false -ExpectedMarker "DEBUG" -ExpectedOfficial 0 -ExpectedCompressedAssets 0
Assert-GeneratedIdentity -Name "debug-all" -BundleOptionalExecutables $true -OfficialBuild $false -CompressEmbeddedAssets $false -ExpectedMarker "DEBUG-All" -ExpectedOfficial 0 -ExpectedCompressedAssets 0
Assert-GeneratedIdentity -Name "official" -BundleOptionalExecutables $true -OfficialBuild $true -CompressEmbeddedAssets $false -ExpectedMarker "" -ExpectedOfficial 1 -ExpectedCompressedAssets 0
Assert-GeneratedIdentity -Name "official-compressed-assets" -BundleOptionalExecutables $true -OfficialBuild $true -CompressEmbeddedAssets $true -ExpectedMarker "" -ExpectedOfficial 1 -ExpectedCompressedAssets 1
"build identity acceptance passed"
