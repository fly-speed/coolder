#!/usr/bin/env bash
# Build relative to this script, independently of the caller's working directory.
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ "${1:-}" == "--help" ]]; then
    printf 'Usage: %s [output-directory] [additional CMake options...]\n' "$0"
    exit 0
fi
output=${1:-"$script_dir/build"}
if (( $# )); then shift; fi
mkdir -p -- "$output"
output=$(cd -- "$output" && pwd)
cmake_bin=${CMAKE:-cmake}
build_type=${BUILD_TYPE:-Release}
# A JDK supplies javac and JNI headers; CMake options can override JNI paths.
if [[ -n ${JAVA_HOME:-} ]]; then
    javac_bin=${JAVAC:-"$JAVA_HOME/bin/javac"}
else
    javac_bin=${JAVAC:-javac}
fi
java_bin=${JAVA:-${JAVA_HOME:+$JAVA_HOME/bin/}java}
arch_options=()
if [[ $(uname -s) == Darwin ]]; then
    java_arch=$("$java_bin" -XshowSettings:properties -version 2>&1 | awk '/os.arch =/{print $3}')
    case "$java_arch" in x86_64|amd64) arch_options=(-DCMAKE_OSX_ARCHITECTURES=x86_64);; aarch64|arm64) arch_options=(-DCMAKE_OSX_ARCHITECTURES=arm64);; esac
fi
"$cmake_bin" -S "$script_dir" -B "$output/cmake" -DCMAKE_BUILD_TYPE="$build_type" \
    -DJava_JAVA_EXECUTABLE="$(command -v "$java_bin")" -DJava_JAVAC_EXECUTABLE="$(command -v "$javac_bin")" \
    "${arch_options[@]}" "$@"
"$cmake_bin" --build "$output/cmake" --config "$build_type" --parallel "${JOBS:-4}"
"$cmake_bin" --install "$output/cmake" --config "$build_type" --prefix "$output/install"
printf 'Libraries and headers: %s/install\n' "$output"
mkdir -p -- "$output/classes"
"$javac_bin" -encoding UTF-8 -source 8 -target 8 -d "$output/classes" \
    "$script_dir/src/main/java/io/coolder/libai/"*.java
# ZIP is the JAR container format; this also works with JDKs lacking the jar utility.
"${PYTHON:-python3}" - "$output" "$script_dir/../LICENSE" <<'PACKAGE'
from pathlib import Path
import sys
import zipfile
output = Path(sys.argv[1])
with zipfile.ZipFile(output / 'install/libai-java.jar', 'w', zipfile.ZIP_DEFLATED) as jar:
    jar.writestr('META-INF/MANIFEST.MF', 'Manifest-Version: 1.0\r\n\r\n')
    jar.write(sys.argv[2], 'META-INF/LICENSE')
    for path in sorted((output / 'classes/io').rglob('*.class')):
        jar.write(path, path.relative_to(output / 'classes').as_posix())
PACKAGE
printf 'Java library: %s/install/libai-java.jar\n' "$output"
