param([string]$Root = (Split-Path -Parent $PSScriptRoot))

$ErrorActionPreference = 'Stop'
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

$webDavService = Get-Content -LiteralPath (Join-Path $Root 'src/services/WebDavFileService.cpp') -Raw
$dialogs = Get-Content -LiteralPath (Join-Path $Root 'src/windows/SimpleDialogs.cpp') -Raw
$scopes = @{
    WebDavFileService = $webDavService
}
foreach ($scope in @(
    @{ Name = 'WebDavFileManagerDialog'; Start = 'class WebDavFileManagerDialog'; End = 'class SettingsDialog' },
    @{ Name = 'SettingsProviderIcons'; Start = '    bool StartContextMenuIconLoad('; End = '    void ReadContextMenuTableDraft(' }
)) {
    $begin = $dialogs.IndexOf($scope.Start, [StringComparison]::Ordinal)
    $end = if ($begin -ge 0) { $dialogs.IndexOf($scope.End, $begin, [StringComparison]::Ordinal) } else { -1 }
    if ($begin -lt 0 -or $end -le $begin) {
        Write-Host "Missing public task audit scope '$($scope.Name)'"
        $failed = $true
        continue
    }
    $scopes[$scope.Name] = $dialogs.Substring($begin, $end - $begin)
}
# Single transfers and backup workflows outside these batch scopes are not
# certified by this check; expanding it requires explicit lifecycle review.
foreach ($name in $scopes.Keys) {
    foreach ($forbidden in @('std::thread', 'std::jthread', 'std::async', 'std::stop_source',
            'std::atomic_size_t', 'DeleteTaskState', 'deleteTaskState_', 'SettingsContextMenuIconAsyncState')) {
        if ($scopes[$name].Contains($forbidden)) {
            Write-Host "private task pattern '$forbidden' remains in $name"
            $failed = $true
        }
    }
}
foreach ($required in @('TaskExecutionService::StartTyped<WebDavFileDeleteBatchResult>',
        'context.ForEach<WebDavFileRecord', 'RemoveBatch(ids)', 'context.StopRequested()')) {
    if (!$webDavService.Contains($required)) {
        Write-Host "WebDAV delete service is missing '$required'"
        $failed = $true
    }
}
if ($scopes.ContainsKey('WebDavFileManagerDialog')) {
    foreach ($required in @('StartDeleteBatch(', 'ToThemedTaskProgressSnapshot',
            'pendingDeleteSyncIds_', 'generation != deleteGeneration_')) {
        if (!$scopes.WebDavFileManagerDialog.Contains($required)) {
            Write-Host "WebDAV delete UI is missing '$required'"
            $failed = $true
        }
    }
}

if ($failed) {
    exit 1
}
Write-Host 'task_service_rules=passed'
