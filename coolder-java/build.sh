#!/usr/bin/env bash
set -euo pipefail
source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ ${1:-} == --help ]]; then
    printf 'Usage: %s [build-directory] [CMake options...]\nRequires a JDK with JNI headers; JAVA_HOME/JAVAC/JAVA/JOBS are supported.\n' "$0"
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
java_bin=${JAVA:-${JAVA_HOME:+$JAVA_HOME/bin/}java}
javac_bin=${JAVAC:-${JAVA_HOME:+$JAVA_HOME/bin/}javac}
arch_options=()
if [[ $(uname -s) == Darwin ]]; then
    java_arch=$("$java_bin" -XshowSettings:properties -version 2>&1 | awk '/os.arch =/{print $3}')
    case "$java_arch" in x86_64|amd64) arch_options=(-DCMAKE_OSX_ARCHITECTURES=x86_64);; aarch64|arm64) arch_options=(-DCMAKE_OSX_ARCHITECTURES=arm64);; esac
fi
"${CMAKE:-cmake}" -DCOOLDER_HTML_DIR="$source_dir/html" -P "$source_dir/../coolder/tools/bundle_frontend.cmake"
"${CMAKE:-cmake}" -S "$source_dir/../libai-java" -B "$output/native" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" -DLIBAI_CODING_FULL=ON -DJava_JAVA_EXECUTABLE="$(command -v "$java_bin")" \
    -DJava_JAVAC_EXECUTABLE="$(command -v "$javac_bin")" "${arch_options[@]}" "$@"
"${CMAKE:-cmake}" --build "$output/native" --parallel "${JOBS:-4}"
"${CMAKE:-cmake}" -S "$source_dir/../coolder/sandbox" -B "$output/helper" -DCMAKE_BUILD_TYPE=Release
"${CMAKE:-cmake}" --build "$output/helper" --parallel "${JOBS:-4}"
mkdir -p "$output/classes" "$dist_dir/bin" "$dist_dir/lib"
"$javac_bin" -encoding UTF-8 -source 8 -target 8 -d "$output/classes" \
    "$source_dir/../libai-java/src/main/java/io/coolder/libai/"*.java \
    "$source_dir/src/main/java/io/coolder/app/"*.java
# ZIP is the JAR format; this also supports JDK bundles without the jar utility.
"${PYTHON:-python3}" - "$output/classes" "$dist_dir/lib/coolder-java.jar" "$source_dir/../LICENSE" <<'PACKAGE'
from pathlib import Path
import sys
import zipfile
classes = Path(sys.argv[1])
with zipfile.ZipFile(sys.argv[2], 'w', zipfile.ZIP_DEFLATED) as jar:
    jar.writestr('META-INF/MANIFEST.MF', 'Manifest-Version: 1.0\r\n\r\n')
    jar.write(sys.argv[3], 'META-INF/LICENSE')
    for path in sorted(classes.rglob('*.class')):
        jar.write(path, path.relative_to(classes).as_posix())
PACKAGE
cp "$output/helper/webcool-sandbox-helper" "$dist_dir/bin/"
cp "$output/native/"*ai_jni.* "$dist_dir/lib/"
# Locate this APP's root html directory, also for a custom DIST_DIR.
printf '#!/usr/bin/env bash\ndefault_html=%q\n' "$source_dir/html" > "$dist_dir/bin/coolder-java"
cat >> "$dist_dir/bin/coolder-java" <<'LAUNCH'
set -euo pipefail
app_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
html_dir="$app_dir/../html"
if [[ ! -f "$html_dir/index.html" ]]; then html_dir=$default_html; fi
exec "${JAVA:-${JAVA_HOME:+$JAVA_HOME/bin/}java}" -Djava.library.path="$app_dir/lib" \
    -Dcoolder.html="$html_dir" -Dcoolder.helper="$app_dir/bin/webcool-sandbox-helper" -cp "$app_dir/lib/coolder-java.jar" io.coolder.app.Main "$@"
LAUNCH
chmod +x "$dist_dir/bin/coolder-java"
printf 'Build cache: %s\nRun: %s/bin/coolder-java\n' "$output" "$dist_dir"
