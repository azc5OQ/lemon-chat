"""exercise Gradle runtime preparation with an absent output directory"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path)
    parser.add_argument('--work-dir', type=Path)
    parser.add_argument('--abi', default='arm64-v8a')
    arguments = parser.parse_args()
    android = Path(__file__).resolve().parent.parent
    node = android.parent / 'nodejs-client'
    spec = importlib.util.spec_from_file_location('runtime_fetcher', node / 'fetch-nodejs-mobile.py')
    fetcher = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fetcher)
    pin = fetcher.load_pin()
    abis = fetcher.parse_abis(arguments.abi, pin)
    parent = arguments.work_dir.resolve() if arguments.work_dir else Path(tempfile.gettempdir()).resolve()
    parent.mkdir(parents=True, exist_ok=True)
    temporary = tempfile.TemporaryDirectory(prefix='lemon-runtime-gradle-', dir=parent)
    directory = Path(temporary.name).resolve()
    assert directory.is_relative_to(parent), 'temporary build escaped its selected parent'
    try:
        output = directory / 'app'
        runtime = output / 'generated/node-runtime' / pin['version'] / '_'.join(abis)
        assert not output.exists(), 'the integration test must start with absent output'
        init = directory / 'test.init.gradle'
        escaped = output.as_posix().replace("'", "\\'")
        init.write_text("gradle.beforeProject\n{\n    project ->\n    if (project.name == 'app')\n    {\n        project.layout.buildDirectory.set(new File('" + escaped + "'))\n    }\n}\n", encoding='utf-8')
        wrapper = android / ('gradlew.bat' if os.name == 'nt' else 'gradlew')
        command = [str(wrapper), '--no-daemon', '--max-workers=1', '-I', str(init), '-PandroidAbis=' + ','.join(abis), ':app:prepareNodeRuntime']
        if arguments.archive:
            command.insert(-1, '-PnodeRuntimeArchive=' + str(arguments.archive.resolve()))
        for attempt in range(2):
            completed = subprocess.run(command, cwd=android, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace')
            if completed.returncode != 0:
                raise AssertionError('Gradle preparation failed on attempt ' + str(attempt + 1) + '\n' + completed.stdout)
            fetcher.validate_runtime(runtime, pin, abis)
            manifest = runtime / 'runtime.json'
            if attempt == 0:
                original = manifest.read_bytes()
                modified = manifest.stat().st_mtime_ns
            else:
                assert manifest.read_bytes() == original and manifest.stat().st_mtime_ns == modified, 'cache verification rewrote its immutable manifest'
        print('PASS: Gradle prepares an absent runtime directory and verifies it again without rewriting files')
        print(json.dumps({'version': pin['version'], 'abis': abis, 'passes': 2}))
    finally:
        temporary.cleanup()


if __name__ == '__main__':
    main()
