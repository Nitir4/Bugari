GitHub Commit Manager for Windows - test candidate

Supports x86_64 Windows 10/11. This candidate still needs native desktop
validation. The package bundles its libraries; no MSYS2 or compiler is needed.

1. Extract the whole ZIP to a folder, keeping bin, lib, share and etc together.
2. Open bin\ghm-gui.exe. Click Login with GitHub and authorize your own account.
   Login needs a public OAuth Client ID configured when the package is built.
3. For the CLI, run bin\ghm.exe --help from PowerShell.

Optional installation (run PowerShell as your normal Windows user):
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\install.ps1

To enable scheduled commits after closing the GUI, explicitly install the
background worker:
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\install.ps1 -InstallWorker

This copies the app into your LocalAppData and adds a Start menu shortcut.
The optional Task Scheduler worker runs under your account while you are signed
in. Jobs missed during power-off, sleep or logout run when the worker resumes
or next starts. Keep the computer awake and signed in for execution at a precise
time. Changing a commit's metadata date does not change GitHub's push time.

The portable GUI alone does not start a background worker. Its job list can
contain pending jobs until you install the worker.

To remove the managed task and shortcut:
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1
Add -RemoveFiles to also remove installed application versions after closing
the GUI. Application data, repositories and Credential Manager login are kept.

SHA256SUMS lists package file checksums. Third-party notices are in third-party.
Report failures with the Windows version and the application's developer log;
do not send passwords or access tokens.
