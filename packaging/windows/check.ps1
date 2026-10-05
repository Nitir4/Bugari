param([Parameter(Mandatory=$true)][string]$PackagePath, [Parameter(Mandatory=$true)][string]$TestPath)
$ErrorActionPreference = 'Stop'
$package = (Resolve-Path $PackagePath).Path
$tests = (Resolve-Path $TestPath).Path
$oldPath = $env:PATH
$sandbox = Join-Path ([System.IO.Path]::GetTempPath()) ('ghm-package-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $sandbox | Out-Null
$oldData = $env:XDG_DATA_HOME
$oldState = $env:XDG_STATE_HOME
try {
    # The test executable lives in the packaged bin directory and must resolve
    # all of its libraries there. Build-environment DLLs cannot satisfy imports.
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $env:XDG_DATA_HOME = $sandbox
    $env:XDG_STATE_HOME = $sandbox
    foreach ($name in @('auth', 'datetime', 'windows_credentials', 'portable')) {
        $testExe = Join-Path $package "bin\ghm-$name-test.exe"
        Copy-Item -LiteralPath (Join-Path $tests "ghm-$name-test.exe") -Destination $testExe
        try {
            & $testExe (Join-Path $package 'bin\ghm-worker.exe')
            if ($LASTEXITCODE -ne 0) { throw "Packaged $name test failed: $LASTEXITCODE" }
        } finally { Remove-Item -LiteralPath $testExe }
    }
    & (Join-Path $package 'bin\ghm.exe') --help
    if ($LASTEXITCODE -ne 0) { throw 'Packaged CLI did not start.' }
    # Starting GTK verifies that bundled schemas, loaders and DLLs work. This
    # is a startup check; a real Windows 10/11 desktop still needs manual tests.
    $guiLog = Join-Path $sandbox 'gui-stderr.txt'
    $gui = Start-Process -FilePath (Join-Path $package 'bin\ghm-gui.exe') -WorkingDirectory $sandbox -PassThru -RedirectStandardError $guiLog
    try {
        Start-Sleep -Seconds 8
        if ($gui.HasExited) { throw "Packaged GUI exited during startup: $($gui.ExitCode). $(Get-Content $guiLog -Raw)" }
    } finally { if (!$gui.HasExited) { Stop-Process -Id $gui.Id } }
    Write-Host 'PASS packaged CLI, tests and GUI startup without the build environment on PATH'
} finally {
    $env:PATH = $oldPath
    $env:XDG_DATA_HOME = $oldData
    $env:XDG_STATE_HOME = $oldState
    Remove-Item -LiteralPath $sandbox -Recurse -Force
}
