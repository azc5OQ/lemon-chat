"""Check an assembled APK's managed JavaScript assets, CSP and native ABI contents."""
from pathlib import Path
import hashlib
import json
import argparse
import importlib.util
import sys
import zipfile
from test_support import ROOT
from native_runtime_check import verify_native_runtime

android = Path(__file__).resolve().parent.parent
node = android.parent / 'nodejs-client'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('apk', nargs='?', type=Path, default=android / 'app/build/outputs/apk/debug/app-debug.apk')
parser.add_argument('--abis', default='arm64-v8a,armeabi-v7a')
parser.add_argument('--runtime-dir', type=Path)
arguments = parser.parse_args()
abis = sorted(set(arguments.abis.split(',')))
spec = importlib.util.spec_from_file_location('runtime_fetcher', node / 'fetch-nodejs-mobile.py')
fetcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetcher)
metadata = fetcher.load_pin()
assert abis and all(abi in metadata['supported_abis'] for abi in abis), 'unsupported ABI selection'
runtime = arguments.runtime_dir or android / 'app/build/generated/node-runtime' / metadata['version'] / '_'.join(abis)
fetcher.validate_runtime(runtime, metadata, abis)
apk = arguments.apk
with zipfile.ZipFile(apk) as archive:
    names = set(archive.namelist())
    for notice in metadata.get('notices', []):
        assert hashlib.sha256(archive.read('assets/third-party/' + Path(notice['path']).name)).hexdigest() == notice['sha256'], 'missing or changed Node dependency notices'
    prefix = 'assets/nodejs-project/'
    assert prefix + 'mini-ws.js' not in names, 'obsolete WebSocket implementation is still packaged'
    assert prefix + 'bridge.json' not in names, 'a device-specific bridge capability must never be packaged'
    expected = ['bundle.js', 'load-bundle.js', 'android-main.js', 'android-bridge-client.js',
                'loopback-ui-server.js', 'websocket-adapter.js', 'transport-limits.js', 'loopback-transfer.js', 'vendor/ws-lock.json']
    expected.extend(path.relative_to(node).as_posix() for path in (node / 'vendor/ws').rglob('*') if path.is_file())
    actual = {name[len(prefix):] for name in names if name.startswith(prefix) and not name.endswith('/')}
    assert actual == set(expected), 'packaged runtime file set differs from the managed source set'
    for relative in expected:
        assert archive.read(prefix + relative) == (node / relative).read_bytes(), 'stale runtime asset: ' + relative
    lock = json.loads(archive.read(prefix + 'vendor/ws-lock.json'))
    for relative, expected_hash in lock['files'].items():
        assert hashlib.sha256(archive.read(prefix + 'vendor/ws/' + relative)).hexdigest() == expected_hash, relative
    assert prefix + 'vendor/ws/LICENSE' in names, 'WebSocket license is missing'
    html = archive.read('assets/client.html')
    assert html == (ROOT / 'client-source-code/client.html').read_bytes(), 'packaged page differs from the browser build'
    for marker in [b"frame-src 'none'", b"object-src 'none'", b'lemon-chat-identity',
                   b'chat_peer_local_state', b'picture_preparation',
                   b'local_transfer__create_receiver', b'runtime_failed']:
        assert marker in html, 'required packaged page feature absent: ' + marker.decode()
    native_report = verify_native_runtime(archive, apk, runtime, abis)
print('PASS: exact managed APK assets, source equality, ws checksums, native runtime sections, symbols and 16 KiB alignment')
for line in native_report:
    print(line)
print('APK: ' + str(apk.stat().st_size) + ' bytes')
