#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
target=${1:-all}
case "$target" in all) targets='fedora debian arch';; fedora|debian|arch) targets=$target;; *) printf 'Usage: %s [all|fedora|debian|arch]\n' "$0" >&2; exit 2;; esac
case "$(uname -m)" in x86_64) ;; *) printf '%s\n' 'Native release builds currently support x86_64 only.' >&2; exit 1;; esac
builder=${CONTAINER_ENGINE:-docker}
output_dir=${GHM_PACKAGE_OUTPUT_DIR:-"$project_dir/dist/linux"}
mkdir -p "$output_dir"
output_dir=$(CDPATH= cd -- "$output_dir" && pwd)
input_dir=$(mktemp -d "${TMPDIR:-/tmp}/ghm-package-input.XXXXXXXX")
if [ "${GHM_LOCAL_ONLY:-0}" = 1 ]; then
    printf '%s\n' 'LOCAL TEST BUILD ONLY: do not upload these artifacts to public repositories.'
    python3 "$project_dir/packaging/linux/prepare.py" "$input_dir" --local-only
else
    python3 "$project_dir/packaging/linux/prepare.py" "$input_dir"
fi
for distro in $targets; do
    distro_output="$output_dir/$distro"
    mkdir -p "$distro_output"
    if [ -n "$(find "$distro_output" -mindepth 1 -maxdepth 1 -print -quit)" ]; then
        printf 'Output directory must be empty: %s (choose GHM_PACKAGE_OUTPUT_DIR for a new build)\n' "$distro_output" >&2
        exit 1
    fi
    "$builder" build --file "$project_dir/packaging/linux/Dockerfile.$distro" \
        --tag "ghm-native-$distro" "$project_dir"
    "$builder" run --rm --network none \
        --env "OUTPUT_UID=$(id -u)" --env "OUTPUT_GID=$(id -g)" \
        --volume "$input_dir:/input:ro,Z" --volume "$distro_output:/output:Z" \
        "ghm-native-$distro"
    printf '%s packages and checksums written to %s\n' "$distro" "$distro_output"
done
printf 'Release source snapshot retained in %s\n' "$input_dir"
