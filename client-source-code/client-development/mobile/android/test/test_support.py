"""Host JVM helpers; no Android SDK, device, application data or network required."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[5]
JAVA = Path(__file__).resolve().parent.parent / 'app/src/main/java/com/lemonchat'
FIXTURES = Path(__file__).resolve().parent / 'fixtures'


def extract(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def compile_run(sources, main_class):
    extension = '.exe' if os.name == 'nt' else ''
    home = os.environ.get('JAVA_HOME')
    javac = Path(home) / 'bin' / ('javac' + extension) if home else None
    if javac is None or not javac.is_file():
        found = shutil.which('javac')
        javac = Path(found).resolve() if found else None
    if javac is None or not javac.is_file():
        raise RuntimeError('Set JAVA_HOME to a JDK (Java 8 or newer), or put javac on PATH.')
    java = javac.with_name('java' + extension)
    with tempfile.TemporaryDirectory(prefix='lemon-native-regression-') as temporary:
        directory = Path(temporary)
        for relative, content in sources.items():
            path = directory / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding='utf-8')
        subprocess.run([str(javac), '-encoding', 'UTF-8', '-d', temporary,
                        *map(str, directory.rglob('*.java'))], check=True, timeout=30)
        result = subprocess.run([str(java), '-cp', temporary, main_class],
                                check=True, text=True, capture_output=True, timeout=20)
        print(result.stdout, end='')

