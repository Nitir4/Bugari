# GitHub Commit Manager

Native Linux repository manager, built with C17, GTK4, libgit2 and SQLite.
The project is in active development. The workbench and application dialogs
use dark mode with GTK's Adwaita theme.

The application opens a GTK4 window, initializes libgit2 and SQLite, discovers
local repositories, and shows working tree status. It supports GitHub login
through the system browser using OAuth device flow, lists repositories visible
to the account, and clones selected repositories into a managed local workspace.
It has a native dark workbench with an activity rail for Explorer, Source
Control, Commit History, Branches, Scheduled Jobs, and Settings. It creates
local commits with separate author and committer dates, fetches and safely
fast-forwards from `origin`, and pushes without force. A persistent user-level
worker executes frozen scheduled commits (and optional pushes) after the window
closes. The `ghm` CLI uses the same core library as the GTK application.

## Portable Linux package

The AppImage recipe produces a single executable for x86_64 Linux with glibc
2.39 or later, including Ubuntu 24.04 and recent Debian/Fedora systems. See
[AppImage build and usage](packaging/appimage/README.md) for the supported
baseline, build command, extraction fallback and optional worker installation.
The package does not require installing GTK or libgit2 on the target machine.

## Build

On Arch Linux:

```sh
sudo pacman -S --needed base-devel cmake pkgconf gtk4 libgit2 curl sqlite libsecret json-glib
cmake -S . -B build -DCMAKE_INSTALL_PREFIX="$HOME/.local" -DGHM_GITHUB_CLIENT_ID=YOUR_PUBLIC_OAUTH_CLIENT_ID
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build
```

Enable the per-user worker once after installation:

```sh
systemctl --user daemon-reload
systemctl --user enable --now ghm-worker.service
systemctl --user status ghm-worker.service
```

After installation, launch **GitHub Commit Manager** from your desktop's
application menu. The launcher, binary, and icon are installed under
`~/.local`. To run without installing, use `./build/ghm-gui`.

GTK 4.12 or later is required. On Fedora, install the dependencies before
running the CMake commands:

```sh
sudo dnf install gcc cmake make pkgconf-pkg-config gtk4-devel libgit2-devel libcurl-devel sqlite-devel libsecret-devel json-glib-devel
```

On Ubuntu 24.04 or later:

```sh
sudo apt install build-essential cmake pkg-config libgtk-4-dev libgit2-dev libcurl4-openssl-dev libsqlite3-dev libsecret-1-dev libjson-glib-dev
```

The build installs the GUI, CLI, worker, desktop launcher and icon. Use
`-DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release` for an installation without test
executables. A desktop D-Bus session and Secret Service keyring are required
for saved GitHub login. Local repositories can be used without signing in.

Application data is stored under `$XDG_DATA_HOME/ghm` (default:
`~/.local/share/ghm`). Repositories under `repos/` are discovered on launch;
other local repositories can be added with the window's **Add Local** button.

## Repository workflow

The workbench opens in **Explorer**, with a repository sidebar, file tree,
and editable text area. **Source Control** shows changed files beside their
colored diff. Fetch, Pull, and Push are in the compact workspace toolbar;
the blue status bar shows the branch and changed/staged counts.
Use Ctrl+1 through Ctrl+6 to switch views, Ctrl+Shift+G for Source Control,
Ctrl+B to toggle the repository sidebar, and Ctrl+S to save an open file.
This is a basic native file editor, not a full IDE: syntax highlighting,
multiple editor tabs, per-hunk staging, and a command palette remain future work.

After signing in, choose **GitHub Repositories** to list repositories visible
to the account. Select one and click **Clone / Open**. The clone is stored under
`$XDG_DATA_HOME/ghm/repos/OWNER/REPO` and reopened on later visits rather than
cloned again. Network work runs outside the GTK main loop.

