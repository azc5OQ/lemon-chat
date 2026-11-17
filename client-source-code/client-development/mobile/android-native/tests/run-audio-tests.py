"""Check real Opus audio through an isolated Lemon server using the native JNI library."""

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--jdk', type=Path, required=True)
    parser.add_argument('--android-jar', type=Path, required=True)
    parser.add_argument('--dependencies', type=Path, required=True)
    parser.add_argument('--native-library', type=Path, required=True)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--node', type=Path)
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    arguments.output.mkdir(parents=True, exist_ok=True)
    classes = arguments.output / 'classes'
    classes.mkdir(exist_ok=True)
    suffix = '.exe' if os.name == 'nt' else ''
    jars = list(arguments.dependencies.glob('*.jar'))
    base = root / 'app/src/main/java/com/lemonchat'
    sources = list((base / 'crypto').glob('*.java'))
    sources.extend([base / 'audio/OpusCodec.java', base / 'audio/MediaReplayGuard.java', base / 'protocol/BoundedWebSocket.java', root / 'tests/java/com/lemonchat/audio/AudioWireProbe.java', root / 'tests/java/com/lemonchat/audio/BrowserAudioFixtures.java', root / 'checks/java/com/lemonchat/audio/OpusSmoke.java'])
    sources.extend([base / 'audio/WebRtcTransport.java', base / 'audio/PcmResampler.java', base / 'audio/AudioWorker.java'])
    sources.append(root / 'tests/java/com/lemonchat/audio/AudioProcessingChecks.java')
    sources.append(root / 'tests/java/com/lemonchat/audio/MediaSnapshotChecks.java')
    sources.append(root / 'tests/java/com/lemonchat/audio/AudioWorkerChecks.java')
    classpath = os.pathsep.join(map(str, [classes] + jars + [arguments.android_jar]))
    subprocess.run([str(arguments.jdk / 'bin' / ('javac' + suffix)), '-encoding', 'UTF-8', '-cp', classpath, '-d', str(classes)] + list(map(str, sources)), check=True)
    java = [str(arguments.jdk / 'bin' / ('java' + suffix)), '-Djava.library.path=' + str(arguments.native_library), '-cp', classpath]
    with (arguments.output / 'audio-tests.log').open('w', encoding='utf-8') as output:
        subprocess.run(java + ['com.lemonchat.audio.OpusSmoke'], stdout=output, stderr=subprocess.STDOUT, check=True, timeout=30)
        subprocess.run(java + ['com.lemonchat.audio.AudioProcessingChecks'], stdout=output, stderr=subprocess.STDOUT, check=True, timeout=30)
        subprocess.run(java + ['com.lemonchat.audio.AudioWorkerChecks'], stdout=output, stderr=subprocess.STDOUT, check=True, timeout=30)
        if arguments.node:
            fixtures = arguments.output / 'browser-audio-fixtures.json'
            subprocess.run(java + ['com.lemonchat.audio.BrowserAudioFixtures', str(fixtures)], stdout=output, stderr=subprocess.STDOUT, check=True, timeout=30)
            subprocess.run([str(arguments.node), str(root / 'tests/browser-audio-interop.cjs'), str(fixtures)], stdout=output, stderr=subprocess.STDOUT, check=True, timeout=30)
        with tempfile.TemporaryDirectory(prefix='audio-server-', dir=arguments.output) as temporary:
            runtime = Path(temporary)
            with socket.socket() as listener:
                listener.bind(('127.0.0.1', 0))
                port = listener.getsockname()[1]
            settings = {
                'websocket_port': port,
                'server_slots': 8,
                'keys': [],
                'is_permission_system_enabled': False,
                'is_identity_storage_enabled': True,
                'is_restart_on_crash_enabled': False,
                'is_client_http_enabled': False,
                'is_display_country_flags_active': False,
                'is_same_ip_address_allowed': True,
                'is_websocket_ping_active': False,
                'is_voice_chat_active': True,
                'is_music_bot_audio_active': False,
                'is_audio_over_websocket_allowed': True,
                'is_audio_over_webrtc_allowed': True,
                'is_link_preview_allowed': False,
                'admin_password': base64.b64encode(hashlib.sha256(b'native-audio-test').digest()).decode(),
                'is_admin_password_initial': False,
            }
            (runtime / 'server_settings.json').write_text(json.dumps(settings), encoding='utf-8')
            environment = dict(os.environ)
            environment['PATH'] = str(arguments.server.parent) + os.pathsep + environment.get('PATH', '')
            with (arguments.output / 'server.log').open('w', encoding='utf-8') as server_log:
                server = subprocess.Popen([str(arguments.server)], cwd=runtime, stdout=server_log, stderr=subprocess.STDOUT, env=environment, creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
                try:
                    deadline = time.monotonic() + 30
                    while True:
                        if server.poll() is not None:
                            raise RuntimeError('The audio test server exited before listening')
                        try:
                            with socket.create_connection(('127.0.0.1', port), timeout=0.5):
                                break
                        except OSError:
                            if time.monotonic() >= deadline:
                                raise RuntimeError('The audio test server did not begin listening')
                            time.sleep(0.1)
                    for transport in ['websocket', 'webrtc', 'mixed']:
                        result = subprocess.run(java + ['com.lemonchat.audio.AudioWireProbe', 'ws://127.0.0.1:' + str(port) + '/', transport], stdout=output, stderr=subprocess.STDOUT, timeout=120)
                        if result.returncode:
                            break
                finally:
                    server.terminate()
                    try:
                        server.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait(timeout=8)
    print((arguments.output / 'audio-tests.log').read_text(encoding='utf-8'))
    raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
