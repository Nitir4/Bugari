#!/bin/bash
# Validate generated integration files with a fake user service manager.
set -euo pipefail
image=$(realpath "${1:?Usage: check-install.sh path/to/AppImage}")
temporary=$(mktemp -d "${TMPDIR:-/tmp}/ghm-install-check-XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT
# Exercise spaces, dollar, percent, quotes and backslashes in installation paths.
export XDG_DATA_HOME="$temporary/data space\$\%\"\\"
export XDG_CONFIG_HOME="$temporary/config space\$\%\"\\"
export XDG_STATE_HOME="$temporary/state"
mkdir -p "$temporary/bin"
cat > "$temporary/bin/systemctl" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$GHM_FAKE_SERVICE_LOG"
EOF
chmod +x "$temporary/bin/systemctl"
export GHM_FAKE_SERVICE_LOG="$temporary/systemctl.log"
export PATH="$temporary/bin:$PATH"
"$image" --appimage-extract-and-run --install
installed="$XDG_DATA_HOME/ghm/appimage/GitHubCommitManager.AppImage"
cmp "$image" "$installed"
desktop-file-validate "$XDG_DATA_HOME/applications/io.github.ghm.CommitManager.AppImage.desktop"
test -f "$XDG_CONFIG_HOME/systemd/user/ghm-worker.service"
grep -F -- '--appimage-extract-and-run --worker' "$XDG_CONFIG_HOME/systemd/user/ghm-worker.service" >/dev/null
grep -F -- '--user enable --now ghm-worker.service' "$GHM_FAKE_SERVICE_LOG" >/dev/null
"$installed" --appimage-extract-and-run --cli --help >/dev/null
# A custom unit must cause refusal before replacing the image or unit.
printf '%s\n' '# User custom unit' > "$XDG_CONFIG_HOME/systemd/user/ghm-worker.service"
if "$image" --appimage-extract-and-run --install; then exit 1; fi
test "$(cat "$XDG_CONFIG_HOME/systemd/user/ghm-worker.service")" = '# User custom unit'
printf '%s\n' 'PASS AppImage installation paths, stable copy, generated files and custom-unit refusal (no host service changes)'
