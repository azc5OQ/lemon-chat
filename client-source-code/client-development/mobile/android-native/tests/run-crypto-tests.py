"""Run both directions of the native and browser identity/content checks."""

import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--jdk', type=Path, required=True)
    parser.add_argument('--dependencies', type=Path, required=True)
    parser.add_argument('--native-library', type=Path, required=True)
    parser.add_argument('--node', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    arguments.output.mkdir(parents=True, exist_ok=True)
    classes = arguments.output / 'classes'
    classes.mkdir(exist_ok=True)
    suffix = '.exe' if os.name == 'nt' else ''
    jars = list(arguments.dependencies.glob('*.jar'))
    classpath = os.pathsep.join(map(str, [classes] + jars))
    sources = list((root / 'app/src/main/java/com/lemonchat/crypto').glob('*.java'))
    sources.append(root / 'test/CryptoSelfTest.java')
    subprocess.run([str(arguments.jdk / 'bin' / ('javac' + suffix)), '--release', '17', '-encoding', 'UTF-8', '-cp', classpath, '-d', str(classes)] + list(map(str, sources)), check=True)
    browser = arguments.output / 'browser-vectors.tsv'
    native = arguments.output / 'native-content.tsv'
    harness = root / 'test/browser-vectors.cjs'
    with (arguments.output / 'crypto-tests.log').open('w', encoding='utf-8') as output:
        subprocess.run([str(arguments.node), str(harness), str(browser)], stdout=output, stderr=subprocess.STDOUT, check=True, timeout=180)
        subprocess.run([str(arguments.jdk / 'bin' / ('java' + suffix)), '-Djava.library.path=' + str(arguments.native_library), '-cp', classpath, 'CryptoSelfTest', str(browser), str(native)], stdout=output, stderr=subprocess.STDOUT, check=True, timeout=180)
        subprocess.run([str(arguments.node), str(harness), str(browser), str(native)], stdout=output, stderr=subprocess.STDOUT, check=True, timeout=180)
    print((arguments.output / 'crypto-tests.log').read_text(encoding='utf-8'))


if __name__ == '__main__':
    main()
