param([switch]$InstallWorker, [string]$InstallRoot = (Join-Path $env:LOCALAPPDATA 'GitHubCommitManager'))
$ErrorActionPreference = 'Stop'
$marker = 'Managed by GitHub Commit Manager Windows installer'
$source = $PSScriptRoot
$user = [System.Security.Principal.WindowsIdentity]::GetCurrent()
$taskName = 'GitHubCommitManager-' + $user.User.Value
$hash = (Get-FileHash (Join-Path $source 'bin\ghm-gui.exe') -Algorithm SHA256).Hash.ToLower()
$destination = Join-Path $InstallRoot ('versions\' + $hash.Substring(0, 16))
$oldTask = Get-ScheduledTask -TaskName $taskName -TaskPath '\' -ErrorAction SilentlyContinue
if ($oldTask -and $oldTask.Description -ne $marker) {
    throw "A custom task named $taskName exists. Rename it before installing the app worker."
}
if (!(Test-Path $destination)) {
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    try {
        Get-ChildItem -LiteralPath $source -Force | Copy-Item -Destination $destination -Recurse -Force
    } catch {
        throw "Package copy failed. No worker or shortcut was changed. Remove the incomplete folder $destination before retrying. $_"
    }
}
if ((Get-FileHash (Join-Path $destination 'bin\ghm-gui.exe') -Algorithm SHA256).Hash.ToLower() -ne $hash) {
    throw 'An existing installation folder is incomplete or contains another build.'
}
$programs = [Environment]::GetFolderPath('Programs')
$shortcutPath = Join-Path $programs 'GitHub Commit Manager.lnk'
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
if (Test-Path $shortcutPath) {
    if ($shortcut.Description -ne $marker) { throw 'A custom GitHub Commit Manager shortcut already exists.' }
}
$shortcut.TargetPath = Join-Path $destination 'bin\ghm-gui.exe'
$shortcut.WorkingDirectory = $destination
$shortcut.Description = $marker
$shortcut.Save()
if ($InstallWorker -or $oldTask) {
    # Update an already-enabled worker, or enable it through explicit opt-in.
    if ($oldTask) { Stop-ScheduledTask -TaskName $taskName -TaskPath '\' }
    $action = New-ScheduledTaskAction -Execute (Join-Path $destination 'bin\ghm-worker.exe') -WorkingDirectory $destination
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $user.Name
    $principal = New-ScheduledTaskPrincipal -UserId $user.User.Value -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew -ExecutionTimeLimit ([TimeSpan]::Zero) `
        -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
    Register-ScheduledTask -TaskName $taskName -TaskPath '\' -Description $marker -Action $action `
        -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null
    Start-ScheduledTask -TaskName $taskName -TaskPath '\'
}
Write-Host "Installed in $destination"
Write-Host 'Launch GitHub Commit Manager from the Start menu. Data and login belong to your Windows account.'
