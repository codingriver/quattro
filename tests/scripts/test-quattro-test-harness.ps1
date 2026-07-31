$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "QuattroTestHarness.ps1")

$root = Join-Path ([System.IO.Path]::GetTempPath()) ("QuattroTestHarness_" + [Guid]::NewGuid().ToString("N"))
$childScript = Join-Path $root "read-environment.ps1"
$report = Join-Path $root "environment.txt"
$previousConfig = $env:QUATTRO_USER_CONFIG_DIR
$previousMode = $env:QUATTRO_TEST_MODE
$previousFocus = $env:QUATTRO_TEST_NO_FOCUS
$process = $null

try {
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    @'
param([string]$Report)
@(
    "config=$env:QUATTRO_USER_CONFIG_DIR"
    "test_mode=$env:QUATTRO_TEST_MODE"
    "no_focus=$env:QUATTRO_TEST_NO_FOCUS"
    "acceptance_mode=$env:QUATTRO_ACCEPTANCE_MODE"
    "run_id=$env:QUATTRO_TEST_RUN_ID"
) | Set-Content -LiteralPath $Report -Encoding UTF8
'@ | Set-Content -LiteralPath $childScript -Encoding UTF8

    $powershell = (Get-Process -Id $PID).Path
    $arguments = "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$childScript`" -Report `"$report`""
    $process = Start-QuattroTestProcess -FilePath $powershell -WorkingDirectory $root -Arguments $arguments
    if (!$process.WaitForExit(10000)) {
        throw "Harness environment child did not exit."
    }
    if ($process.ExitCode -ne 0) {
        throw "Harness environment child failed with exit code $($process.ExitCode)."
    }

    $values = @{}
    foreach ($line in Get-Content -LiteralPath $report) {
        $parts = $line -split '=', 2
        if ($parts.Count -eq 2) {
            $values[$parts[0]] = $parts[1]
        }
    }
    $expectedRoot = [System.IO.Path]::GetFullPath($root)
    if ($values["config"] -ne $expectedRoot) {
        throw "Child config root was not isolated. expected=$expectedRoot actual=$($values['config'])"
    }
    if ($values["test_mode"] -ne "1" -or $values["no_focus"] -ne "1" -or $values["acceptance_mode"] -ne "background") {
        throw "Child test environment was incomplete."
    }
    if ([string]::IsNullOrWhiteSpace($values["run_id"])) {
        throw "Child test run id was empty."
    }
    if (!(Test-Path -LiteralPath (Join-Path $root ".quattro-test-root"))) {
        throw "Test root marker was not created."
    }
    if ($env:QUATTRO_USER_CONFIG_DIR -ne $previousConfig -or
        $env:QUATTRO_TEST_MODE -ne $previousMode -or
        $env:QUATTRO_TEST_NO_FOCUS -ne $previousFocus) {
        throw "Harness modified the parent process environment."
    }

    "quattro_test_harness=passed"
} finally {
    if ($process -and !$process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
