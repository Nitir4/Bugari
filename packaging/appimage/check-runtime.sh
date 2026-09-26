#!/bin/bash
set -euo pipefail
image=$(realpath "${1:?Usage: check-runtime.sh path/to/AppImage}")
sh /checks/check.sh "$image"
bash /checks/check-install.sh "$image"
temporary=$(mktemp -d "${TMPDIR:-/tmp}/ghm-image-gui-XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT
export XDG_DATA_HOME="$temporary/data" XDG_STATE_HOME="$temporary/state" XDG_CONFIG_HOME="$temporary/config"
set +e
dbus-run-session -- xvfb-run -a timeout 8s "$image" --appimage-extract-and-run >"$temporary/gui.log" 2>&1
status=$?
set -e
if [[ $status != 124 ]] || grep -Eq 'CRITICAL|undefined symbol|Segmentation fault|error while loading shared libraries|Failed to load icon|Unrecognized image file format|Fontconfig error' "$temporary/gui.log"; then
    cat "$temporary/gui.log"
    printf 'FAIL portable GUI status %s\n' "$status" >&2
    exit 1
fi
cat "$temporary/gui.log"
printf '%s\n' 'PASS portable GTK startup without distro GTK or application libraries'
