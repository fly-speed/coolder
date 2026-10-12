#!/usr/bin/env bash
set -euo pipefail
source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ ${1:-} == --help ]]; then
    printf 'Usage: %s [build-directory] [cargo build options...]\nRequires Cargo, CMake, a C++ compiler and prebuilt desktop ACL/OpenSSL.\n' "$0"
    exit 0
fi
output="$source_dir/build"
if (( $# )) && [[ $1 != -* ]]; then output=$1; shift; fi
mkdir -p -- "$output"
output=$(cd -- "$output" && pwd -P)
# Compilation caches and runnable files have separate lifetimes.
dist_dir=${DIST_DIR:-"$(dirname -- "$output")/dist"}
mkdir -p -- "$dist_dir"
dist_dir=$(cd -- "$dist_dir" && pwd -P)
case "$dist_dir/" in
    "$output/"*) printf 'DIST_DIR must be outside the build directory.\n' >&2; exit 2 ;;
esac
case "$output/" in
    "$dist_dir/"*) printf 'Build directory must be outside DIST_DIR.\n' >&2; exit 2 ;;
esac
export LIBAI_CODING_FULL=ON CMAKE_BUILD_PARALLEL_LEVEL=${JOBS:-4}
"${CMAKE:-cmake}" -DCOOLDER_HTML_DIR="$source_dir/html" -P "$source_dir/../coolder/tools/bundle_frontend.cmake"
"${CARGO:-cargo}" build --locked --release --manifest-path "$source_dir/Cargo.toml" --target-dir "$output/target" "$@"
"${CMAKE:-cmake}" -S "$source_dir/../coolder/sandbox" -B "$output/helper" -DCMAKE_BUILD_TYPE=Release
"${CMAKE:-cmake}" --build "$output/helper" --parallel "${JOBS:-4}"
mkdir -p "$dist_dir/bin" "$dist_dir/lib"
# Publish using fresh inodes: overwriting a signed Mach-O file in place can
# leave macOS executable-page signature caches pointing at the old contents.
# Atomic replacement also preserves files mapped by an already running APP.
publish_file() {
    local source=$1 destination=$2 staging name
    name=$(basename -- "$source")
    staging=$(mktemp -d "$dist_dir/.publish-XXXXXX")
    if ! cp -p -P -- "$source" "$staging/$name"; then
        rm -f -- "$staging/$name"
        rmdir -- "$staging"
        return 1
    fi
    if ! mv -f -- "$staging/$name" "$destination/$name"; then
        rm -f -- "$staging/$name"
        rmdir -- "$staging"
        return 1
    fi
    rmdir -- "$staging"
}
publish_file "$output/helper/webcool-sandbox-helper" "$dist_dir/bin"
publish_file "$output/target/release/coolder-rust" "$dist_dir/bin"
# Package the SDK for the executable-relative RPATH.
while IFS= read -r -d '' library; do publish_file "$library" "$dist_dir/lib"; done < <(
    find "$output/target/release/build" -path '*/install/lib/libai_c*' \( -type f -o -type l \) -print0)
while IFS= read -r -d '' library; do publish_file "$library" "$dist_dir/bin"; done < <(
    find "$output/target/release/build" -path '*/install/bin/ai_c.dll' -type f -print0)

printf 'Build cache: %s\nRun: %s/bin/coolder-rust\n' "$output" "$dist_dir"
