param(
    [string]$ExePath = "build-vcpkg-preset\Release\Quattro.exe"
)

$ErrorActionPreference = "Stop"
$resolvedExe = [System.IO.Path]::GetFullPath($ExePath)

$root = Join-Path ([System.IO.Path]::GetTempPath()) ("QuattroProductionIsolation_" + [Guid]::NewGuid().ToString("N"))
$formalRoot = Join-Path $root "formal"
$logRoot = Join-Path $root "logs"
$formal = $null

try {
    New-Item -ItemType Directory -Force -Path $formalRoot, $logRoot | Out-Null
    Copy-Item -LiteralPath $resolvedExe -Destination (Join-Path $formalRoot "Quattro.exe") -Force
    @"
[main]
bTopMost=0
bGlobalHotKeysEnabled=0
bHideOnStart=0
bHideNotify=1
bAutoDock=0
"@ | Set-Content -LiteralPath (Join-Path $formalRoot "conf.ini") -Encoding Unicode

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = [System.IO.Path]::GetFullPath((Join-Path $formalRoot "Quattro.exe"))
    $startInfo.WorkingDirectory = [System.IO.Path]::GetFullPath($formalRoot)
    $startInfo.UseShellExecute = $false
    $startInfo.EnvironmentVariables["QUATTRO_USER_CONFIG_DIR"] = [System.IO.Path]::GetFullPath($formalRoot)
    $startInfo.EnvironmentVariables["QUATTRO_TEST_NO_FOCUS"] = "1"
    $startInfo.EnvironmentVariables["QUATTRO_ACCEPTANCE_MODE"] = "background"
    $formal = New-Object System.Diagnostics.Process
    $formal.StartInfo = $startInfo
    [void]$formal.Start()
    Start-Sleep -Milliseconds 1200
    $formal.Refresh()
    if ($formal.HasExited) {
        throw "Formal control process exited before test started. exit=$($formal.ExitCode)"
    }
    $formalPid = $formal.Id

    & (Join-Path $PSScriptRoot "run-ui-smoke.ps1") `
        -ExePath $resolvedExe `
        -LogDir $logRoot | Out-Host

    $formal.Refresh()
    if ($formal.HasExited) {
        throw "Formal control process was terminated by test acceptance. pid=$formalPid"
    }
    "production_test_isolation=passed"
    "formal_pid=$formalPid"
} finally {
    if ($formal -and !$formal.HasExited) {
        Stop-Process -Id $formal.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