The **Explorer** view browses the local working tree. It can create files and
folders, rename entries, delete files with confirmation, and edit/save UTF-8
text files up to 512 KiB. Saves check that the file has not changed on disk
since opening it. Double-click folders to expand them. The explorer lists at
most 5,000 entries for responsiveness; **Source Control** still shows all Git
status paths. Files are saved to the working tree immediately, but never
committed automatically. Save or discard editor changes before switching
repositories or scheduling a commit. In **Source Control**, use
**Stage** or **Unstage** on individual files, or **Stage All**. Select a changed
file for a bounded native diff preview (staged and working-tree changes are
shown separately; binary or very large diffs may not have a text preview).
**Commit Staged**
commits only staged content, leaving unstaged edits alone. It supports separate
author and committer metadata dates and an initial commit in an empty
repository. Set `user.name` and `user.email` in the Git configuration first.
Selecting a future metadata date does **not** schedule execution; use
**Schedule Commit** for that. **Push** sends the checked-out branch to `origin` without
force. GitHub HTTPS remotes use the OAuth token from the desktop keyring; local
filesystem remotes need no token. SSH and other hosts are not yet supported by
the push UI. **Fetch** updates remote-tracking information without changing
working files. **Pull** fast-forwards a clean checked-out branch only. It stops
on uncommitted changes, divergence, conflicts, detached HEAD, or pending
scheduled snapshots; it does not automatically merge or rebase. Use the
**Branches** view's **Merge into current** action (or `ghm branch merge NAME`)
to merge a *local* branch explicitly. Clean divergent merges create a
two-parent commit; fast-forward merges just advance the branch. Conflicts
are reported before any worktree or reference change and currently need
another Git client to resolve. A
non-fast-forward or rejected push fails without rewriting remote history.
Commit dates are Git metadata and do not change GitHub's push time.

The **Scheduled Jobs** view keeps job controls separate from the working-tree
view. **Commit History** shows the current branch's latest 200 commits with SHA,
message, author, author date, and committer date. Select a commit to inspect
its metadata. **Branches** lists local and remote branches, creates
local branches, and switches between local branches only when the worktree is
clean and the current branch has no pending or running scheduled commits.
Checkout, Merge, Pull, and Rewrite HEAD recognize scheduled jobs even when the
same worktree is opened through a relative path, `.git`, or a symlink. These
operations and the other mutations share a nonblocking repository lock across
the GUI, CLI, and worker. The lock uses the common Git directory, including
path aliases and linked worktrees. A busy operation asks you to retry; a busy
scheduled job remains pending. Native Git reference and index locks are also
reserved before changes. Checkout also refuses a branch occupied by another
linked worktree. A durable, worktree-specific recovery record covers commit
publication and checkout (including merge/pull checkout). After interruption,
the next mutation checks the recorded refs and index, repairs a recognized
partial operation, and removes only native locks whose recorded inodes still
match. Unexpected edits or locks stop recovery and leave an inspection error.
Other linked worktrees refuse mutations until the affected worktree recovers.
These protections do not make files, Git and SQLite one crash-atomic
transaction; external editors do not participate in the application lock.
Permission and available-space checks refuse known unsafe writes before
capturing a scheduled snapshot. Editor saves use an atomic replacement, so a
failed write preserves the previous file.

The GitHub repository picker includes search, description, visibility,
archive, stars, forks, and default-branch metadata. Select a repository
and choose **GitHub Branches** to inspect live branch names, head SHAs,
and protection flags without fetching or changing local Git. The CLI
equivalent is `ghm repo branches OWNER/NAME`.

**Rewrite HEAD…** in History is an advanced local-only operation that changes
the current commit's message and dates while preserving its files, parents,
and author/committer identities. It requires explicit confirmation, a clean
worktree, and no pending scheduled jobs. It refuses signed commits. This
changes the SHA. If the old commit was already pushed, ordinary Push will
reject the rewritten branch. The app does **not** offer force push for rewritten
history yet because lease-safe behavior has not been implemented and verified.

Creating an immediate commit advances the branch and may cause an older
scheduled job on that branch to fail its parent-safety check. Review any
pending scheduled jobs before committing now.

## Scheduled commits

Select a local repository, then click **Schedule Commit**. Set the message,
the Git metadata date/time, and the separate execution date/time. The app
freezes the selected file contents into a private Git tree immediately, without
changing the normal Git index. By default it privately stages all current
non-ignored working-tree changes at scheduling time; uncheck **Capture all
current changes now** to use only the files you manually staged in Source
Control. The worker performs no `git add` later. A later
scheduled job on the same branch builds on that earlier frozen tree, so later
edits do not enter the first commit. Files staged separately are preserved.
If a file exists only in an earlier pending snapshot and you remove it before
the next job, staged-only mode cannot represent that deletion in the ordinary
index; the app rejects the job and asks you to use **Capture all current
changes** instead.

