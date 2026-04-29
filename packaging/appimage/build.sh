#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
client_id=${GHM_GITHUB_CLIENT_ID:-}
if [ -z "$client_id" ]; then
    printf '%s\n' 'Set GHM_GITHUB_CLIENT_ID to your public OAuth device-flow Client ID.' >&2
    exit 1
fi
case "$client_id" in *[!A-Za-z0-9]*) printf '%s\n' 'Invalid public Client ID.' >&2; exit 1;; esac
case "$(uname -m)" in x86_64) ;; *) printf '%s\n' 'This recipe currently builds x86_64 only.' >&2; exit 1;; esac
builder=${CONTAINER_ENGINE:-docker}
output_dir=${GHM_APPIMAGE_OUTPUT_DIR:-"$project_dir/dist"}
mkdir -p "$output_dir"
output_dir=$(CDPATH= cd -- "$output_dir" && pwd)
"$builder" build --file "$project_dir/packaging/appimage/Dockerfile" \
    --build-arg "GHM_GITHUB_CLIENT_ID=$client_id" --tag ghm-appimage-builder "$project_dir"
if [ "$(basename "$builder")" = podman ]; then
    "$builder" run --rm --network none --userns=keep-id --user "$(id -u):$(id -g)" \
        --volume "$output_dir:/output:Z" ghm-appimage-builder
else
    "$builder" run --rm --network none --user "$(id -u):$(id -g)" \
        --volume "$output_dir:/output:Z" ghm-appimage-builder
fi
printf 'AppImage and checksum written to %s\n' "$output_dir"
