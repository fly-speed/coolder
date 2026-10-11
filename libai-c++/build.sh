#!/usr/bin/env bash
# Build relative to this script, independently of the caller's working directory.
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ "${1:-}" == "--help" ]]; then
    printf 'Usage: %s [output-directory] [additional CMake options...]\nDefault: static library. Add -DBUILD_SHARED_LIBS=ON for a shared library.\n' "$0"
    exit 0
fi
output="$script_dir/build"
if (( $# )) && [[ "$1" != -* ]]; then
    output=$1
    shift
fi
mkdir -p -- "$output"
output=$(cd -- "$output" && pwd)
cmake_bin=${CMAKE:-cmake}
build_type=${BUILD_TYPE:-Release}
"$cmake_bin" -S "$script_dir" -B "$output/cmake" \
    -DCMAKE_BUILD_TYPE="$build_type" -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_LIBDIR=lib -DLIBAI_CPP_BUILD_TESTS=OFF -DLIBAI_CPP_BUILD_EXAMPLES=ON "$@"
"$cmake_bin" --build "$output/cmake" --config "$build_type" --parallel "${JOBS:-4}"
"$cmake_bin" --install "$output/cmake" --config "$build_type" --prefix "$output/install"
printf 'Libraries and headers: %s/install\n' "$output"
