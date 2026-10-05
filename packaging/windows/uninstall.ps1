param([switch]$RemoveFiles, [string]$InstallRoot = (Join-Path $env:LOCALAPPDATA 'GitHubCommitManager'))
$ErrorActionPreference = 'Stop'
$marker = 'Managed by GitHub Commit Manager Windows installer'
$user = [System.Security.Principal.WindowsIdentity]::GetCurrent()
$taskName = 'GitHubCommitManager-' + $user.User.Value
$task = Get-ScheduledTask -TaskName $taskName -TaskPath '\' -ErrorAction SilentlyContinue
if ($task) {
    if ($task.Description -ne $marker) { throw 'The worker task is custom; it was preserved.' }
    Stop-ScheduledTask -TaskName $taskName -TaskPath '\'
    Unregister-ScheduledTask -TaskName $taskName -TaskPath '\' -Confirm:$false
}
$shortcutPath = Join-Path ([Environment]::GetFolderPath('Programs')) 'GitHub Commit Manager.lnk'
if (Test-Path $shortcutPath) {
    $shell = New-Object -ComObject WScript.Shell
    if ($shell.CreateShortcut($shortcutPath).Description -ne $marker) { throw 'The Start menu shortcut is custom; it was preserved.' }
    Remove-Item -LiteralPath $shortcutPath
}
if ($RemoveFiles -and (Test-Path $InstallRoot)) {
    # Delete only version directories carrying this package's own metadata.
    Get-ChildItem -LiteralPath (Join-Path $InstallRoot 'versions') -Directory | ForEach-Object {
        if ((Test-Path (Join-Path $_.FullName 'build-info.json')) -and
            (Test-Path (Join-Path $_.FullName 'bin\ghm-gui.exe'))) {
            Remove-Item -LiteralPath $_.FullName -Recurse -Force
        }
    }
}
Write-Host 'Removed the managed worker and launcher. Repositories, job data and saved login were preserved.'
