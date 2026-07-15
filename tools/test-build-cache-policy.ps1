param(
    [string]$Configuration = "Release",
    [ValidateSet("vcpkg", "classic")]
    [string]$Backend = "vcpkg",
    [string]$Version = "0.1.4",
    [double]$MaxWarmSeconds = 60
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $PSScriptRoot "build.ps1"
$plan = @(& $buildScript -Backend $Backend -PlanOnly)
$directoryLine = $plan | Where-Object { $_ -like "build_directories=*" } | Select-Object -First 1
if (!$directoryLine) {
    throw "Default build plan did not report a build directory."
}
$buildDirectoryName = $directoryLine.Substring("build_directories=".Length)
if ($buildDirectoryName.Contains(",")) {
    throw "Default build plan unexpectedly contains multiple directories: $buildDirectoryName"
}
$buildDirectory = Join-Path $root $buildDirectoryName
if (!(Test-Path -LiteralPath (Join-Path $buildDirectory "CMakeCache.txt"))) {
    throw "Build cache does not exist; run one default package build before this incremental cache acceptance: $buildDirectory"
}

$sentinel = Join-Path $buildDirectory ".quattro-cache-policy-sentinel"
New-Item -ItemType File -Force -Path $sentinel | Out-Null

function Invoke-PackageTimed {
    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    $output = @(& $buildScript -Configuration $Configuration -Backend $Backend -NoZip -Version $Version)
    if ($LASTEXITCODE -ne 0) {
        throw "Package build failed with exit code $LASTEXITCODE."
    }
    foreach ($line in $output) {
        Write-Host $line
    }
    $stopwatch.Stop()
    return $stopwatch.Elapsed.TotalSeconds
}

try {
    $changedOptionSeconds = Invoke-PackageTimed
    if (!(Test-Path -LiteralPath $sentinel)) {
        throw "A normal CMake option change deleted the compatible build directory."
    }
    $warmSeconds = Invoke-PackageTimed
    if (!(Test-Path -LiteralPath $sentinel)) {
        throw "A warm repeat package deleted the compatible build directory."
    }
    if ($warmSeconds -gt $MaxWarmSeconds) {
        throw "Warm repeat package took $([math]::Round($warmSeconds, 1))s, exceeding ${MaxWarmSeconds}s."
    }
    "build_cache_policy_acceptance=passed option_change_seconds=$([math]::Round($changedOptionSeconds, 1)) warm_seconds=$([math]::Round($warmSeconds, 1))"
} finally {
    Remove-Item -LiteralPath $sentinel -Force -ErrorAction SilentlyContinue
}
