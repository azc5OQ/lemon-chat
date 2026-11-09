#!/usr/bin/env python3
"""Remove recognised build output without treating every ignored file as disposable."""

import argparse
import fnmatch
from functools import lru_cache
import os
from pathlib import Path
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
DESKTOP = 'client-source-code/client-development/desktop'
BROWSER = 'client-source-code/client-development/browser'
WASM = BROWSER + '/client-webassemblies'
SERVER = 'server-source-code'
SCOPES = ('client-source-code', SERVER, 'buildresult', 'build', 'tests', '.audio-test-runtime')
SKIP_DIRECTORIES = {'.git', '.idea', '.build-tools', 'build-tools', 'node_modules', '.venv', 'venv'}
DEPENDENCY_DIRECTORIES = {
    'client-source-code/client-development/mobile/android/app/src/main/jniLibs',
    'client-source-code/client-development/mobile/android/app/src/main/cpp/nodejs-mobile-include',
}
CACHE_DIRECTORIES = {'CMakeFiles', '.cmake', 'Testing', '_CPack_Packages', '__pycache__',
                     '.zig-cache', 'zig-cache', '.cxx', '.gradle', '.vs', 'autom4te.cache', '.deps', '.libs'}
BUILD_FILES = {'CMakeCache.txt', 'cmake_install.cmake', 'CTestTestfile.cmake', 'install_manifest.txt',
               'DartConfiguration.tcl', 'CPackConfig.cmake', 'CPackSourceConfig.cmake',
               'build.ninja', 'rules.ninja', '.ninja_log', '.ninja_deps', '.ninja_lock',
               'config.log', 'config.cache', 'compile_commands.json'}
COMPILED_PATTERNS = ('*.o', '*.obj', '*.a', '*.lib', '*.dll', '*.dll.a', '*.exe', '*.so', '*.so.*',
                     '*.dylib', '*.pdb', '*.idb', '*.ilk', '*.exp', '*.res', '*.pch', '*.gch',
                     '*.ipch', '*.tlog', '*.lo', '*.la', '*.pyc', '*.pyo', '*.class', '*.dex', '*.apk', '*.aab')
RUNTIME_FILES = {'server_settings.json', 'desktop.json', 'stunnel.conf', 'bridge.json', 'local.properties'}
RUNTIME_DIRECTORIES = {'uploads', 'downloads', 'music', 'playlists', 'recordings', 'keys', 'identities', 'logs'}
RUNTIME_SUFFIXES = {'.lmn', '.pem', '.key', '.p12', '.pfx', '.sqlite', '.sqlite3', '.db', '.mp3', '.wav', '.opus'}
WINDOWS_SERVER_LIBRARIES = (
    'mbedtls-3.6.6', 'libdatachannel-0.24.2', 'libtom/libtommath', 'libtom/libtomcrypt',
    'theldus-websocket', 'libviolet-0.5.4', 'libmaxminddb-1.12.2', 'libopus-1.6.1',
)
WINDOWS_SERVER_OUTPUTS = {'chat-server.exe', 'chat-server.pdb', 'libdatachannel.dll'}
STUNNEL = SERVER + '/third-party/stunnel'
WINDOWS_PACKAGE_FILES = {
    'libdatachannel-0.24.2/LibDataChannelConfig.cmake',
    'libdatachannel-0.24.2/LibDataChannelConfigVersion.cmake',
    'libtom/libtommath/libtommath-config.cmake',
    'libtom/libtommath/libtommath-config-version.cmake',
    'libtom/libtommath/libtommath.pc',
    'libmaxminddb-1.12.2/maxminddb-config.cmake',
    'libmaxminddb-1.12.2/cmake_uninstall.cmake',
    'libmaxminddb-1.12.2/src/libmaxminddb.pc',
    'libopus-1.6.1/OpusConfig.cmake', 'libopus-1.6.1/OpusConfigVersion.cmake',
    'libopus-1.6.1/opus.pc',
}


