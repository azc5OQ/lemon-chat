"""Run the Java protocol checks and optional isolated server interoperability checks."""

import argparse
import base64
import hashlib
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import json


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--jdk', type=Path, required=True)
    parser.add_argument('--android-jar', type=Path, required=True)
    parser.add_argument('--dependencies', type=Path, required=True)
    parser.add_argument('--native-library', type=Path, required=True)
    parser.add_argument('--server', type=Path)
    parser.add_argument('--demo', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    arguments = parser.parse_args()
    if arguments.demo and not arguments.server:
        parser.error('--demo requires --server')
    root = Path(__file__).resolve().parents[1]
    arguments.output.mkdir(parents=True, exist_ok=True)
    classes = arguments.output / 'classes'
    classes.mkdir(exist_ok=True)
    executable_suffix = '.exe' if os.name == 'nt' else ''
    javac = arguments.jdk / 'bin' / ('javac' + executable_suffix)
    java = arguments.jdk / 'bin' / ('java' + executable_suffix)
    jars = list(arguments.dependencies.glob('*.jar'))
    compile_classpath = os.pathsep.join(map(str, [arguments.android_jar] + jars))
    base = root / 'app/src/main/java/com/lemonchat'
    sources = [path for folder in ['model', 'protocol', 'crypto', 'audio'] for path in (base / folder).glob('*.java')]
    sources.append(base / 'service/ReconnectBackoff.java')
    sources.append(base / 'service/MessageUpdates.java')
    sources.extend((root / 'tests/java').rglob('*.java'))
    subprocess.run([str(javac), '-encoding', 'UTF-8', '-cp', compile_classpath, '-d', str(classes)] + list(map(str, sources)), check=True)
    runtime_classpath = os.pathsep.join(map(str, [classes] + jars + [arguments.android_jar]))
    subprocess.run([str(java), '-cp', runtime_classpath, 'com.lemonchat.service.BackgroundChecks'], check=True)
    subprocess.run([str(java), '-cp', runtime_classpath, 'com.lemonchat.protocol.AdminHostChecks'], check=True)
    with tempfile.TemporaryDirectory(prefix='transport-', dir=arguments.output) as transport:
        certificate = Path(transport) / 'listener.p12'
        keytool = arguments.jdk / 'bin' / ('keytool' + executable_suffix)
        subprocess.run([str(keytool), '-genkeypair', '-alias', 'listener', '-keyalg', 'RSA', '-keysize', '2048', '-validity', '2', '-storetype', 'PKCS12', '-dname', 'CN=localhost', '-ext', 'SAN=dns:localhost', '-keystore', str(certificate), '-storepass', 'transport-fixture', '-keypass', 'transport-fixture'], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        subprocess.run([str(java), '-cp', runtime_classpath, 'com.lemonchat.protocol.AutomaticTransportChecks', str(certificate)], check=True)
    command = [str(java), '-Djava.library.path=' + str(arguments.native_library), '-Dlemon.test.image=' + str(root.parent.parent / 'desktop/assets/icons/channel.png'), '-cp', runtime_classpath, 'com.lemonchat.protocol.ProtocolHostProbe']
    if arguments.demo:
        command[-1] = 'com.lemonchat.protocol.DemoPeers'
    server = None
    with tempfile.TemporaryDirectory(prefix='server-', dir=arguments.output) as temporary:
        runtime = Path(temporary)
        with (arguments.output / 'server.log').open('w', encoding='utf-8') as log:
            try:
                if arguments.server:
                    with socket.socket() as listener:
                        listener.bind(('127.0.0.1', 0))
                        port = listener.getsockname()[1]
                    settings = {
                        'websocket_port': port,
                        'server_slots': 16,
                        'keys': [],
                        'is_permission_system_enabled': True,
                        'is_idle_mode_allowed': True,
                        'is_file_upload_allowed': True,
                        'is_avatar_upload_allowed': True,
                        'is_blocking_direct_chat_pictures_active': True,
                        'is_blocking_direct_chat_files_active': True,
                        'is_blocking_channel_chat_pictures_active': True,
                        'is_blocking_channel_chat_files_active': True,
                        'groups': [{'id': 1, 'name': 'Test guests', 'group_type': 'guest', 'is_default_guest': True,
                                    'permissions': {'send_offline_messages': 'grant', 'receive_offline_messages': 'grant', 'call_idle_clients': 'grant', 'send_pokes': 'grant', 'send_direct_pictures': 'grant', 'send_direct_files': 'grant', 'send_channel_pictures': 'grant', 'send_channel_files': 'grant', 'upload_emojis': 'grant'}}],
                        'is_temp_channel_creation_allowed': True,
                        'is_identity_storage_enabled': True,
                        'is_offline_messaging_allowed': True,
                        'is_restart_on_crash_enabled': False,
                        'is_client_http_enabled': False,
                        'is_display_country_flags_active': False,
                        'is_same_ip_address_allowed': True,
                        'is_websocket_ping_active': False,
                        'is_voice_chat_active': False,
                        'is_music_bot_audio_active': False,
                        'is_audio_over_websocket_allowed': False,
                        'is_audio_over_webrtc_allowed': False,
                        'is_link_preview_allowed': False,
                        'admin_password': base64.b64encode(hashlib.sha256(b'native-protocol-test').digest()).decode(),
                        'is_admin_password_initial': False,
                    }
                    if arguments.demo:
                        settings['groups'][0]['permissions']['create_music_bots'] = 'grant'
                        settings['is_voice_chat_active'] = True
                        settings['is_music_bot_audio_active'] = True
                        settings['is_audio_over_websocket_allowed'] = True
                        settings['channels'] = [
                            {'channel_id': index, 'parent_channel_id': -1 if index == 0 else 0,
                             'name': name, 'description': description, 'is_root_channel': index == 0,
                             'is_audio_enabled': True, 'is_using_password': False, 'password': ''}
                            for index, (name, description) in enumerate([
                                ('General', 'A place to catch up'),
                                ('Weekend plans', 'Make some time for good company'),
                                ('Music', 'Listen together'),
                            ])
                        ]
                    (runtime / 'server_settings.json').write_text(json.dumps(settings), encoding='utf-8')
                    environment = dict(os.environ)
                    environment['PATH'] = str(arguments.server.parent) + os.pathsep + environment.get('PATH', '')
                    server = subprocess.Popen([str(arguments.server)], cwd=runtime, stdout=log, stderr=subprocess.STDOUT, env=environment, creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
                    deadline = time.monotonic() + 30
                    while True:
                        if server.poll() is not None:
                            raise RuntimeError('The isolated server exited before listening')
                        try:
                            with socket.create_connection(('127.0.0.1', port), timeout=0.5):
                                break
                        except OSError:
                            if time.monotonic() >= deadline:
                                raise RuntimeError('The isolated server did not begin listening')
                            time.sleep(0.1)
                    command.append('ws://127.0.0.1:' + str(port) + '/')
                    if arguments.demo:
                        stop_file = arguments.output / 'demo.stop'
                        stop_file.unlink(missing_ok=True)
                        command.append(str(stop_file))
                        print('Demo endpoint: ws://10.0.2.2:' + str(port) + '/', flush=True)
                        print('Stop file: ' + str(stop_file), flush=True)
                with (arguments.output / 'protocol-tests.log').open('w', encoding='utf-8') as output:
                    result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, timeout=7200 if arguments.demo else 180)
                print((arguments.output / 'protocol-tests.log').read_text(encoding='utf-8'))
                if result.returncode:
                    raise SystemExit(result.returncode)
            finally:
                if server is not None and server.poll() is None:
                    server.terminate()
                    try:
                        server.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait(timeout=8)


if __name__ == '__main__':
    main()
