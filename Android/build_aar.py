#!/usr/bin/env python3
"""Build an offline AAR from the portable JNI binding and an installed NDK/JDK."""
import argparse
import os
from pathlib import Path
import subprocess
import zipfile


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', default=os.environ.get('ANDROID_NDK_HOME'))
    parser.add_argument('--java-home', default=os.environ.get('JAVA_HOME'))
    parser.add_argument('--abis', nargs='+', default=['arm64-v8a', 'x86_64'])
    parser.add_argument('--api', type=int, default=21)
    parser.add_argument('--output', type=Path, default=Path('build/android'))
    args = parser.parse_args()
    if not args.ndk or not args.java_home:
        parser.error('provide --ndk and --java-home (or environment variables)')
    if args.api < 21:
        parser.error('API level must be at least 21')
    root = Path(__file__).resolve().parent.parent
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    classes = output / 'classes'
    classes.mkdir(exist_ok=True)
    java = Path(args.java_home) / 'bin'
    sources = sorted((root / 'libai-java/src/main/java').rglob('*.java'))
    run(java / 'javac', '-encoding', 'UTF-8', '-source', '8', '-target', '8', '-d', classes, *sources)
    jar = output / 'classes.jar'
    with zipfile.ZipFile(jar, 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(classes.rglob('*.class')):
            archive.write(path, path.relative_to(classes).as_posix())
    libraries = []
    for abi in args.abis:
        if abi not in ('arm64-v8a', 'armeabi-v7a', 'x86', 'x86_64'):
            parser.error('unsupported ABI: ' + abi)
        build = output / abi
        run('cmake', '-S', root / 'libai-java', '-B', build,
            '-DCMAKE_TOOLCHAIN_FILE=' + str(Path(args.ndk).resolve() / 'build/cmake/android.toolchain.cmake'),
            '-DANDROID_ABI=' + abi, '-DANDROID_PLATFORM=android-' + str(args.api),
            '-DANDROID_STL=c++_static', '-DCMAKE_BUILD_TYPE=Release')
        run('cmake', '--build', build, '--parallel')
        libraries.append((build / 'libai_jni.so', 'jni/' + abi + '/libai_jni.so'))
    artifact = output / 'libai.aar'
    with zipfile.ZipFile(artifact, 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.write(jar, 'classes.jar')
        archive.write(root / 'LICENSE', 'META-INF/LICENSE')
        archive.write(root / 'Android/AndroidManifest.xml', 'AndroidManifest.xml')
        archive.write(root / 'Android/consumer-rules.pro', 'proguard.txt')
        for path, name in libraries:
            archive.write(path, name)
    print(artifact)


if __name__ == '__main__':
    main()
