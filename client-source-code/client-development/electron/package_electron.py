# Packs client.html as the Lemon Chat desktop app with Electron.
# On Windows it writes buildresult/client-electron-amd64.exe, client-electron-arm64.exe and client-electron-i386.exe,
# portable programs, each a starter program with the app files appended. On Linux it writes
# client-electron-amd64.AppImage, client-electron-arm64.AppImage and client-electron-armhf.AppImage, which need
# mksquashfs from the squashfs-tools package. On macOS it writes client-electron-amd64.dmg and
# client-electron-arm64.dmg, which need the Xcode command line tools. --target builds only the named target and can
# be given more than once. The Windows targets can be built on any system with Zig.
#
# browser/build.cjs --electron calls this after it has written client.html. It can also run alone, for example
# python package_electron.py --client ../../client.html --target win32-arm64
#
# Nothing is installed from npm. The Electron archives are downloaded once into electron/.downloads and checked
# against electron-downloads.txt before every use. The push-to-talk helper (ptt-helper) and the Windows starter are
# compiled with Zig, which the client build already downloads for its WebAssembly modules. The macOS helper is
# compiled with clang. The script needs Python 3.8 or newer and only its standard library.
import argparse
import glob
import hashlib
import os
import plistlib
import shutil
import stat
import struct
import subprocess
import sys
import urllib.request
import zipfile
import zlib

ELECTRON_FOLDER = os.path.dirname(os.path.abspath(__file__))
DEVELOPMENT_FOLDER = os.path.dirname(ELECTRON_FOLDER)
REPOSITORY_FOLDER = os.path.dirname(os.path.dirname(DEVELOPMENT_FOLDER))
DOWNLOAD_FOLDER = os.path.join(ELECTRON_FOLDER, '.downloads')
BUILD_FOLDER = os.path.join(ELECTRON_FOLDER, '.build')
ASSET_FOLDER = os.path.join(DEVELOPMENT_FOLDER, 'desktop', 'assets')
MINIZ_FOLDER = os.path.join(REPOSITORY_FOLDER, 'server-source-code', 'main-server', 'third-party', 'miniz-3.1.2')
MINIZ_DEFINES = ['-DMINIZ_NO_STDIO', '-DMINIZ_NO_TIME', '-DMINIZ_NO_ARCHIVE_APIS', '-DMINIZ_NO_ZLIB_APIS', '-DMINIZ_NO_DEFLATE_APIS']

# Zig target or clang architecture, output file and AppImage runtime of each Electron target. The 32-bit targets
# win32-ia32 and linux-armv7l use Electron 43, the last version published for them (see electron-downloads.txt).
TARGETS = {
    'win32-x64': {'zig_target': 'x86_64-windows-gnu', 'output_name': 'client-electron-amd64.exe'},
    'win32-arm64': {'zig_target': 'aarch64-windows-gnu', 'output_name': 'client-electron-arm64.exe'},
    'win32-ia32': {'zig_target': 'x86-windows-gnu', 'output_name': 'client-electron-i386.exe'},
    'linux-x64': {'zig_target': 'x86_64-linux-gnu.2.17', 'output_name': 'client-electron-amd64.AppImage', 'runtime': 'runtime-x86_64'},
    'linux-arm64': {'zig_target': 'aarch64-linux-gnu.2.17', 'output_name': 'client-electron-arm64.AppImage', 'runtime': 'runtime-aarch64'},
    'linux-armv7l': {'zig_target': 'arm-linux-gnueabihf.2.17', 'output_name': 'client-electron-armhf.AppImage', 'runtime': 'runtime-armhf'},
    'darwin-x64': {'clang_architecture': 'x86_64', 'output_name': 'client-electron-amd64.dmg'},
    'darwin-arm64': {'clang_architecture': 'arm64', 'output_name': 'client-electron-arm64.dmg'},
}


def fail(message):
    """prints the reason and ends the script with exit code 1"""
    print('The Electron app was not built. ' + message, file=sys.stderr)
    sys.exit(1)


