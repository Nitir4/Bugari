# GitHub Commit Manager 0.1.0 release preparation

Status: **Windows test candidate; supported release pending desktop acceptance**.

## Candidate features

- Native GTK4 workbench, file editing, Source Control, history and branches.
- GitHub OAuth device login with Windows Credential Manager persistence.
- Repository listing/cloning and non-force GitHub HTTPS pushes.
- Shared CLI and SQLite-backed worker for frozen, chained scheduled commits.
- Commit publication recovery, process locks and exact-commit push retries.
- Portable x86_64 Windows ZIP with GTK resources and dependency notices.
- Per-user installation, Start menu shortcut and optional Task Scheduler worker.
- Uninstall preserves repositories, job data and saved login.

## Validation evidence

The [5 October native CI run](https://github.com/Nitir4/Bugari/actions/runs/37295608677)
for `1a522b40202a0b06ec39a129616742da0ed08bfb` passed 23 Linux tests, four
Windows tests, production GUI startup without MSYS2 on PATH, and real Task
Scheduler installation, startup and uninstall.

The updated workflow also runs the native GTK workflow/stress harness against
the packaged runtime and uploads screenshots and logs as
`windows-gui-validation`. Use the successful run for the exact candidate
commit as evidence; a prior run does not validate changed code.

Windows 10 and Windows 11 desktop results are **pending**. Record them using
[desktop acceptance](DESKTOP-ACCEPTANCE.md). Server CI does not validate browser
authorization, persisted real login, logout/login or sleep/resume on a client
desktop. No supported Windows release is claimed by these notes.

## Remaining release work

1. Pass CI for the candidate commit and preserve its artifacts and evidence.
2. Pass desktop acceptance on Windows 10 and Windows 11 for that candidate.
3. Resolve failures and repeat affected checks on any replacement build.
4. Merge the reviewed Windows changes into `master`, run CI for the resulting
   commit and use that commit's package. Repeat acceptance if application or
   packaging behavior changes during integration.
5. Publish the ZIP and its SHA-256 with the tested commit, validation links and
   known limitations. Until acceptance passes, distribute only as a candidate.

## Known limitations

- Worker execution requires the user to be signed in and the machine awake;
  overdue jobs run when the worker next starts or resumes.
- Windows automated coverage remains smaller than Linux coverage.
- Merge conflicts need another Git client. Pull only fast-forwards a clean
  branch. SSH push and force push are not available in the GUI.
- The editor has no syntax highlighting, tabs or per-hunk staging.
- The initial package is unsigned; code signing is not part of this candidate.
- Windows flush/replacement behavior does not promise identical power-loss
  durability to POSIX filesystems.
