#!/bin/bash
set -euo pipefail
if [[ $# != 0 ]]; then printf '%s\n' 'Usage: AppImage --install' >&2; exit 1; fi
if [[ -z ${APPIMAGE:-} || ! -f $APPIMAGE ]]; then
    printf '%s\n' 'Run --install from the AppImage file, not from an extracted AppDir.' >&2
    exit 1
fi
data_dir=${XDG_DATA_HOME:-"$HOME/.local/share"}
config_dir=${XDG_CONFIG_HOME:-"$HOME/.config"}
case "$data_dir$config_dir" in *$'\n'*|*$'\r'*) printf '%s\n' 'Installation paths must not contain newlines.' >&2; exit 1;; esac
mkdir -p "$data_dir/ghm/appimage" "$data_dir/applications" \
    "$data_dir/icons/hicolor/scalable/apps" "$config_dir/systemd/user"
data_dir=$(cd -- "$data_dir" && pwd)
config_dir=$(cd -- "$config_dir" && pwd)
destination="$data_dir/ghm/appimage/GitHubCommitManager.AppImage"
unit="$config_dir/systemd/user/ghm-worker.service"
if [[ -e $unit ]] && ! head -1 "$unit" | grep -qx '# Managed by GitHub Commit Manager AppImage'; then
    printf 'Existing custom worker unit preserved: %s\n' "$unit" >&2
    printf '%s\n' 'Move that custom unit aside before opting into AppImage worker installation.' >&2
    exit 1
fi

# Quote paths for each file format; systemd and desktop files both interpret %.
desktop_escape() {
    local value=$1
    value=${value//\\/\\\\}; value=${value//\"/\\\"}
    value=${value//\$/\\\$}; value=${value//\`/\\\`}; value=${value//%/%%}
    printf '%s' "$value"
}
unit_escape() {
    local value=$1
    value=${value//\\/\\\\}; value=${value//\"/\\\"}
    value=${value//%/%%}; value=${value//\$/\$\$}
    printf '%s' "$value"
}
stage=$(mktemp -d "$data_dir/ghm/appimage/.install-XXXXXX")
trap 'rm -rf -- "$stage"' EXIT
cp -- "$APPIMAGE" "$stage/GitHubCommitManager.AppImage"
chmod 755 "$stage/GitHubCommitManager.AppImage"
cat > "$stage/ghm-worker.service" <<EOF
# Managed by GitHub Commit Manager AppImage
[Unit]
Description=GitHub Commit Manager scheduled commit worker (AppImage)

[Service]
Type=simple
ExecStart="$(unit_escape "$destination")" --appimage-extract-and-run --worker
Restart=on-failure
RestartSec=5s

[Install]
WantedBy=default.target
EOF
cat > "$stage/io.github.ghm.CommitManager.AppImage.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=GitHub Commit Manager (AppImage)
Comment=Manage local Git repositories and GitHub sign-in
Exec="$(desktop_escape "$destination")" --appimage-extract-and-run
Icon=io.github.ghm.CommitManager
Terminal=false
Categories=Development;
Keywords=Git;GitHub;Repository;Commit;
EOF
systemctl --user stop ghm-worker.service
mv -f -- "$stage/GitHubCommitManager.AppImage" "$destination"
install -m 644 "$stage/ghm-worker.service" "$unit"
install -m 644 "$stage/io.github.ghm.CommitManager.AppImage.desktop" "$data_dir/applications/"
install -m 644 "$APPDIR/io.github.ghm.CommitManager.svg" "$data_dir/icons/hicolor/scalable/apps/"
systemctl --user daemon-reload
systemctl --user enable --now ghm-worker.service
printf 'Installed %s\n' "$destination"
printf '%s\n' 'Existing repositories, database and keyring login are shared with the native installation.'
printf '%s\n' 'The worker uses extraction mode so it also works without FUSE. Lingering is unchanged.'
