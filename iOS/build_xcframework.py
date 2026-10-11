#!/usr/bin/env python3
"""Build device/simulator archives and a distributable Foundation XCFramework."""
import argparse
from pathlib import Path
import subprocess
import shutil


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('build/ios'))
    parser.add_argument('--deployment-target', default='13.0')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    output = args.output.resolve()
    libraries = []
    for sdk, arch in [('iphoneos', 'arm64'), ('iphonesimulator', 'arm64;x86_64')]:
        build = output / sdk
        run('cmake', '-S', root / 'libai-objc', '-B', build,
            '-DCMAKE_SYSTEM_NAME=iOS', '-DCMAKE_OSX_SYSROOT=' + sdk,
            '-DCMAKE_OSX_ARCHITECTURES=' + arch,
            '-DCMAKE_OSX_DEPLOYMENT_TARGET=' + args.deployment_target,
            '-DCMAKE_BUILD_TYPE=Release', '-DLIBAI_OBJC_TESTS=OFF')
        run('cmake', '--build', build, '--parallel')
        libraries.extend(['-library', str(build / 'libai_objc.a'), '-headers', str(root / 'libai-objc/include')])
    artifact = output / 'LibAI.xcframework'
    if artifact.exists():
        parser.error('output already exists; remove it explicitly or choose another --output')
    run('xcodebuild', '-create-xcframework', *libraries, '-output', artifact)
    shutil.copyfile(root / 'LICENSE', artifact / 'LICENSE')
    print(artifact)


if __name__ == '__main__':
    main()
