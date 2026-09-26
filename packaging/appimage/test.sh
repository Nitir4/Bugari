#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
image=$(realpath "${1:?Usage: test.sh path/to/AppImage}")
builder=${CONTAINER_ENGINE:-docker}
for distribution in ubuntu:24.04 debian:13-slim; do
    tag=$(printf '%s' "$distribution" | tr ':.' '--')
    "$builder" build --file "$project_dir/packaging/appimage/Dockerfile.test" \
        --build-arg "BASE_IMAGE=$distribution" --tag "ghm-appimage-test-$tag" "$project_dir"
    "$builder" run --rm --network none --volume "$image:/payload/CommitManager.AppImage:ro,Z" \
        "ghm-appimage-test-$tag" /payload/CommitManager.AppImage
done
