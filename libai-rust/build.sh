#!/usr/bin/env bash
# Build relative to this script, independently of the caller's working directory.
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ "${1:-}" == "--help" ]]; then
    printf 'Usage: %s [output-directory] [additional cargo build options...]\nDefaults to --release; set CARGO or LIBAI_CMAKE_TOOLCHAIN_FILE as needed.\n' "$0"
    exit 0
fi
output=${1:-"$script_dir/build"}
if (( $# )); then shift; fi
mkdir -p -- "$output"
output=$(cd -- "$output" && pwd)
# Cargo's target directory also contains the static native runtime built by build.rs.
"${CARGO:-cargo}" build --offline --release --manifest-path "$script_dir/Cargo.toml" \
    --target-dir "$output/target" "$@"
printf 'Rust library and native build outputs: %s/target\n' "$output"
