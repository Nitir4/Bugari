# Windows desktop acceptance

Status: **pending**. Run this on an ordinary Windows 10 or Windows 11 desktop
with the candidate ZIP, without MSYS2 or application DLLs installed elsewhere.
Record one report per Windows version for the same package source commit.
GitHub Actions validates native code, packaged GTK workflows and Task Scheduler
on Windows Server; it does not replace these desktop/session checks.

## Identify the build

Download `GitHubCommitManager-windows-x86_64-candidate` from a successful
[Windows candidate run](https://github.com/Nitir4/Bugari/actions/workflows/windows.yml).
Extract the artifact, check the ZIP, then extract the entire application ZIP:

```powershell
Get-FileHash .\GitHubCommitManager-0.1.0-windows-x86_64.zip -Algorithm SHA256
Get-Content .\GitHubCommitManager-0.1.0-windows-x86_64.zip.sha256
```

The two hashes must match. Inside the extracted application folder,
`build-info.json` records `source_commit`. Keep the extracted package for the
uninstall step. Run PowerShell as your normal user, without administrator rights.
Use a disposable repository and configure its Git author name/email; Git for
Windows may be used to create this fixture, but MSYS2 must not supply app DLLs.
Do not perform these checks in a repository containing valuable work.

## Checks

1. **Clean launch and local editing.** Launch `bin\ghm-gui.exe` from Explorer
   in a directory containing spaces and a Unicode character. Choose **Continue
   with local repositories**, add the disposable repository, and verify fonts,
   icons, dialogs and all six views. Create, edit, save, rename and delete a
   test file. Stage it, commit, and check history. Try a branch checkout and
   merge using disposable changes. Confirm unsaved edits block closing the
   window until saved or discarded.
2. **GitHub browser login.** Choose **Login with GitHub** and public access.
   Verify the system browser opens the device sign-in page; authorize your
   own account and confirm the repository picker loads. Clone a disposable
   repository and inspect its live GitHub branches. Keep TLS verification
   enabled. A disabled login button is a failed release check, not a pass.
3. **Saved login.** Close all app windows and reopen the application. Verify
   login is restored and the GitHub repository list loads without repeating
   device authorization. Do not copy Credential Manager secrets into reports.
4. **Installed background execution.** Close all windows and run:

   ```powershell
   powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\install.ps1 -InstallWorker
   ```

   Open the Start menu shortcut. Schedule a uniquely named commit two minutes
   ahead in the disposable repo, then edit the file again and close the GUI.
   After the deadline, reopen it. The job must be completed exactly once with
   the frozen content; the later edit must remain outside that commit. Task
   Scheduler should show one `GitHubCommitManager-...` task under your identity.
5. **Logout/login and overdue execution.** Save all unrelated work. Schedule
   another uniquely named job two minutes ahead and sign out of Windows before
   its deadline. Wait until it is overdue, sign in again, and launch the Start
   menu shortcut. The worker must restart, complete the job once, and preserve
   saved GitHub login. Record scheduled and observed completion times.
6. **Sleep/resume.** Schedule another job, close the GUI and sleep the computer
   before its deadline. Resume after it is overdue. Confirm the worker completes
   it once and retains the frozen snapshot. Jobs cannot run during sleep; record
   the resume and completion times rather than expecting the missed deadline.
7. **Push and retry.** Use a disposable GitHub repository you own and explicitly
   enable a scheduled push. Check that the scheduled SHA reaches the remote.
   Repeat with a new job while offline: its local commit must complete, its push
   must remain queued, and restoring connectivity must send that same commit
   without duplication or later edits. Record SHAs and retry state.
8. **Uninstall and retained data.** Close the GUI and run:

   ```powershell
   powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1 -RemoveFiles
   ```

   Confirm the managed task, shortcut and installed versions are removed.
   Reopen the original portable package and check repositories, jobs and saved
   login are retained. The portable GUI must not restart an uninstalled worker.

Save each result before signing out or sleeping. Resume with the next check
after reopening this document. A failure or a skipped check keeps acceptance
pending. Fix failures and repeat affected checks against a new candidate.

## Result template

Copy this section to a separate report outside the extracted package. Record
observations and timestamps, not just checkmarks. Do not include account names,
device codes, passwords or tokens. Redact identifying log paths before sharing.

- Windows version/edition/build: PENDING
- Clean desktop without MSYS2: PENDING
- Source commit from `build-info.json`: PENDING
- ZIP SHA-256: PENDING
- Successful CI run URL: PENDING
- Test date and time zone: PENDING

| Check | Result (PASS / FAIL / SKIPPED) | Observations / evidence |
| --- | --- | --- |
| Clean launch and local editing | PENDING | |
| GitHub browser/device login | PENDING | |
| Saved login after reopening | PENDING | |
| Installed worker with GUI closed | PENDING | |
| Logout/login and overdue job | PENDING | |
| Sleep/resume and overdue job | PENDING | |
| Exact GitHub push and offline retry | PENDING | |
| Uninstall and preserved data/login | PENDING | |

Overall result: **PENDING**. Only mark PASS when every check passes on this
machine. A report for Windows Server or Wine is automated/compatibility
evidence and cannot be recorded as Windows 10/11 desktop acceptance.
