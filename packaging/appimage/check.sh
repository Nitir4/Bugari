#!/bin/sh
# Packaging smoke test: disposable state, no account, no host service changes.
set -eu
image=$(realpath "${1:?Usage: check.sh path/to/AppImage}")
temporary=$(mktemp -d "${TMPDIR:-/tmp}/ghm-portable-check-XXXXXX")
worker_pid=
worker_unit=
cleanup() {
    if [ -n "$worker_unit" ]; then systemctl --user stop "$worker_unit" 2>/dev/null || true; fi
    if [ -n "$worker_pid" ]; then
        kill "$worker_pid" 2>/dev/null || true
        wait "$worker_pid" 2>/dev/null || true
    fi
    rm -rf -- "$temporary"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM
export XDG_DATA_HOME="$temporary/data" XDG_STATE_HOME="$temporary/state" XDG_CONFIG_HOME="$temporary/config"
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null
unset GHM_GITHUB_CLIENT_ID
cli() { "$image" --appimage-extract-and-run --cli "$@"; }
cli --help >/dev/null
repository="$XDG_DATA_HOME/ghm/repos/portable-fixture"
mkdir -p "$(dirname "$repository")"
git init -q -b main "$repository"
git init -q --bare "$temporary/remote.git"
cd "$repository"
git config user.name 'Portable test'
git config user.email 'portable@example.invalid'
git remote add origin "$temporary/remote.git"
printf '%s\n' base > notes.txt
git add notes.txt
cli commit --message 'portable initial' >/dev/null
cli repo list >/dev/null
test "$(git log -1 --format=%s)" = 'portable initial'
cli branch create portable >/dev/null
printf '%s\n' frozen > notes.txt
execute_at=$(date -u -d '+5 seconds' '+%Y-%m-%dT%H:%M:%S+00:00')
cli schedule --message 'portable scheduled' --at "$execute_at" --push >/dev/null
printf '%s\n' later > notes.txt
if [ "${GHM_CHECK_SYSTEMD:-0}" = 1 ]; then
    worker_unit="ghm-appimage-check-$$.service"
    systemd-run --user --collect --unit "$worker_unit" \
        --setenv "XDG_DATA_HOME=$XDG_DATA_HOME" --setenv "XDG_STATE_HOME=$XDG_STATE_HOME" \
        --setenv "XDG_CONFIG_HOME=$XDG_CONFIG_HOME" \
        --property "StandardOutput=append:$temporary/worker.log" \
        --property "StandardError=append:$temporary/worker.log" \
        "$image" --appimage-extract-and-run --worker
else
    "$image" --appimage-extract-and-run --worker >"$temporary/worker.log" 2>&1 &
    worker_pid=$!
fi
attempt=0
until [ "$(git log -1 --format=%s)" = 'portable scheduled' ] &&
      [ "$(git rev-parse HEAD)" = "$(git --git-dir="$temporary/remote.git" rev-parse refs/heads/main 2>/dev/null || true)" ]; do
    attempt=$((attempt + 1))
    if [ "$attempt" -gt 30 ]; then cat "$temporary/worker.log"; cli schedule list; exit 1; fi
    sleep 1
done
test "$(git show HEAD:notes.txt)" = frozen
test "$(cat notes.txt)" = later
test "$(git rev-list --count HEAD)" = 2
cli schedule list | grep COMPLETED >/dev/null
printf '%s\n' 'PASS AppImage CLI, branch, frozen scheduled commit and exact local push using the packaged worker'
