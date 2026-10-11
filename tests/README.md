# Tests

Build with CMake's default `BUILD_TESTING=ON`, then run:

```sh
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The 23 registered Linux tests use disposable repositories and databases. They cover
commits, branches, merging, history, diffs, staging, file operations, frozen
snapshots, scheduling, locks, linked worktrees, interrupted mutation recovery,
SQLite failures, authentication response handling and push retries. Fault
injection is compiled into a separate, uninstalled test library. Production
binaries do not support test fault controls.

Windows registers authentication response, datetime, synthetic credential and
portable integration tests. The portable test also runs on Linux and covers
staging, Unicode paths, safe editor saves, process locks, interrupted commit
publication, frozen/chained jobs, background execution, exact local pushes,
and SQLite completion recovery. See [Windows validation](../packaging/windows/README.md)
for native CI and package checks. Linux-specific process/namespace tests are
not silently counted as Windows coverage.

## GUI workflow and stress

The workflow harness builds on Linux and Windows. On Linux, run from a
desktop session or an Xvfb display:

```sh
GSK_RENDERER=cairo ./build/ghm-gui-workflow
```

This harness drives real GTK widgets in an isolated application workspace. It
covers the editor, files, staging, commit dates, branches, merge, local history
rewrite, scheduled jobs, retry confirmations, warnings, shortcuts and window
closure during asynchronous work. Stress includes rapid view switching and a
large file tree. It does not sign in or write to a hosted repository. Screenshots
are written under the system temporary directory; `GHM_GUI_TEST_OUTPUT_DIR`
selects an evidence directory. Windows CI also runs the harness against the
bundled runtime with MSYS2 removed from PATH and uploads logs/screenshots.
This validates native widgets and local workflows on Windows Server, while
Windows 10/11 browser login and session/power checks remain manual. Slow virtual machines may set
`GHM_GUI_TEST_TIMEOUT_SECONDS` to a bounded value between 15 and 120.

`ghm-gui-smoke` provides an additional manual display harness. Its defaults use
a temporary workspace. Do not point manual tools at a repository containing
work you need to preserve.

## Filesystem and network failures

On Linux with unprivileged user/mount/network namespaces enabled:

```sh
sh tests/run_isolated_failures.sh
```

The script creates a private small filesystem for real ENOSPC failures and a
private disconnected network for libcurl/libgit2 and worker connectivity
checks. It does not fill the host disk or change host networking. It requires
`unshare`, `mount`, `ip`, and the built test/worker executables. See the script
for build-directory overrides.

Sanitizer builds can use `-fsanitize=address,undefined -fno-omit-frame-pointer`.
Run tests under the appropriate sanitizer environment; GTK and graphics
dependencies can require disabling leak detection. That does not establish
leak freedom.