def git(*arguments):
    result = subprocess.run(['git', '-C', str(ROOT), *arguments], check=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    return result.stdout


def tracked_files():
    if Path(os.fsdecode(git('rev-parse', '--show-toplevel')).strip()).resolve() != ROOT:
        raise RuntimeError('Run the scripts from their original repository. The Git root does not match.')
    files = git('ls-files', '-z')
    # Also protect files staged for deletion, if they are still on disk.
    if subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--verify', 'HEAD'],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
        files += git('ls-tree', '-r', '-z', '--name-only', 'HEAD')
    return {os.fsdecode(name).casefold() for name in files.split(b'\0') if name}


def is_link(path):
    info = path.lstat()
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, 'st_file_attributes', 0) & 0x400)


def relative(path):
    return path.relative_to(ROOT).as_posix()


def runtime_file(path):
    parts = path.relative_to(ROOT).parts
    return (path.name.lower() in RUNTIME_FILES or path.suffix.lower() in RUNTIME_SUFFIXES
            or any(part.lower() in RUNTIME_DIRECTORIES for part in parts)
            or path.name.startswith('music-bots-'))


def build_directory(path):
    rel = relative(path)
    parent = path.parent
    owner = relative(parent)
    name = path.name
    owners = {DESKTOP, BROWSER, WASM, SERVER, '.', 'tests'}
    owners.update(WASM + '/' + project for project in ('connection-crypto', 'rsa-keygen', 'libopusjs', 'mp3-to-pcm'))
    owners.update(('client-source-code/client-development/mobile/android',
                   'client-source-code/client-development/mobile/android/app',
                   SERVER + '/android-server-app', SERVER + '/android-server-app/app'))
    if owner in owners and (name == 'build' or name.startswith(('build-', 'build_', 'cmake-build-'))):
        return True
    if owner == DESKTOP and name in {'out', 'dist', 'package', 'preview', 'Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel'}:
        return True
    return rel == 'buildresult'


def same_file(left, right):
    if not right.is_file() or is_link(right) or left.stat().st_size != right.stat().st_size:
        return False
    with left.open('rb') as a, right.open('rb') as b:
        while True:
            chunk = a.read(65536)
            if chunk != b.read(65536):
                return False
            if not chunk:
                return True


