#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
target=${1:-all}
case "$target" in all) targets='fedora debian arch';; fedora|debian|arch) targets=$target;; *) printf 'Usage: %s [all|fedora|debian|arch]\n' "$0" >&2; exit 2;; esac
builder=${CONTAINER_ENGINE:-docker}
output_dir=${GHM_PACKAGE_OUTPUT_DIR:-"$project_dir/dist/linux"}
for distro in $targets; do
    case "$distro" in
        fedora) base=fedora:43; pattern='ghm-[0-9]*.x86_64.rpm' ;;
        debian) base=debian:13-slim; pattern='ghm_[0-9]*_amd64.deb' ;;
        arch) base=archlinux:base; pattern='ghm-[0-9]*-x86_64.pkg.tar.zst' ;;
    esac
    context=$(mktemp -d "${TMPDIR:-/tmp}/ghm-package-verify.XXXXXXXX")
    cp "$project_dir/packaging/linux/check-installed.sh" "$context/"
    package=$(find "$output_dir/$distro" -maxdepth 1 -type f -name "$pattern")
    if [ -z "$package" ] || [ "$(printf '%s\n' "$package" | wc -l)" -ne 1 ]; then
        printf 'Expected one main %s package in %s/%s\n' "$distro" "$output_dir" "$distro" >&2
        exit 1
    fi
    cp "$package" "$context/"
    "$builder" build --file "$project_dir/packaging/linux/Dockerfile.verify" \
        --build-arg "BASE_IMAGE=$base" --build-arg "DISTRO=$distro" \
        --tag "ghm-native-verify-$distro" "$context"
    "$builder" run --rm --network none "ghm-native-verify-$distro"
done
