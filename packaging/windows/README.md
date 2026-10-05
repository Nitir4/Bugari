# Windows port: developing from Linux

Status: initial port implemented on `feature/windows-support`. The GUI, CLI
and background worker cross-compile to native x86_64 Windows executables. All
23 Linux tests and four Windows tests under Wine have passed. Native Windows
CI and interactive Windows 10/11 desktop checks are separate validation steps;
the package is a test candidate until those checks pass.

## Build and test without a local Windows machine

Use a GitHub Actions Windows runner as the build and automated test machine.
Develop and review the source on Linux, then run the Windows job for the same
commit. A passing Linux build alone is not evidence of Windows compatibility.

The initial toolchain should be MSYS2 **UCRT64**, CMake and Ninja. GTK documents
this toolchain for Windows. Use the native UCRT64 compiler and libraries rather
than linking the application to the MSYS POSIX runtime. Friends should receive
a package containing the application, DLLs and GTK resources, without needing
MSYS2 or development tools installed.

The [Windows workflow](../../.github/workflows/windows.yml) runs automatically
when this branch is pushed, and on pull requests. It also runs the Linux suite
to check for regressions. Set the repository's Actions variable
`GHM_GITHUB_CLIENT_ID` to the public Client ID of your OAuth application with
Device Flow enabled. Without it, local Git features still work, but a new
user's GitHub login is disabled.

The workflow will:

1. Check out the branch on a pinned Windows runner image, such as
   `windows-2022`.
2. Install the UCRT64 compiler, CMake, Ninja, pkg-config, GTK4, libgit2, curl,
   SQLite and json-glib with `msys2/setup-msys2`.
3. Configure and build the GUI, CLI, worker and Windows-compatible tests.
   Supply the public OAuth Client ID using the repository variable
   `GHM_GITHUB_CLIENT_ID`; never include a personal token or client secret.
4. Run the Git and scheduling tests against disposable local repositories.
   Tests must not depend on real GitHub credentials or repositories.
5. Assemble a portable directory with its DLLs, GTK resources, dependency
   notices and version information.
6. Test that directory from PowerShell with the MSYS2 library directories
   removed from PATH, so missing bundled dependencies cannot be hidden.
7. Upload the tested directory as a ZIP artifact, with a SHA-256 checksum.
   An artifact is a test download; publishing a release is a separate step.

After a successful run, open the repository's Actions page, select **Windows
candidate**, and download `GitHubCommitManager-windows-x86_64-candidate` from
the run's artifacts. It contains the application ZIP and its checksum. Send the
whole ZIP to a Windows volunteer; they do not need development dependencies.
Failed builds/tests cannot upload the candidate artifact.

For a development build on a Windows machine with MSYS2 UCRT64:

```sh
cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DGHM_GITHUB_CLIENT_ID=YOUR_PUBLIC_CLIENT_ID
cmake --build build-windows --parallel 4
ctest --test-dir build-windows --output-on-failure
pacman -Q > build-windows/packages.txt
python packaging/windows/bundle.py --build build-windows --prefix /ucrt64 \
  --output dist/GitHubCommitManager-0.1.0-windows-x86_64 \
  --inventory build-windows/packages.txt
```

Run packaging into a fresh output directory. The tests use disposable
repositories, include a synthetic Credential Manager round trip, and verify
interrupted publication and SQLite completion recovery. They do not sign in to
GitHub. The PowerShell package check reruns them with MSYS2 removed from PATH
and checks GUI startup. Linux namespace/fork stress tests have not all been
ported; the Windows suite is not equal in coverage to the Linux suite.

## Platform implementations

Keep the libgit2 operations, SQLite job queue, frozen snapshots and job recovery
logic shared. Introduce platform interfaces for the OS-specific behavior:

| Area | Current implementation | Windows implementation |
| --- | --- | --- |
| Credentials | `src/security/credentials.c`, libsecret | Windows Credential Manager, with per-user persistence |
| Worker startup | systemd user unit | Task Scheduler logon task running under the signed-in user |
| Worker waiting | timerfd, inotify, poll, netlink | A Windows waitable timer checks the queue and retry deadlines every second |
| Operation locks | flock, device/inode identity | File-handle locks and Windows file identity |
| Index publishing and editor saves | POSIX exclusive creation, fsync, rename | Windows exclusive creation, flush and replacement with explicit sharing rules |
| Safe path traversal | openat, O_NOFOLLOW | Handle-based path checks, including reparse points and junctions |
| Data and logs | XDG directories | A per-user Windows application data directory |
| Tests | /tmp, fork, Unix signals, namespaces | Temporary Windows directories and Windows process lifecycle helpers |

The bundled OpenSSL libraries use a PEM export of the current user's Windows
ROOT certificate store. `GHM_CA_BUNDLE` can override it with an explicit trust
bundle. TLS verification remains enabled. Trust-store changes are picked up
on the next fresh application/worker start. This export does not reproduce
every Windows certificate-policy check.

Windows files are opened write-through and flushed before publication. Windows
does not provide POSIX directory `fsync`; the adapter must not be taken as a
promise of identical power-loss durability on every Windows filesystem.

The package includes `install.ps1`, `uninstall.ps1` and user instructions.
Installation is per user and adds a Start menu shortcut. `-InstallWorker`
explicitly enables a Task Scheduler logon task under that user's identity.
Updates copy a version to a new directory before changing the worker action,
and refuse to overwrite a custom task or shortcut. Uninstallation preserves
the job database, repositories and saved credentials.

POSIX device/inode comparisons must not be carried over using Windows CRT
`stat` fields: worktree aliases and lock recovery need reliable Windows file
identity. Likewise, simply removing `O_NOFOLLOW` checks would weaken the file
operations and is not an acceptable portability fix.

Use one background worker and keep individual jobs in SQLite. Task Scheduler
starts the worker; it should not become a second independent job database.
Initially support execution while the user is signed in, including after the
GUI closes. Run overdue jobs on the next worker startup. Execution while the
computer is powered off is unavailable; sleep and logout behavior must be
documented and tested rather than promised.

## Release checks

Automated Windows checks must demonstrate:

- Stage/unstage, deletions, renamed files and a first commit in an empty repo.
- Scheduled snapshots exclude edits made after scheduling and preserve the
  ordinary staged index.
- Chained jobs, cancellations, branch/parent safety checks and exact local
  pushes to a disposable bare remote.
- Cross-process locking, linked worktrees, path aliases and recovery after
  interrupted publication, including no duplicate commits.
- Paths containing spaces and Unicode, Windows case behavior, and refusal of
  unsafe junction/reparse-point traversal.
- Worker execution after the GUI closes and handling of overdue jobs and push
  retries after restart.
- Packaged execution without the build environment's PATH.

Before sharing as a supported release, a volunteer should also test a clean
Windows 10/11 desktop: GUI startup, browser/device login, token persistence,
background worker installation, logout/login, sleep/resume, and uninstall.
CI results do not establish those interactive desktop behaviors. Keep the
first candidate explicitly marked as a test build until these checks pass.

## References

- [GitHub-hosted Windows runners](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)
- [MSYS2 setup action](https://github.com/msys2/setup-msys2)
- [GTK on Windows](https://www.gtk.org/docs/installations/windows/)
- [libgit2 supported platforms](https://libgit2.org/)
- [Windows Credential Manager](https://learn.microsoft.com/en-us/windows/win32/api/wincred/nf-wincred-credwritew)
- [Task Scheduler logon trigger](https://learn.microsoft.com/en-us/windows/win32/taskschd/logon-trigger-example--c---)
- [Windows file locks](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-lockfileex)