Jobs are stored in SQLite and run by `ghm-worker`, which is independent of the
GTK window. **Refresh Jobs** shows pending, completed, failed, and cancelled
jobs. Pending jobs can be rescheduled, have their message/push choice edited,
or be cancelled. Changing the frozen file snapshot requires cancelling and
creating a new job. A job with dependent pending jobs cannot be cancelled or
rescheduled until the later jobs are cancelled first. A job
will fail safely if its branch/parent changed or a merge is in progress.
Before publishing a scheduled commit, the worker stores its planned SHA.
Restart recovery recognizes that exact published commit and records completion
without creating it twice. An interrupted unpublished job fails for inspection.
The worker does not silently rewrite history. Checking **Push this commit to
origin** schedules a separate non-force push of exactly that new commit, after
the local commit is recorded. If the branch advances before the push, the push
fails rather than including later commits. The job's local commit remains
completed even if its push fails; **Retry Push** safely requeues that exact
commit. Scheduled network/server failures remain queued and retry automatically
with a 15–300 second backoff. Linux connectivity events and service startup
make offline pushes eligible immediately; a timer provides a fallback.
Rate limits use their available retry/reset deadline and a longer backoff;
repeated rate limits pause automatic attempts and allow **Retry Push**.
Authentication, permission and permanent remote errors require manual correction.
A persistent amber warning makes rate limits visible across workbench views,
and jobs show push attempts and the next retry time. Worker database changes
refresh the displayed jobs and warnings automatically. A valid saved GitHub
login with permission to access a private repository is required for GitHub
HTTPS remotes.

The user service runs while your user systemd manager is active. If the
machine is off or the user manager has stopped, overdue jobs run at the next
login/start of the service, not at the missed time. To keep the user manager
running after logout as well, the user can explicitly opt in with
`loginctl enable-linger "$USER"`; the app does not change that setting.
For a laptop, suspend also delays jobs until resume. A service restart recovers
pending jobs and already-published scheduled commits. Unrecognized interrupted
commit state remains an inspection failure.
Interrupted pushes are retried on worker restart because exact-commit,
non-force pushes are idempotent.
Recovery leaves RUNNING jobs alone while another process holds their repository
lock. Contention retries use a timer with bounded backoff; an unrelated unlocked
repository can still run its due jobs.

## Development and tests

The CTest suite covers repository operations, frozen scheduling, linked
worktrees, cross-process locking, interrupted mutations, database failures and
push retries. Tests use disposable repositories rather than your account or
normal application database. See [tests/README.md](tests/README.md) for manual
GUI stress and isolated network/filesystem checks.

Portable Linux packaging is described in
[packaging/appimage/README.md](packaging/appimage/README.md).

## CLI and logs

`ghm --help` lists the native CLI commands. Examples:

```sh
ghm repo list
ghm repo status .
ghm commit --message "Implement login" --date "2026-09-20T14:30:00+05:30"
ghm schedule --message "Later snapshot" --at "$(date -d '+1 hour' --iso-8601=seconds)" --push
ghm schedule list
ghm fetch
ghm pull
ghm branch merge feature
ghm repo branches OWNER/NAME
ghm history
ghm branch create feature/login
```

`ghm commit` commits currently staged files; add `--all` to stage all current
changes first. `ghm schedule` freezes all current changes by default; use
`--staged` to capture only files already staged. `ghm repo open` prints the
local path because a CLI cannot change its parent shell's current directory.
`ghm login` prints GitHub's verification URL and one-time code, then waits for
approval using the same secure core as the GUI. Logs are JSON Lines at
`$XDG_STATE_HOME/ghm/ghm.log` (default `~/.local/state/ghm/ghm.log`) and are
viewable from **Settings → Refresh Developer Log**. Tokens are never logged.

## GitHub login

The app developer registers a [GitHub OAuth app](https://docs.github.com/en/apps/oauth-apps/building-oauth-apps/creating-an-oauth-app),
enables **Device Flow**, and sets its public Client ID at build time with
`-DGHM_GITHUB_CLIENT_ID=...`. No client secret is needed. For a local development
build, the `GHM_GITHUB_CLIENT_ID` environment variable can override the built-in
ID. Builds without a configured ID disable the login button. Existing installs
can still use a previously saved public Client ID.

Users simply click **Login with GitHub**, then sign in and approve access in
their system browser using the one-time code shown in the app. The app never
asks for a GitHub password or personal access token. Access and refresh tokens
are saved only in the desktop secret service through libsecret, not SQLite. A
working Secret Service keyring is required to persist the login.

Choose **Public repositories only** to request `public_repo read:user`, or
**Public and private repositories** to request `repo read:user`. The latter is
the broader OAuth scope needed for private repository access. The app opens the
system browser at GitHub's device sign-in page and displays a code to enter
there. It respects GitHub's polling interval and refreshes expiring tokens on
subsequent starts.
