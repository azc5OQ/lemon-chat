"""Verify timestamp-only input changes do not invalidate a content-current Node bundle.

Run after a successful Gradle assembleDebug. Requires the Android build toolchain.
The source bytes are untouched, and its original timestamps are restored afterward.
"""
from pathlib import Path
import os
import subprocess
import time

android = Path(__file__).resolve().parent.parent
node = android.parent / 'nodejs-client'
source = android.parents[1] / 'browser/main/wasm/rsa_keygen.wasm'
bundle = node / 'bundle.js'
fingerprint = android / 'app/build/generated/nodejs/bundle-fingerprint.json'
assert source.is_file() and bundle.is_file() and fingerprint.is_file(), 'run assembleDebug first'

source_stat = source.stat()
bundle_stat = bundle.stat()
bundle_bytes = bundle.read_bytes()
fingerprint_bytes = fingerprint.read_bytes()
try:
    newer_ns = max(time.time_ns(), bundle_stat.st_mtime_ns + 2_000_000_000)
    os.utime(source, ns=(source_stat.st_atime_ns, newer_ns))
    result = subprocess.run(
        ['cmd.exe', '/d', '/c', 'gradlew.bat', ':app:checkNodeBundleFresh', '--console=plain'],
        cwd=android, text=True, capture_output=True, check=False)
    print(result.stdout)
    print(result.stderr)
    assert result.returncode == 0, 'content freshness check failed after timestamp-only change'
    assert ':app:buildNodeBundle UP-TO-DATE' in result.stdout, 'unchanged Node content rebuilt'
    assert bundle.stat().st_mtime_ns == bundle_stat.st_mtime_ns, 'bundle was rewritten'
    assert bundle.read_bytes() == bundle_bytes, 'bundle bytes changed'
    assert fingerprint.read_bytes() == fingerprint_bytes, 'fingerprint bytes changed'
finally:
    os.utime(source, ns=(source_stat.st_atime_ns, source_stat.st_mtime_ns))

print('PASS: timestamp-only Wasm change preserves the content-current bundle and passes exact freshness validation')
