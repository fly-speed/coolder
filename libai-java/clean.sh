#!/usr/bin/env bash
# Remove only the named outputs produced by this directory's build.sh.
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
if [[ "${1:-}" == "--help" ]]; then
    printf 'Usage: %s [output-directory]\n       %s --output output-directory\n' "$0" "$0"
    exit 0
fi
if [[ "${1:-}" == "--output" ]]; then
    shift
    if (( $# != 1 )); then
        printf 'Provide one output directory after --output.\n' >&2
        exit 2
    fi
fi
if (( $# == 1 )) && [[ -z "$1" ]]; then
    printf 'Output directory must not be empty.\n' >&2
    exit 2
fi
if (( $# > 1 )) || [[ ${1:-valid} == -* ]]; then
    printf 'Expected one output directory; use --help for usage.\n' >&2
    exit 2
fi
output=${1:-"$script_dir/build"}
generated=("cmake" "install" "classes")
if [[ ! -e "$output" && ! -L "$output" ]]; then
    printf 'No build output: %s\n' "$output"
    exit 0
fi
if [[ ! -d "$output" || -L "$output" ]]; then
    printf 'Output must be a directory, not a file or symlink: %s\n' "$output" >&2
    exit 1
fi
output=$(cd -- "$output" && pwd -P)
# Reject source roots and their ancestors before considering any removal.
repo_dir=$(cd -- "$script_dir/.." && pwd -P)
case "$repo_dir/" in
    "$output/"*) printf 'Refusing to clean a source directory or its ancestor: %s\n' "$output" >&2; exit 1 ;;
esac
if [[ "$output" == / || "$output" == "$script_dir" || "$output" == "${HOME:-}" ||
      -e "$output/.git" || -f "$output/build.sh" ]]; then
    printf 'Refusing to clean a home/root/repository directory: %s\n' "$output" >&2
    exit 1
fi
for name in "${generated[@]}"; do
    rm -rf -- "$output/$name"
done
# Keep an output directory containing unrelated files. Failure includes nonempty dirs.
rmdir -- "$output" 2>/dev/null || true
printf 'Cleaned build outputs: %s\n' "$output"
