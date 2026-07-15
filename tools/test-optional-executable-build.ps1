param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    [string]$BuildRoot = "build-optional-executable-acceptance"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$resolvedBuildRoot = [System.IO.Path]::GetFullPath((Join-Path $root $BuildRoot))
$resolvedRoot = [System.IO.Path]::GetFullPath($root).TrimEnd('\')
if (!$resolvedBuildRoot.StartsWith($resolvedRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "BuildRoot must stay inside the repository: $resolvedBuildRoot"
}

function Invoke-Native {
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

function Reset-BuildDirectory {
    param([string]$Path)

    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

function Find-BuiltExecutable {
    param(
        [string]$BuildDirectory,
        [string]$Name
    )

    return @(Get-ChildItem -LiteralPath $BuildDirectory -Recurse -Filter $Name -File -ErrorAction SilentlyContinue)
}

function Build-And-Assert {
    param(
        [string]$Name,
        [bool]$BundleOptionalExecutables
    )

    $buildDirectory = Join-Path $resolvedBuildRoot $Name
    Reset-BuildDirectory -Path $buildDirectory
    $bundleValue = if ($BundleOptionalExecutables) { "ON" } else { "OFF" }
    Invoke-Native -Description "$Name configure" -Command "cmake" -Arguments @(
        "-S", $root,
        "-B", $buildDirectory,
        "-DQUATTRO_BUILD_TESTS=OFF",
        "-DQUATTRO_BUNDLE_OPTIONAL_EXECUTABLES=$bundleValue"
    )
    $quattroProject = Join-Path $buildDirectory "Quattro.vcxproj"
    if (Test-Path -LiteralPath $quattroProject) {
        $projectText = Get-Content -LiteralPath $quattroProject -Raw
        $referencesAppLaunchLocker = $projectText -match "AppLaunchLocker\.vcxproj"
        $referencesUpdater = $projectText -match "QuattroUpdater\.vcxproj"
        if ($referencesAppLaunchLocker -ne $BundleOptionalExecutables -or
            $referencesUpdater -ne $BundleOptionalExecutables) {
            throw "$Name Quattro target has an incorrect optional-executable dependency graph."
        }

        $solutionPath = @(Get-ChildItem -LiteralPath $buildDirectory -Filter "*.sln" -File)[0].FullName
        $solutionText = Get-Content -LiteralPath $solutionPath -Raw
        foreach ($optionalProjectName in @("AppLaunchLocker", "QuattroUpdater")) {
            $optionalProjectPath = Join-Path $buildDirectory "$optionalProjectName.vcxproj"
            $optionalProjectText = Get-Content -LiteralPath $optionalProjectPath -Raw
            if ($optionalProjectText -notmatch '<ProjectGuid>(\{[^<]+\})</ProjectGuid>') {
                throw "$Name could not read $optionalProjectName project GUID."
            }
            $projectGuid = [regex]::Escape($matches[1])
            if ($solutionText -match "$projectGuid\.[^\r\n]+\.Build\.0\s*=") {
                throw "$Name includes $optionalProjectName in the default solution build."
            }
        }
    }
    Invoke-Native -Description "$Name embedded executable catalog build" -Command "cmake" -Arguments @(
        "--build", $buildDirectory,
        "--config", $Configuration,
        "--target", "QuattroEmbeddedExecutableCatalogProbe"
    )

    $appLaunchLockerCount = (Find-BuiltExecutable -BuildDirectory $buildDirectory -Name "AppLaunchLocker.exe").Count
    $updaterCount = (Find-BuiltExecutable -BuildDirectory $buildDirectory -Name "QuattroUpdater.exe").Count
    $catalogPath = Join-Path $buildDirectory "generated\EmbeddedExecutableCatalog.cpp"
    if (!(Test-Path -LiteralPath $catalogPath)) {
        throw "$Name did not generate the embedded executable catalog."
    }
    $catalog = Get-Content -LiteralPath $catalogPath -Raw
    if ($BundleOptionalExecutables) {
        if ($appLaunchLockerCount -eq 0 -or $updaterCount -eq 0) {
            throw "$Name did not build both optional executables."
        }
        if ($catalog -notmatch "EmbeddedExecutable_AppLaunchLocker" -or
            $catalog -notmatch "EmbeddedExecutable_QuattroUpdater") {
            throw "$Name did not register both optional executables in the embedded catalog."
        }
    } else {
        if ($appLaunchLockerCount -ne 0 -or $updaterCount -ne 0) {
            throw "$Name unexpectedly built an optional executable."
        }
        if ($catalog -match "EmbeddedExecutable_AppLaunchLocker" -or
            $catalog -match "EmbeddedExecutable_QuattroUpdater") {
            throw "$Name unexpectedly registered an optional executable."
        }
    }
}

New-Item -ItemType Directory -Force -Path $resolvedBuildRoot | Out-Null
$buildScript = Join-Path $PSScriptRoot "build.ps1"
$minimalPlan = @(& $buildScript -PlanOnly)
$completePlan = @(& $buildScript -All -PlanOnly)
if ($minimalPlan -notcontains "architectures=x64" -or
    $minimalPlan -notcontains "bundle_optional_executables=OFF") {
    throw "Default build.ps1 plan is not a minimal x64 build."
}
if ($completePlan -notcontains "architectures=x86,x64" -or
    $completePlan -notcontains "bundle_optional_executables=ON") {
    throw "build.ps1 -All plan is not a complete x86+x64 build."
}
Build-And-Assert -Name "minimal" -BundleOptionalExecutables $false
Build-And-Assert -Name "complete" -BundleOptionalExecutables $true
"optional executable build acceptance passed"
