$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$failed = $false

$taskService = Get-Content -LiteralPath (Join-Path $root 'src/services/LinkResourceRefreshService.cpp') -Raw
foreach ($required in @('TaskContext', 'TaskForEachOptions', 'ShellContextMenuRefreshService', 'UrlIconDownloadService::RefreshNow')) {
    if (-not $taskService.Contains($required)) {
        Write-Host "LinkResourceRefreshService is missing public task capability '$required'"
        $failed = $true
    }
}
foreach ($forbidden in @('std::thread', 'std::jthread', 'std::async', 'hardware_concurrency', 'std::atomic_size_t')) {
    if ($taskService.Contains($forbidden)) {
        Write-Host "private task worker pattern '$forbidden' remains in LinkResourceRefreshService"
        $failed = $true
    }
}

$mainWindow = Get-Content -LiteralPath (Join-Path $root 'src/windows/MainWindow.cpp') -Raw
foreach ($required in @('TaskExecutionService::StartTyped<LinkResourceRefreshResult>', 'ThemedTaskProgressDialog', 'WM_QUATTRO_RESOURCE_REFRESH_DONE', 'shellContextMenuCache_.UpdateBatch')) {
    if (-not $mainWindow.Contains($required)) {
        Write-Host "MainWindow refresh is missing public task integration '$required'"
        $failed = $true
    }
}

if ($failed) {
    exit 1
}
Write-Host 'task_service_rules=passed'
