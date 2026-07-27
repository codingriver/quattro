param(
    [string]$ExePath = "build-vcpkg-preset\Release\Quattro.exe",
    [string]$SeedPath = "build-vcpkg-preset\Release\QuattroDbSeed.exe",
    [string]$LogDir = "build-vcpkg-preset\Release\logs\background-acceptance"
)

$ErrorActionPreference = "Stop"
$resolvedExe = [System.IO.Path]::GetFullPath($ExePath)
$resolvedSeed = [System.IO.Path]::GetFullPath($SeedPath)
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

& (Join-Path $PSScriptRoot "test-background-acceptance-policy.ps1") | Out-Host
& (Join-Path $PSScriptRoot "test-quattro-test-harness.ps1") | Out-Host
& (Join-Path $PSScriptRoot "test-quattro-test-root-guard.ps1") -ExePath $resolvedExe | Out-Host
& (Join-Path $PSScriptRoot "test-quattro-production-test-isolation.ps1") -ExePath $resolvedExe | Out-Host

& (Join-Path $PSScriptRoot "run-ui-smoke.ps1") `
    -ExePath $resolvedExe `
    -LogDir (Join-Path $LogDir "ui") | Out-Host
& (Join-Path $PSScriptRoot "run-menu-visual-tests.ps1") `
    -ExePath $resolvedExe `
    -LogDir (Join-Path $LogDir "menu") | Out-Host
& (Join-Path $PSScriptRoot "run-scroll-tests.ps1") `
    -ExePath $resolvedExe `
    -SeedPath $resolvedSeed `
    -LogDir (Join-Path $LogDir "scroll") | Out-Host
& (Join-Path $PSScriptRoot "run-display-settings-tests.ps1") `
    -ExePath $resolvedExe `
    -LogDir (Join-Path $LogDir "display") | Out-Host
& (Join-Path $PSScriptRoot "run-dialog-display-tests.ps1") `
    -ExePath $resolvedExe `
    -LogDir (Join-Path $LogDir "dialogs") | Out-Host

"background_acceptance=passed"
"screenshots=$([System.IO.Path]::GetFullPath($LogDir))"
