$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$serviceFiles = @(
    'src/services/QuickImportService.cpp',
    'src/services/FileLockQueryService.cpp',
    'src/services/WebDavFileService.cpp',
    'src/services/ShellContextMenuRefreshService.cpp',
    'src/services/PortScanService.cpp',
    'src/applaunchlocker/AppLaunchLockerCore.cpp'
)
$forbiddenServicePatterns = @(
    'std::thread',
    'std::jthread',
    'std::async',
    'hardware_concurrency',
    'std::atomic_size_t next'
)
$failed = $false

foreach ($relative in $serviceFiles) {
    $path = Join-Path $root $relative
    $text = Get-Content -LiteralPath $path -Raw
    if ($text -notmatch 'ScanExecutionService|ScanTaskContext') {
        Write-Error "$relative does not use the public scan service"
    }
    foreach ($pattern in $forbiddenServicePatterns) {
        if ($text.Contains($pattern)) {
            Write-Host "forbidden scan worker pattern '$pattern' in $relative"
            $failed = $true
        }
    }
}

$uiChecks = @{
    'src/windows/QuickImportDialog.cpp' = @('scanThread_', 'scanState_')
    'src/windows/SimpleDialogs.cpp' = @('contextMenuRefreshThread_', 'refreshStop_')
    'src/windows/BuiltinTools.cpp' = @('fileLockThread_', 'fileLockScanState_', 'FileLockScanState', 'CollectTcp4Port')
    'src/applaunchlocker/AdBlockWindow.cpp' = @('scanState_')
}
foreach ($relative in $uiChecks.Keys) {
    $text = Get-Content -LiteralPath (Join-Path $root $relative) -Raw
    foreach ($pattern in $uiChecks[$relative]) {
        if ($text.Contains($pattern)) {
            Write-Host "private scan state '$pattern' remains in $relative"
            $failed = $true
        }
    }
}

if ($failed) {
    exit 1
}
Write-Host 'scan_service_rules=passed'
