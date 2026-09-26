#!/bin/sh
set -eu
# Both mounts and networking belong only to disposable child namespaces.
project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${1:-"$project_root/build"}
initial_netns=$(readlink /proc/self/ns/net)
unshare -Ur -m sh -c 'mount --make-rprivate / && mount -t tmpfs -o size=8m tmpfs /tmp && exec "$1/ghm-full-disk-test"' sh "$build_dir"
unshare -Ur -n env GHM_TEST_ROUTE_EVENT=1 GHM_TEST_INITIAL_NETNS="$initial_netns" "$build_dir/ghm-worker-retry-test" "$build_dir/ghm-worker"
