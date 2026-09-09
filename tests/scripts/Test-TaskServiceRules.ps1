param([string]$OutputRoot = (Join-Path $env:TEMP ('quattro-task-lint-' + [Guid]::NewGuid().ToString('N'))))

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$checker = Join-Path $repo 'tools/check-task-service-usage.ps1'
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $OutputRoot) { throw 'Fixture output already exists' }
New-Item -ItemType Directory -Path $OutputRoot | Out-Null
Set-Content -LiteralPath (Join-Path $OutputRoot '.quattro-test-root') -Value 'task-service-rule-fixtures'
$files = @(
    'src/services/LinkResourceRefreshService.cpp',
    'src/services/WebDavFileService.cpp',
    'src/windows/MainWindow.cpp',
    'src/windows/SimpleDialogs.cpp'
)
foreach ($relative in $files) {
    $destination = Join-Path $OutputRoot $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo $relative) -Destination $destination
}
$shell = (Get-Process -Id $PID).Path
$positive = & $shell -NoProfile -File $checker -Root $OutputRoot 2>&1
if ($LASTEXITCODE -ne 0) { throw "Positive task rules failed: $positive" }
$positive | Set-Content -LiteralPath (Join-Path $OutputRoot 'positive.txt')
$dialogPath = Join-Path $OutputRoot 'src/windows/SimpleDialogs.cpp'
$original = [IO.File]::ReadAllText($dialogPath)
foreach ($fixture in @(
    @{ Name = 'private-delete-thread'; Anchor = 'class WebDavFileManagerDialog {'; Payload = 'std::thread forbiddenDeleteWorker;' },
    @{ Name = 'private-delete-stop'; Anchor = 'class WebDavFileManagerDialog {'; Payload = 'std::stop_source forbiddenDeleteStop;' },
    @{ Name = 'private-provider-thread'; Anchor = '    bool StartContextMenuIconLoad(bool force) {'; Payload = 'std::thread forbiddenIconWorker;' }
)) {
    if (!$original.Contains($fixture.Anchor)) { throw "Fixture anchor missing: $($fixture.Name)" }
    [IO.File]::WriteAllText($dialogPath, $original.Replace($fixture.Anchor, $fixture.Anchor + "`n" + $fixture.Payload))
    $negative = & $shell -NoProfile -File $checker -Root $OutputRoot 2>&1
    $code = $LASTEXITCODE
    $negative | Set-Content -LiteralPath (Join-Path $OutputRoot ($fixture.Name + '.txt'))
    if ($code -ne 1) { throw "Negative task fixture was not rejected: $($fixture.Name)" }
}
[IO.File]::WriteAllText($dialogPath, $original)
Write-Output "task_rule_fixtures=passed evidence=$OutputRoot"
