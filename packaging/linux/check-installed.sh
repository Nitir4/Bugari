#!/bin/sh
set -eu
temporary=$(mktemp -d /tmp/ghm-native-check.XXXXXXXX)
export XDG_DATA_HOME="$temporary/data" XDG_STATE_HOME="$temporary/state" XDG_CONFIG_HOME="$temporary/config"
for executable in ghm ghm-gui ghm-worker; do
    test -x "/usr/bin/$executable"
    ldd "/usr/bin/$executable" > "$temporary/$executable.ldd"
    if grep -q 'not found' "$temporary/$executable.ldd"; then
        cat "$temporary/$executable.ldd"
        exit 1
    fi
done
ghm --help
desktop-file-validate /usr/share/applications/io.github.ghm.CommitManager.desktop
test -s /usr/share/icons/hicolor/scalable/apps/io.github.ghm.CommitManager.svg
test -s /usr/lib/systemd/user/ghm-worker.service
grep -q '^ExecStart=/usr/bin/ghm-worker$' /usr/lib/systemd/user/ghm-worker.service
test ! -e /etc/systemd/user/default.target.wants/ghm-worker.service
set +e
dbus-run-session -- xvfb-run -a timeout 8s env GSK_RENDERER=cairo ghm-gui > "$temporary/gui.log" 2>&1
status=$?
set -e
if [ "$status" -ne 124 ] || grep -Eq 'CRITICAL|undefined symbol|Segmentation fault|error while loading shared libraries|Failed to load icon|Unrecognized image file format|Fontconfig error' "$temporary/gui.log"; then
    cat "$temporary/gui.log"
    printf 'FAIL native GTK startup (status %s)\n' "$status" >&2
    exit 1
fi
printf '%s\n' 'PASS native package: dependency resolution, CLI, desktop entry, icon, opt-in worker, GTK startup'
