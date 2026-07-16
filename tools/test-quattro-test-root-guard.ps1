param(
    [string]$ExePath = "build-vcpkg-preset\Release\Quattro.exe"
)

$ErrorActionPreference = "Stop"
$root = Join-Path ([System.IO.Path]::GetTempPath()) ("QuattroMissingTestRoot_" + [Guid]::NewGuid().ToString("N"))
$process = $null

try {
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = [System.IO.Path]::GetFullPath($ExePath)
    $startInfo.WorkingDirectory = [System.IO.Path]::GetFullPath($root)
    $startInfo.UseShellExecute = $false
    $startInfo.EnvironmentVariables["QUATTRO_TEST_MODE"] = "1"
    $startInfo.EnvironmentVariables["QUATTRO_TEST_NO_FOCUS"] = "1"
    $startInfo.EnvironmentVariables["QUATTRO_ACCEPTANCE_MODE"] = "background"
    $startInfo.EnvironmentVariables["QUATTRO_USER_CONFIG_DIR"] = [System.IO.Path]::GetFullPath($root)
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    [void]$process.Start()
    if (!$process.WaitForExit(10000)) {
        throw "Quattro did not reject the unmarked test root."
    }
    if ($process.ExitCode -ne 2) {
        throw "Unexpected missing-test-root exit code: $($process.ExitCode)"
    }
    if ((Test-Path -LiteralPath (Join-Path $root "conf.ini")) -or
        (Test-Path -LiteralPath (Join-Path $root "db"))) {
        throw "Quattro wrote runtime data before rejecting the unmarked test root."
    }
    "test_root_guard=passed"
} finally {
    if ($process -and !$process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