def host_targets():
    """the targets built when --target is not given, which are the targets of the system this script runs on"""
    if sys.platform == 'win32':
        return ['win32-x64', 'win32-arm64', 'win32-ia32']
    if sys.platform == 'darwin':
        return ['darwin-x64', 'darwin-arm64']
    return ['linux-x64', 'linux-arm64', 'linux-armv7l']


def read_downloads():
    """reads electron-downloads.txt into a dictionary that maps each key to its SHA-256 and address"""
    downloads = {}
    with open(os.path.join(ELECTRON_FOLDER, 'electron-downloads.txt'), encoding='utf-8') as file:
        for line in file:
            parts = line.split()
            if len(parts) == 3 and not line.startswith('#'):
                downloads[parts[0]] = (parts[1], parts[2])
    return downloads


def file_sha256(path):
    """SHA-256 of a file as lower-case hex"""
    digest = hashlib.sha256()
    with open(path, 'rb') as file:
        for block in iter(lambda: file.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def download_file(address, path, checksum):
    """downloads a file, shows the progress and keeps it only when its SHA-256 matches"""
    partial = path + '.partial'
    print('Downloading ' + address)
    with urllib.request.urlopen(address) as response, open(partial, 'wb') as file:
        total = int(response.headers.get('Content-Length') or 0)
        received = 0
        shown = -10
        for block in iter(lambda: response.read(1024 * 1024), b''):
            file.write(block)
            received += len(block)
            if total and received * 100 // total >= shown + 10:
                shown = received * 100 // total
                print('  ' + str(shown) + '%')
    if file_sha256(partial) != checksum:
        os.remove(partial)
        fail('The downloaded file ' + os.path.basename(path) + ' does not match its checksum and was not kept.')
    os.replace(partial, path)


def electron_archives(targets, policy):
    """returns the checked Electron archive of each target, downloading the missing ones once when allowed"""
    downloads = read_downloads()
    paths = {}
    missing = []
    for target in targets:
        checksum, address = downloads[target]
        paths[target] = os.path.join(DOWNLOAD_FOLDER, address.rsplit('/', 1)[1])

        # cached archive
        if os.path.isfile(paths[target]):
            if file_sha256(paths[target]) == checksum:
                continue
            os.remove(paths[target])
            print('The cached archive ' + os.path.basename(paths[target]) + ' did not match its checksum and was removed.')
        missing.append(target)
    if not missing:
        return paths

    # permission to download
    is_allowed = policy == 'yes'
    if policy == 'ask' and sys.stdin.isatty():
        answer = input('Electron for ' + ' and '.join(missing) + ' (up to 160 MB each) is needed. Download it into electron/.downloads? [y/N] ')
        is_allowed = answer.strip().lower() in ('y', 'yes')
    if not is_allowed:
        fail('The Electron archive of ' + ' and '.join(missing) + ' is missing. Run the build with --download-tools, or download '
             + ' and '.join(downloads[target][1] for target in missing) + ' into ' + DOWNLOAD_FOLDER + '.')

    # download and check
    os.makedirs(DOWNLOAD_FOLDER, exist_ok=True)
    for target in missing:
        download_file(downloads[target][1], paths[target], downloads[target][0])
    return paths


def extract_zip(archive_path, destination):
    """extracts a zip archive and keeps the Unix file modes and symbolic links stored in it"""
    with zipfile.ZipFile(archive_path) as archive:
        for entry in archive.infolist():
            target = os.path.join(destination, *entry.filename.split('/'))
            mode = entry.external_attr >> 16
            if entry.is_dir():
                os.makedirs(target, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            if stat.S_ISLNK(mode):
                os.symlink(archive.read(entry).decode('utf-8'), target)
                continue
            with archive.open(entry) as source, open(target, 'wb') as output:
                shutil.copyfileobj(source, output, 1024 * 1024)
            if mode & 0o777:
                os.chmod(target, mode & 0o777)


def find_zig(given):
    """finds the Zig compiler in --zig, ZIG, the download of the client build or the PATH"""
    candidates = [given, os.environ.get('ZIG')]
    candidates += sorted(glob.glob(os.path.join(DEVELOPMENT_FOLDER, 'browser', '.build-tools', 'zig-*', 'zig.exe' if sys.platform == 'win32' else 'zig')), reverse=True)
    candidates.append(shutil.which('zig'))
    for candidate in candidates:
        if candidate and os.path.isfile(candidate):
            return candidate
    fail('Zig was not found. Run the client build once (it downloads Zig for the WebAssembly modules) or pass --zig.')


def run(command, folder=None):
    """runs a build command and ends the script when it fails"""
    result = subprocess.run(command, cwd=folder)
    if result.returncode != 0:
        fail('The command ' + ' '.join(command) + ' failed.')


def place_app_files(resources, client):
    """copies main.js, package.json, client.html and the window icon into resources/app"""
    app_folder = os.path.join(resources, 'app')
    os.makedirs(app_folder, exist_ok=True)
    for name in ('main.js', 'package.json'):
        shutil.copyfile(os.path.join(ELECTRON_FOLDER, 'app', name), os.path.join(app_folder, name))
    shutil.copyfile(client, os.path.join(app_folder, 'client.html'))
    shutil.copyfile(os.path.join(ASSET_FOLDER, 'lemon.png'), os.path.join(app_folder, 'icon.png'))
    default_app = os.path.join(resources, 'default_app.asar')
    if os.path.exists(default_app):
        os.remove(default_app)


def build_payload(folder):
    """packs every file of a folder for the Windows starter and returns the packed bytes and the file count"""
    records = []
    paths = []
    for root, _, names in os.walk(folder):
        for name in names:
            paths.append(os.path.join(root, name))
    for path in sorted(paths):
        relative = os.path.relpath(path, folder).replace(os.sep, '/').encode('utf-8')
        with open(path, 'rb') as file:
            data = file.read()
        packer = zlib.compressobj(9, zlib.DEFLATED, -15)
        packed = packer.compress(data) + packer.flush()
        records.append(struct.pack('<I', len(relative)) + relative + struct.pack('<QQ', len(data), len(packed)) + packed)
    return b''.join(records), len(records)


def package_windows(client, zig, target, archive, output_folder):
    """builds client-electron-amd64.exe or client-electron-arm64.exe"""
    zig_target = TARGETS[target]['zig_target']
    stage = os.path.join(BUILD_FOLDER, target)
    app_folder = os.path.join(stage, 'app')
    launcher_folder = os.path.join(stage, 'launcher')
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(launcher_folder)

    # Electron and the page
    print('Unpacking Electron for ' + target + '.')
    extract_zip(archive, app_folder)
    os.replace(os.path.join(app_folder, 'electron.exe'), os.path.join(app_folder, 'lemon-chat.exe'))
    place_app_files(os.path.join(app_folder, 'resources'), client)

    # push-to-talk helper
    print('Compiling the push-to-talk helper and the starter with Zig for ' + zig_target + '.')
    run([zig, 'cc', '-target', zig_target, '-O2', '-s', os.path.join(ELECTRON_FOLDER, 'ptt-helper', 'ptt_windows.c'),
         '-o', os.path.join(launcher_folder, 'ptt-helper.exe'), '-luser32'])
    shutil.copyfile(os.path.join(launcher_folder, 'ptt-helper.exe'), os.path.join(app_folder, 'resources', 'ptt-helper.exe'))

    # starter with the icon
    shutil.copyfile(os.path.join(ELECTRON_FOLDER, 'windows-launcher', 'launcher.rc'), os.path.join(launcher_folder, 'launcher.rc'))
    shutil.copyfile(os.path.join(ASSET_FOLDER, 'lemon.ico'), os.path.join(launcher_folder, 'lemon.ico'))
    run([zig, 'cc', '-target', zig_target, '-O2', '-s', os.path.join(ELECTRON_FOLDER, 'windows-launcher', 'launcher.c'),
         os.path.join(MINIZ_FOLDER, 'miniz_tinfl.c'), '-I', MINIZ_FOLDER] + MINIZ_DEFINES
        + ['launcher.rc', '-o', 'launcher.exe', '-Wl,--subsystem,windows', '-luser32', '-lgdi32'], launcher_folder)

    # starter and files
    print('Packing the app files into the starter.')
    with open(os.path.join(launcher_folder, 'launcher.exe'), 'rb') as file:
        launcher = file.read()
    payload, count = build_payload(app_folder)
    build_id = hashlib.sha256(payload).hexdigest()[:16]
    trailer = b'LEMONPAK' + struct.pack('<QQ', len(launcher), count) + build_id.encode('ascii')
    output = os.path.join(output_folder, TARGETS[target]['output_name'])
    with open(output + '.partial', 'wb') as file:
        file.write(launcher)
        file.write(payload)
        file.write(trailer)
    os.replace(output + '.partial', output)
    shutil.rmtree(stage, ignore_errors=True)
    return output


def package_linux(client, zig, target, archive, output_folder):
    """builds client-electron-amd64.AppImage or client-electron-arm64.AppImage"""
    zig_target = TARGETS[target]['zig_target']
    stage = os.path.join(BUILD_FOLDER, target)
    app_dir = os.path.join(stage, 'AppDir')
    app_folder = os.path.join(app_dir, 'lemon-chat')
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(app_dir)

    # Electron and the page
    print('Unpacking Electron for ' + target + '.')
    extract_zip(archive, app_folder)
    os.replace(os.path.join(app_folder, 'electron'), os.path.join(app_folder, 'lemon-chat'))
    place_app_files(os.path.join(app_folder, 'resources'), client)

    # push-to-talk helper
    print('Compiling the push-to-talk helper with Zig for ' + zig_target + '.')
    helper = os.path.join(app_folder, 'resources', 'ptt-helper')
    run([zig, 'cc', '-target', zig_target, '-O2', '-s', os.path.join(ELECTRON_FOLDER, 'ptt-helper', 'ptt_linux.c'),
         '-o', helper, '-ldl'])
    os.chmod(helper, 0o755)

    # start script, desktop entry, icon
    with open(os.path.join(ELECTRON_FOLDER, 'linux', 'AppRun'), 'rb') as file:
        start_script = file.read().replace(b'\r\n', b'\n')
    with open(os.path.join(app_dir, 'AppRun'), 'wb') as file:
        file.write(start_script)
    os.chmod(os.path.join(app_dir, 'AppRun'), 0o755)
    shutil.copyfile(os.path.join(ELECTRON_FOLDER, 'linux', 'lemon-chat.desktop'), os.path.join(app_dir, 'lemon-chat.desktop'))
    shutil.copyfile(os.path.join(ASSET_FOLDER, 'lemon.png'), os.path.join(app_dir, 'lemon-chat.png'))
    shutil.copyfile(os.path.join(ASSET_FOLDER, 'lemon.png'), os.path.join(app_dir, '.DirIcon'))

    # runtime and image
    print('Writing the AppImage.')
    image = os.path.join(stage, 'app.squashfs')
    run(['mksquashfs', app_dir, image, '-root-owned', '-noappend', '-comp', 'zstd', '-quiet'])
    output = os.path.join(output_folder, TARGETS[target]['output_name'])
    with open(output + '.partial', 'wb') as file:
        for part in (os.path.join(ELECTRON_FOLDER, 'linux', TARGETS[target]['runtime']), image):
            with open(part, 'rb') as source:
                shutil.copyfileobj(source, file, 1024 * 1024)
    os.chmod(output + '.partial', 0o755)
    os.replace(output + '.partial', output)
    shutil.rmtree(stage, ignore_errors=True)
    return output


def package_macos(client, target, archive, output_folder):
    """builds client-electron-amd64.dmg or client-electron-arm64.dmg, on macOS only"""
    architecture = TARGETS[target]['clang_architecture']
    stage = os.path.join(BUILD_FOLDER, target)
    image_folder = os.path.join(stage, 'image')
    app_bundle = os.path.join(image_folder, 'Lemon Chat.app')
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(image_folder)

    # Electron and the page
    print('Unpacking Electron for ' + target + '.')
    run(['ditto', '-x', '-k', archive, image_folder])
    os.replace(os.path.join(image_folder, 'Electron.app'), app_bundle)
    resources = os.path.join(app_bundle, 'Contents', 'Resources')
    place_app_files(resources, client)

    # push-to-talk helper
    print('Compiling the push-to-talk helper with clang for ' + architecture + '.')
    run(['clang', '-O2', '-arch', architecture, '-mmacosx-version-min=11.0', os.path.join(ELECTRON_FOLDER, 'ptt-helper', 'ptt_macos.c'),
         '-o', os.path.join(resources, 'ptt-helper'), '-framework', 'ApplicationServices', '-framework', 'CoreFoundation'])

    # name and permission texts
    info_path = os.path.join(app_bundle, 'Contents', 'Info.plist')
    with open(info_path, 'rb') as file:
        info = plistlib.load(file)
    info.update({'CFBundleName': 'Lemon Chat', 'CFBundleDisplayName': 'Lemon Chat', 'CFBundleIdentifier': 'lemon.chat.electron',
                 'NSMicrophoneUsageDescription': 'Lemon Chat uses the microphone for voice chat.',
                 'NSCameraUsageDescription': 'Lemon Chat uses the camera for video.'})
    with open(info_path, 'wb') as file:
        plistlib.dump(info, file)

    # local signature and disk image
    print('Signing locally and writing the disk image.')
    run(['codesign', '--force', '--deep', '--sign', '-', app_bundle])
    output = os.path.join(output_folder, TARGETS[target]['output_name'])
    run(['hdiutil', 'create', '-volname', 'Lemon Chat', '-srcfolder', image_folder, '-ov', '-format', 'UDZO', output])
    shutil.rmtree(stage, ignore_errors=True)
    return output


def main():
    parser = argparse.ArgumentParser(description='Packs client.html as the Lemon Chat Electron app.')
    parser.add_argument('--client', default=os.path.join(REPOSITORY_FOLDER, 'client-source-code', 'client.html'), help='the built client.html')
    parser.add_argument('--zig', help='the Zig compiler, found automatically when omitted')
    parser.add_argument('--target', action='append', choices=list(TARGETS),
                        help='a target to build, can be given more than once, all targets of this system when omitted')
    parser.add_argument('--output-folder', default=os.path.join(REPOSITORY_FOLDER, 'buildresult'))
    policy = parser.add_mutually_exclusive_group()
    policy.add_argument('--download-tools', dest='policy', action='store_const', const='yes')
    policy.add_argument('--no-download-tools', dest='policy', action='store_const', const='no')
    arguments = parser.parse_args()
    policy_value = arguments.policy or 'ask'
    targets = list(dict.fromkeys(arguments.target or host_targets()))

    # inputs
    if not os.path.isfile(arguments.client) or os.path.getsize(arguments.client) == 0:
        fail('client.html was not found at ' + arguments.client + '. Build the client first.')
    os.makedirs(arguments.output_folder, exist_ok=True)

    # tools of the targets
    zig = None
    if any(target.startswith('darwin') for target in targets) and sys.platform != 'darwin':
        fail('The macOS app can only be built on a Mac.')
    if any(target.startswith('linux') for target in targets) and shutil.which('mksquashfs') is None:
        fail('mksquashfs is missing. Install the squashfs-tools package, on Debian and Ubuntu with sudo apt install squashfs-tools.')
    if any(not target.startswith('darwin') for target in targets):
        zig = find_zig(arguments.zig)
    archives = electron_archives(targets, policy_value)

    # package each target
    for target in targets:
        print('\nPacking the Electron app for ' + target + '.')
        if target.startswith('win32'):
            output = package_windows(arguments.client, zig, target, archives[target], arguments.output_folder)
        elif target.startswith('linux'):
            output = package_linux(arguments.client, zig, target, archives[target], arguments.output_folder)
        else:
            output = package_macos(arguments.client, target, archives[target], arguments.output_folder)
        print('The Electron app is written to ' + output + ' (' + str(os.path.getsize(output) // (1024 * 1024)) + ' MB).')


if __name__ == '__main__':
    main()
