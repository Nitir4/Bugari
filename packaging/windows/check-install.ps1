param([Parameter(Mandatory=$true)][string]$PackagePath)
$ErrorActionPreference = 'Stop'
# This integration check registers a real task and launcher only on a fresh,
# disposable hosted runner. Manual desktop validation uses the user guide.
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted') {
    throw 'Run this installation integration check on a disposable GitHub-hosted Windows runner.'
}
$package = (Resolve-Path $PackagePath).Path
$root = Join-Path ([System.IO.Path]::GetTempPath()) ('ghm-install-' + [guid]::NewGuid().ToString('N'))
$user = [System.Security.Principal.WindowsIdentity]::GetCurrent()
$taskName = 'GitHubCommitManager-' + $user.User.Value
$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'GitHub Commit Manager.lnk'
if ((Get-ScheduledTask -TaskName $taskName -TaskPath '\' -ErrorAction SilentlyContinue) -or (Test-Path $shortcut)) {
    throw 'An existing task or shortcut was preserved; this runner is not clean.'
}
try {
    & (Join-Path $package 'install.ps1') -InstallRoot $root -InstallWorker
    Start-Sleep -Seconds 5
    $task = Get-ScheduledTask -TaskName $taskName -TaskPath '\'
    if ($task.State -ne 'Running' -or !(Test-Path $shortcut) -or
        !(Test-Path $task.Actions[0].Execute)) {
        $info = Get-ScheduledTaskInfo -TaskName $taskName -TaskPath '\'
        throw "The installed worker or launcher did not start correctly. State=$($task.State), result=$($info.LastTaskResult)."
    }
    & (Join-Path $package 'uninstall.ps1') -InstallRoot $root -RemoveFiles
    if ((Get-ScheduledTask -TaskName $taskName -TaskPath '\' -ErrorAction SilentlyContinue) -or (Test-Path $shortcut)) {
        throw 'The installer left a task or shortcut behind.'
    }
    Write-Host 'PASS real Task Scheduler worker installation, startup and uninstall on a disposable Windows runner'
} finally {
    try {
        & (Join-Path $package 'uninstall.ps1') -InstallRoot $root -RemoveFiles
        if (Test-Path $root) { Remove-Item -LiteralPath $root -Recurse -Force }
    } catch { Write-Warning "Installation-check cleanup failed: $_" }
}
