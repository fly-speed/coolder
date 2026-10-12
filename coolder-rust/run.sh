#!/usr/bin/env bash
set -euo pipefail
app_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
dist_dir=${DIST_DIR:-"$app_root/dist"}
# Resolve custom relative release paths before changing the working directory.
if [[ "$dist_dir" != /* ]]; then dist_dir="$PWD/$dist_dir"; fi
executable="$dist_dir/bin/coolder-rust"
if [[ ! -x "$executable" ]]; then
    printf 'APP executable not found: %s\nRun %s/build.sh first, or set DIST_DIR to your release directory.\n' "$executable" "$app_root" >&2
    exit 1
fi
# Default data and logs belong to this APP, independently of the calling directory.
cd -- "$app_root"
exec "$executable" --html "$app_root/html" "$@"