@lru_cache(maxsize=None)
def committed_versions(name):
    versions = []
    for revision in (':' + name, 'HEAD:' + name):
        result = subprocess.run(['git', '-C', str(ROOT), 'show', revision], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        if result.returncode == 0:
            versions.append(result.stdout)
    return versions


def unchanged_copy(path, original):
    if same_file(path, original):
        return True
    # Older packaged copies are disposable only when their exact contents still exist in Git.
    versions = committed_versions(relative(original))
    return bool(versions) and path.read_bytes() in versions


def copied_asset(path):
    """A changed live CSS/font/sound copy is user data, not disposable output."""
    parts = path.relative_to(ROOT).parts
    if relative(path).startswith(DESKTOP + '/'):
        for directory in ('assets', 'fonts'):
            if directory in parts[4:]:
                index = parts.index(directory, 4)
                suffix = Path(*parts[index + 1:])
                originals = [ROOT / DESKTOP / directory / suffix]
                if directory == 'assets':
                    originals.append(ROOT / DESKTOP / 'main/overlay/assets' / suffix)
                else:
                    originals.append(ROOT / DESKTOP / 'third-party/meek-ui/fonts' / suffix)
                return any(unchanged_copy(path, original) for original in originals)
        if 'include' in parts[4:]:
            index = parts.index('include', 4)
            suffix = Path(*parts[index + 1:])
            if suffix.parts and suffix.parts[0] == 'opus':
                return unchanged_copy(path, ROOT / DESKTOP / 'third-party/libopus-1.5.2/include' / suffix.name)
        if 'licenses' in parts[4:]:
            originals = {
                'cJSON.txt': 'third-party/cJSON-LICENSE.txt',
                'clib-math.LICENSE': 'main/clib/clib-math.LICENSE',
                'libfvad.txt': 'third-party/libfvad/LICENSE',
                'libfvad-PATENTS.txt': 'third-party/libfvad/PATENTS',
                'miniaudio.txt': 'third-party/miniaudio-0.11.26/LICENSE',
                'Opus.txt': 'third-party/libopus-1.5.2/COPYING',
                'MbedTLS.txt': 'third-party/mbedtls-3.6.6/LICENSE',
                'MinHook.txt': 'third-party/minhook/LICENSE.txt',
            }
            if path.name in originals:
                return unchanged_copy(path, ROOT / DESKTOP / originals[path.name])
    if path.name == 'client.html':
        return unchanged_copy(path, ROOT / 'client-source-code/client.html')
    if path.name == 'start_server.sh':
        return same_file(path, ROOT / SERVER / 'main/unix_start_template')
    if path.suffix == '.mmdb':
        return same_file(path, ROOT / SERVER / 'third-party/libmaxminddb-1.12.2' / path.name)
    return False


def generated_file(path, in_build, in_cache):
    name = path.name
    if in_cache or name in BUILD_FILES or any(fnmatch.fnmatch(name, pattern) for pattern in COMPILED_PATTERNS):
        return True
    if name in {'Makefile', 'config.status', 'libtool', 'config.h'}:
        with path.open('rb') as stream:
            header = stream.read(4096)
        return any(marker in header for marker in (b'CMAKE generated file', b'Generated by configure',
                   b'generated by GNU Autoconf', b'Generated from Makefile.in by configure', b'Generated by CMake'))
    if name.startswith('.lemon-client-') and name.endswith('.tmp'):
        return True
    if name == 'bundle.js' and relative(path) == 'client-source-code/client-development/mobile/nodejs-client/bundle.js':
        return True
    if not in_build:
        return False
    if copied_asset(path):
        return True
    if name.startswith(('desktop-', 'overlay-smoke')) and path.suffix in {'.bmp', '.png'}:
        return True
    if path.suffix in {'.wasm', '.log', '.cmake', '.ninja', '.vcxproj', '.filters', '.sln', '.d', '.def'}:
        return True
    if name in {'lemon-chat.rc', 'lemon-chat.manifest', 'build.lock'}:
        return True
    # Unix executables have no extension; never mistake an executable source script for a binary.
    with path.open('rb') as stream:
        return stream.read(4) in (b'\x7fELF', b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe', b'\xca\xfe\xba\xbe')


def windows_server_path(path, include_stunnel):
    rel = relative(path)
    linkage = SERVER + '/main/linkage-files/'
    if rel.startswith(linkage) and rel != linkage + 'windows' and not rel.startswith(linkage + 'windows/'):
        return False
    if rel.startswith('buildresult/'):
        outputs = WINDOWS_SERVER_OUTPUTS | ({'stunnel.exe', 'tstunnel.exe'} if include_stunnel else set())
        return path.parent == ROOT / 'buildresult' and path.name in outputs
    # Shared dependency source directories can also contain Unix build output.
    if path.suffix in {'.so', '.dylib'} or '.so.' in path.name:
        return False
    return True


def plan(tracked, scope='all', include_stunnel=False):
    selected = []
    preserved = []
    directories = []

    def visit(folder, in_build=False, in_cache=False):
        if is_link(folder):
            preserved.append((relative(folder), 'linked directory'))
            return
        if (folder / '.git').exists():
            preserved.append((relative(folder), 'nested repository'))
            return
        in_build = in_build or build_directory(folder)
        in_cache = in_cache or folder.name in CACHE_DIRECTORIES
        directories.append(folder)
        for path in sorted(folder.iterdir()):
            rel = relative(path)
            if scope == 'server-windows' and not windows_server_path(path, include_stunnel):
                continue
            if path.name in SKIP_DIRECTORIES or rel in DEPENDENCY_DIRECTORIES or rel == WASM + '/prebuilt-backup':
                continue
            if path.name.lower() in RUNTIME_DIRECTORIES:
                preserved.append((rel, 'runtime data'))
                continue
            if is_link(path):
                # Unlink only a generated library link itself; never walk its destination.
                if in_build and path.is_symlink() and not path.is_dir() and rel.casefold() not in tracked \
                        and any(fnmatch.fnmatch(path.name, pattern) for pattern in ('*.so', '*.so.*', '*.dylib')):
                    selected.append(path)
                else:
                    preserved.append((rel, 'link or junction'))
                continue
            if path.is_dir():
                visit(path, in_build, in_cache)
                continue
            if rel.casefold() in tracked or runtime_file(path):
                continue
            # Published Wasm and browser assets remain build inputs and fallback resources.
            if rel.startswith(BROWSER + '/main/') or rel.startswith(DESKTOP + '/assets/') or rel.startswith(DESKTOP + '/fonts/'):
                continue
            windows_generated = scope == 'server-windows' and (
                rel.startswith(STUNNEL + '/win-build/')
                or rel.startswith(STUNNEL + '/stunnel-5.75/obj/MGW32/')
                or rel.startswith(STUNNEL + '/stunnel-5.75/bin/MGW32/')
                or rel in {SERVER + '/third-party/' + name for name in WINDOWS_PACKAGE_FILES}
                or (rel.startswith(SERVER + '/third-party/') and path.suffix == '.d'))
            if windows_generated or generated_file(path, in_build, in_cache):
                selected.append(path)
            elif in_build or in_cache:
                preserved.append((rel, 'not a recognised artifact or modified live asset'))

    scopes = SCOPES
    if scope == 'server-windows':
        scopes = (SERVER + '/main', SERVER + '/build/windows', 'buildresult')
        scopes += tuple(SERVER + '/third-party/' + name for name in WINDOWS_SERVER_LIBRARIES)
        if include_stunnel:
            scopes += (STUNNEL + '/win-build', STUNNEL + '/stunnel-5.75/obj/MGW32',
                       STUNNEL + '/stunnel-5.75/bin/MGW32')
    for directory in scopes:
        path = ROOT / directory
        if path.exists():
            visit(path)
    return selected, preserved, directories


def check_path(path):
    # Recheck the canonical parent immediately before unlinking; directory links are never followed.
    parent = path.parent
    while parent != ROOT:
        if ROOT not in parent.parents or is_link(parent):
            raise RuntimeError('Refusing a path outside the repository or through a linked directory: ' + str(path))
        parent = parent.parent
    path.parent.resolve().relative_to(ROOT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dry-run', action='store_true', help='list the files without removing anything')
    parser.add_argument('--report', type=Path, help='also write the complete deletion plan to this text file')
    parser.add_argument('--scope', choices=('all', 'server-windows'), default='all',
                        help='limit cleanup to the Windows server build and its dependencies')
    parser.add_argument('--include-stunnel', action='store_true',
                        help='also clean Windows stunnel/OpenSSL output when rebuilding it')
    parser.add_argument('--quiet', action='store_true', help='print only the result or a failure')
    args = parser.parse_args()
    if args.include_stunnel and args.scope != 'server-windows':
        parser.error('--include-stunnel requires --scope server-windows')
    sentinels = (SERVER + '/main/CMakeLists.txt',)
    if args.scope == 'all':
        sentinels += (DESKTOP + '/CMakeLists.txt', BROWSER + '/build.cjs')
    for sentinel in sentinels:
        if not (ROOT / sentinel).is_file():
            raise RuntimeError('Repository layout is incomplete; refusing cleanup.')
    # A source archive has no Git metadata. Its Windows build can still clean the
    # explicitly selected artifacts. A checkout must protect its tracked files.
    tracked = set() if args.scope == 'server-windows' and not (ROOT / '.git').exists() else tracked_files()
    files, preserved, directories = plan(tracked, args.scope, args.include_stunnel)
    lines = [relative(path) for path in files]
    if args.report:
        report = args.report.resolve()
        if report.exists() or str(report).casefold() in {str(path).casefold() for path in files}:
            raise RuntimeError('The report must be a new file outside the deletion plan.')
        report.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    for path in files:
        if not args.quiet:
            print(('WOULD REMOVE ' if args.dry_run else 'REMOVE ') + relative(path))
        if not args.dry_run:
            check_path(path)
            if relative(path).casefold() in tracked or runtime_file(path):
                raise RuntimeError('A protected file reached the deletion step: ' + str(path))
            path.unlink()
    if not args.dry_run:
        # Remove only empty directories affected by this cleanup. No recursive directory deletion.
        touched = {parent for path in files for parent in path.parents if parent != ROOT and ROOT in parent.parents}
        for folder in sorted(touched, key=lambda value: len(value.parts), reverse=True):
            if folder in directories and folder.exists() and not is_link(folder):
                check_path(folder / '.empty-directory-check')
                try:
                    folder.rmdir()
                except OSError:
                    pass
    for name, reason in preserved:
        if not args.quiet:
            print('KEEP ' + name + ' (' + reason + ')')
    print(('Dry run: ' if args.dry_run else 'Cleaned: ') + str(len(files)) + ' build files. Tracked files, source, tools and runtime data are kept.')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print('Cleanup stopped: ' + str(error), file=sys.stderr)
        sys.exit(1)
