#!/usr/bin/env python3
"""Formats the C, C++, Java and JavaScript sources of the repository with clang-format and js-beautify."""
# There are three rules files in this folder.
# - .clang-format holds the C, C++ and Java rules of each programmer. Change it to format the code in a personal
#   style while working. format-codebase.bat (Windows) and format-codebase.sh (Linux, macOS) use it. They ask which
#   part to format, server-source-code, client-source-code or both.
# - before-commit.clang-format and before-commit.jsbeautifyrc hold the project rules for C, C++ and Java and for
#   JavaScript and are not to be changed. format-before-commit.bat and format-before-commit.sh use them. They must be
#   run before every commit. They format the C, C++, Java and JavaScript files that differ from the last commit with
#   these rules, clang-format 23.1.5 and js-beautify 2.0.3, so every commit keeps the project style and contains no
#   changes that only come from another style or another tool version. The rules are read as they are in the last
#   commit, so a local change of these files does not count. The Java rules are a second section of the clang-format
#   rules files. While the rules in the last commit have no Java section, the Java files are left unchanged before a
#   commit.
#
# Every .c, .h, .cpp, .hpp and .java file (and before a commit every .js, .cjs and .mjs file) of the chosen part is
# formatted. Folders named third-party are skipped, and so are other folders of bundled libraries, build output
# folders, folders whose name starts with a dot, links, empty files and the files that Git ignores.
#
# js-beautify 2.0.3 is downloaded once into tools/js-beautify-2.0.3 when a JavaScript file is to be formatted, after
# asking. The download is the Python package of PyPI (https://pypi.org/project/jsbeautifier/2.0.3/), checked against
# js-beautify-downloads.txt.
#
# clang-format 23.1.5 is downloaded once into tools/clang-format-23.1.5/<system> when it is missing, after asking. The
# downloads are the clang-format packages of PyPI (https://pypi.org/project/clang-format/23.1.5/), which hold the
# clang-format program of LLVM for one system each. They are checked against clang-format-downloads.txt.
# install-clang-format.bat and install-clang-format.sh only download it. With own rules, the CLANG_FORMAT environment
# variable or a clang-format on the PATH can be used instead. Before a commit only 23.1.5 is used, because other
# versions can format differently.
#
# Options
#   --before-commit              formats the changed C, C++, Java and JavaScript files with the project rules
#   --target server|client|all   the part to format with own rules, asked when omitted
#   --check                      lists the files that would change and changes nothing, exit code 1 when any would
#   --list                       lists the files that would be formatted and ends
#   --style-file PATH            other own rules than tools/.clang-format
#   --install                    downloads clang-format and ends
#   --download-tools             downloads clang-format and js-beautify without asking when they are missing
#   --no-download-tools          never downloads
# The script needs Python 3.8 or newer and only its standard library. Git is needed for --before-commit to find the
# changed files. Without Git every file is formatted.
import argparse
import concurrent.futures
import glob
import hashlib
import io
import json
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
VERSION = '23.1.5'
INSTALL_FOLDER = os.path.join(TOOLS, 'clang-format-' + VERSION)
PROGRAM_NAME = 'clang-format.exe' if sys.platform == 'win32' else 'clang-format'
OWN_RULES = os.path.join(TOOLS, '.clang-format')
COMMIT_RULES = os.path.join(TOOLS, 'before-commit.clang-format')
COMMIT_RULES_IN_GIT = 'tools/before-commit.clang-format'
JS_VERSION = '2.0.3'
JS_FOLDER = os.path.join(TOOLS, 'js-beautify-' + JS_VERSION)
JS_RULES = os.path.join(TOOLS, 'before-commit.jsbeautifyrc')
JS_RULES_IN_GIT = 'tools/before-commit.jsbeautifyrc'
TARGETS = {'server': ['server-source-code'], 'client': ['client-source-code'], 'all': ['server-source-code', 'client-source-code']}
EXTENSIONS = ('.c', '.h', '.cpp', '.hpp', '.java')
JS_EXTENSIONS = ('.js', '.cjs', '.mjs')
# folders without own source code, which are bundled libraries, build output and downloaded tools. nodejs-mobile-include
# holds the Node.js headers that the Android client build downloads, vendor the ws library of the Node.js client and
# legacy-reference the old JavaScript RSA libraries that a test compares against.
SKIPPED_FOLDERS = {'third-party', 'third_party', 'node_modules', 'nodejs-mobile-include', 'jniLibs', 'vendor', 'legacy-reference',
                   'build', 'buildresult', 'CMakeFiles', 'linkage-files'}
SKIPPED_PREFIXES = ('.', 'build-', 'cmake-build-')
COMMIT_REMINDER = ('Before every commit, run format-before-commit.bat or format-before-commit.sh in the tools folder. It formats '
                   'the changed C, C++, Java and JavaScript files with the project rules, so the commit keeps the project style and '
                   'contains no formatting changes.')


def fail(message):
    """prints the reason and ends the script with exit code 1"""
    print(message, file=sys.stderr)
    sys.exit(1)


def ask(question):
    """asks a question on the terminal and returns the answer, or None when there is no input"""
    try:
        answer = input(question)
    except EOFError:
        return None

    # byte order mark that PowerShell puts before piped text
    return answer.replace('﻿', '').strip()


def git(*arguments):
    """runs Git in the repository and returns its output, or None when Git or the repository is not available"""
    try:
        result = subprocess.run(['git', '-C', ROOT] + list(arguments), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError:
        return None
    return result.stdout if result.returncode == 0 else None


def system_key():
    """the key of this system in clang-format-downloads.txt, or None when no package exists for it"""
    machine = platform.machine().lower()
    if sys.platform == 'win32':
        machine = (os.environ.get('PROCESSOR_ARCHITEW6432') or os.environ.get('PROCESSOR_ARCHITECTURE') or machine).lower()
        return {'amd64': 'windows-x64', 'x86': 'windows-x86', 'arm64': 'windows-arm64'}.get(machine)
    if sys.platform == 'darwin':
        return 'macos-arm64' if machine == 'arm64' else 'macos-x64'
    if sys.platform.startswith('linux'):
        name = {'x86_64': 'x64', 'amd64': 'x64', 'aarch64': 'arm64', 'arm64': 'arm64', 'armv7l': 'armv7', 'armv8l': 'armv7',
                'i686': 'x86', 'i586': 'x86', 'i386': 'x86', 'ppc64le': 'ppc64le', 's390x': 's390x'}.get(machine)

        # 32-bit system on a 64-bit kernel, for example Raspberry Pi OS 32-bit, which reports aarch64
        if sys.maxsize < 2 ** 32:
            name = {'arm64': 'armv7', 'x64': 'x86'}.get(name, name)
        is_musl = bool(glob.glob('/lib/ld-musl-*.so.1'))
        return None if name is None else ('linux-musl-' if is_musl else 'linux-') + name
    return None


def read_downloads():
    """reads clang-format-downloads.txt into a dictionary that maps each key to its SHA-256 and address"""
    downloads = {}
    with open(os.path.join(TOOLS, 'clang-format-downloads.txt'), encoding='utf-8') as file:
        for line in file:
            parts = line.split()
            if len(parts) == 3 and not line.startswith('#'):
                downloads[parts[0]] = (parts[1], parts[2])
    return downloads


def program_version(program):
    """the version of a clang-format program, or None when it does not start"""
    try:
        result = subprocess.run([program, '--version'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    match = re.search(r'clang-format version (\d+\.\d+\.\d+)', result.stdout.decode('utf-8', 'replace'))
    return match.group(1) if result.returncode == 0 and match else None


def program_folder():
    """the folder of the downloaded clang-format of this system, one per system so that a checkout can be shared"""
    return os.path.join(INSTALL_FOLDER, system_key() or 'unknown')


def installed_program():
    """the downloaded clang-format when it is present and starts, or None"""
    program = os.path.join(program_folder(), PROGRAM_NAME)
    return program if os.path.isfile(program) and program_version(program) == VERSION else None


def install():
    """downloads clang-format for this system into tools/clang-format-<version> and returns the program"""
    key = system_key()
    downloads = read_downloads()
    if key not in downloads:
        fail('No clang-format package exists for this system (' + sys.platform + ' ' + platform.machine() + '). Install '
             + 'clang-format ' + VERSION + ' with the package manager of the system and set CLANG_FORMAT to it. No files were changed.')
    checksum, address = downloads[key]

    # download and check
    print('Downloading clang-format ' + VERSION + ' for ' + key + ' from ' + address)
    with urllib.request.urlopen(address) as response:
        package_data = response.read()
    if hashlib.sha256(package_data).hexdigest() != checksum:
        fail('The downloaded package does not match its checksum and was not kept. No files were changed.')

    # program and licenses
    folder = program_folder()
    partial = folder + '.partial'
    shutil.rmtree(partial, ignore_errors=True)
    os.makedirs(partial)
    with zipfile.ZipFile(io.BytesIO(package_data)) as package:
        with open(os.path.join(partial, PROGRAM_NAME), 'wb') as file:
            file.write(package.read('clang_format/data/bin/' + PROGRAM_NAME))
        for member in package.namelist():
            if '.dist-info/licenses/' in member and not member.endswith('/'):
                with open(os.path.join(partial, os.path.basename(member)), 'wb') as file:
                    file.write(package.read(member))
    os.chmod(os.path.join(partial, PROGRAM_NAME), 0o755)
    shutil.rmtree(folder, ignore_errors=True)
    os.replace(partial, folder)

    # program starts
    program = installed_program()
    if program is None:
        shutil.rmtree(folder, ignore_errors=True)
        fail('The downloaded clang-format does not start on this system, for example because its C library is older '
             + 'than the program needs. Install clang-format ' + VERSION + ' with the package manager of the system and '
             + 'set CLANG_FORMAT to it. No files were changed.')
    print('clang-format ' + VERSION + ' is installed in ' + folder + '.')
    return program


def find_program(policy, is_exact_version):
    """returns the clang-format to use, downloading the pinned version when it is missing and allowed"""

    # downloaded program
    program = installed_program()
    if program is not None:
        return program

    # program from CLANG_FORMAT
    chosen = os.environ.get('CLANG_FORMAT')
    if chosen:
        version = program_version(chosen)
        if version is None:
            fail('CLANG_FORMAT is set to ' + chosen + ', which does not start as clang-format. No files were changed.')
        if version == VERSION:
            return chosen
        if not is_exact_version:
            print('Using clang-format ' + version + ' from CLANG_FORMAT. Versions other than ' + VERSION + ' can format differently.')
            return chosen
        print('CLANG_FORMAT is clang-format ' + version + '. Before a commit only ' + VERSION + ' is used.')

    # permission to download
    is_allowed = policy == 'yes'
    if policy == 'ask':
        answer = ask('clang-format ' + VERSION + ' is not installed. Download it from PyPI (about 2 MB) into '
                     + 'tools/clang-format-' + VERSION + '? [y/N] ')
        is_allowed = (answer or '').lower() in ('y', 'yes')
    if is_allowed:
        return install()
    if is_exact_version:
        fail('Formatting before a commit needs clang-format ' + VERSION + ', because other versions can format differently. '
             + 'Run install-clang-format.bat or install-clang-format.sh. No files were changed.')

    # program on the path
    found = shutil.which('clang-format')
    version = program_version(found) if found else None
    if version is None:
        fail('clang-format was not found. Run install-clang-format.bat or install-clang-format.sh, or set CLANG_FORMAT. '
             + 'No files were changed.')
    if int(version.split('.')[0]) < 14:
        fail('clang-format ' + version + ' on the PATH is older than 14, which is the first version that reads the rules '
             + 'from another folder. Run install-clang-format.bat or install-clang-format.sh. No files were changed.')
    if version != VERSION:
        print('Using clang-format ' + version + ' from the PATH. Versions other than ' + VERSION + ' can format differently.')
    return found


def is_link(path):
    """true for a symbolic link or a Windows directory junction"""
    try:
        info = os.lstat(path)
    except OSError:
        return True
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, 'st_file_attributes', 0) & 0x400)


def relative(path):
    """a path relative to the repository with forward slashes, as Git writes it"""
    return os.path.relpath(path, ROOT).replace(os.sep, '/')


def ignored_by_git(paths):
    """the paths among the given ones that Git ignores, or an empty set when Git or the repository is not available"""
    try:
        result = subprocess.run(['git', '-C', ROOT, 'check-ignore', '--stdin', '-z'], input=b'\0'.join(os.fsencode(relative(path)) for path in paths),
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError:
        return set()

    # exit code 1 means that no path is ignored, 128 that this is not a Git repository
    if result.returncode != 0:
        return set()
    return {os.fsdecode(name) for name in result.stdout.split(b'\0') if name}


def source_files(parts, extensions=EXTENSIONS):
    """the source files with the given extensions in the repository folders, without third-party code, build output and links"""
    files = []
    for part in parts:
        for folder, names, file_names in os.walk(os.path.join(ROOT, part)):

            # skipped folders
            names[:] = sorted(name for name in names if name not in SKIPPED_FOLDERS and not name.startswith(SKIPPED_PREFIXES)
                              and not is_link(os.path.join(folder, name)))

            # source files
            for name in sorted(file_names):
                path = os.path.join(folder, name)
                if name.endswith(extensions) and not is_link(path):
                    files.append(path)

    # files of the repository
    ignored = ignored_by_git(files)
    return [path for path in files if relative(path) not in ignored]


def changed_files(files):
    """the files among the given ones that differ from the last commit or are new, or None without Git or commit"""
    changed = git('diff', '--name-only', '-z', 'HEAD')
    new = git('ls-files', '--others', '--exclude-standard', '-z')
    if changed is None or new is None:
        return None
    names = {os.fsdecode(name) for name in (changed + b'\0' + new).split(b'\0') if name}
    return [path for path in files if relative(path) in names]


def committed_rules(path_in_git, local_path):
    """a project rules file as it is in the last commit, written to a temporary file, or the file itself, and whether
    the returned file is temporary"""
    committed = git('show', 'HEAD:' + path_in_git)
    if committed is None:
        if not os.path.isfile(local_path):
            fail('The project rules ' + local_path + ' were not found. No files were changed.')
        return local_path, False

    # local change of the rules
    with open(local_path, 'rb') as file:
        if file.read() != committed:
            print(os.path.basename(local_path) + ' differs from the last commit. The committed rules are used, because this file is not to be changed.')
    handle, path = tempfile.mkstemp(suffix='-' + os.path.basename(local_path))
    with os.fdopen(handle, 'wb') as file:
        file.write(committed)
    return path, True


def read_js_download():
    """the SHA-256 and address of the js-beautify package in js-beautify-downloads.txt"""
    with open(os.path.join(TOOLS, 'js-beautify-downloads.txt'), encoding='utf-8') as file:
        for line in file:
            parts = line.split()
            if len(parts) == 3 and not line.startswith('#'):
                return parts[1], parts[2]
    fail('js-beautify-downloads.txt holds no package. No files were changed.')


def install_js_beautify():
    """downloads js-beautify into tools/js-beautify-<version>"""
    checksum, address = read_js_download()

    # download and check
    print('Downloading js-beautify ' + JS_VERSION + ' from ' + address)
    with urllib.request.urlopen(address) as response:
        package_data = response.read()
    if hashlib.sha256(package_data).hexdigest() != checksum:
        fail('The downloaded package does not match its checksum and was not kept. No files were changed.')

    # package files and the license notice in its metadata
    partial = JS_FOLDER + '.partial'
    shutil.rmtree(partial, ignore_errors=True)
    with zipfile.ZipFile(io.BytesIO(package_data)) as package:
        for member in package.namelist():
            if member.startswith('jsbeautifier/') and not member.endswith('/') and '..' not in member.split('/'):
                target = os.path.join(partial, *member.split('/'))
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target, 'wb') as file:
                    file.write(package.read(member))
        with open(os.path.join(partial, 'METADATA'), 'wb') as file:
            file.write(package.read('jsbeautifier-' + JS_VERSION + '.dist-info/METADATA'))
    shutil.rmtree(JS_FOLDER, ignore_errors=True)
    os.replace(partial, JS_FOLDER)
    print('js-beautify ' + JS_VERSION + ' is installed in ' + JS_FOLDER + '.')


def load_js_beautify(policy):
    """imports js-beautify from tools/js-beautify-<version>, downloading it first when it is missing and allowed"""

    # permission to download
    if not os.path.isfile(os.path.join(JS_FOLDER, 'jsbeautifier', '__init__.py')):
        is_allowed = policy == 'yes'
        if policy == 'ask':
            answer = ask('js-beautify ' + JS_VERSION + ' is not installed. Download it from PyPI (about 92 KB) into '
                         + 'tools/js-beautify-' + JS_VERSION + '? [y/N] ')
            is_allowed = (answer or '').lower() in ('y', 'yes')
        if not is_allowed:
            fail('Formatting JavaScript before a commit needs js-beautify ' + JS_VERSION + '. Allow the download or pass '
                 + '--download-tools. No files were changed.')
        install_js_beautify()

    # pinned version
    sys.path.insert(0, JS_FOLDER)
    import jsbeautifier
    if jsbeautifier.__version__ != JS_VERSION:
        fail('tools/js-beautify-' + JS_VERSION + ' holds js-beautify ' + jsbeautifier.__version__ + '. Delete the folder and run '
             + 'the script again. No files were changed.')
    return jsbeautifier


def format_js_file(jsbeautifier, rules, path, is_check):
    """formats one JavaScript file with js-beautify and returns (path, state), where state is changed, unchanged or
    the error text. The line endings and a byte order mark of the file are kept."""
    with open(path, 'rb') as file:
        original = file.read()
    if not original.strip():
        return path, 'unchanged'
    has_bom = original.startswith(b'\xef\xbb\xbf')
    try:
        source = original[3 if has_bom else 0:].decode('utf-8')
    except UnicodeDecodeError:
        return path, 'error the file is not UTF-8'

    # formatted text
    options = jsbeautifier.default_options()
    for name, value in rules.items():
        setattr(options, name, value)
    options.eol = '\n'
    try:
        result = jsbeautifier.beautify(source.replace('\r\n', '\n'), options)
    except Exception as error:
        return path, 'error ' + str(error)

    # line endings of the file
    crlf = source.count('\r\n')
    if crlf > source.count('\n') - crlf:
        result = result.replace('\n', '\r\n')
    formatted = (b'\xef\xbb\xbf' if has_bom else b'') + result.encode('utf-8')
    if formatted == original:
        return path, 'unchanged'

    # file written
    if not is_check:
        with open(path, 'wb') as file:
            file.write(formatted)
    return path, 'changed'


def format_file(program, style_file, path, is_check):
    """formats one file and returns (path, state), where state is changed, unchanged or the error text"""
    with open(path, 'rb') as file:
        original = file.read()

    # formatted text
    result = subprocess.run([program, '--style=file:' + style_file, path], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        return path, 'error ' + result.stderr.decode('utf-8', 'replace').strip()
    if result.stdout == original:
        return path, 'unchanged'

    # file written
    if not is_check:
        with open(path, 'wb') as file:
            file.write(result.stdout)
    return path, 'changed'


def format_files(tool, files, formatter, is_check, workers):
    """formats the files with formatter(path), with the given number of files at the same time, prints the changed
    files and the errors and returns the exit code"""
    print(('Checking ' if is_check else 'Formatting ') + str(len(files)) + ' files with ' + tool + '.')
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(pool.map(formatter, files))

    # summary
    changed = [path for path, state in results if state == 'changed']
    errors = [(path, state[len('error '):]) for path, state in results if state.startswith('error')]
    for path in changed:
        print(('would change ' if is_check else 'changed ') + relative(path))
    for path, text in errors:
        print('failed ' + relative(path) + ' ' + text, file=sys.stderr)
    if is_check:
        print(str(len(changed)) + ' of ' + str(len(files)) + ' files would change. No files were changed.')
    else:
        print(str(len(changed)) + ' of ' + str(len(files)) + ' files changed.')
    if errors:
        print(str(len(errors)) + ' files could not be formatted and were not changed.', file=sys.stderr)
    return 1 if errors or (is_check and changed) else 0


def before_commit(policy, is_check):
    """formats the changed C, C++, Java and JavaScript files with the project rules and the pinned tools, returns the exit code"""
    c_files = source_files(TARGETS['all'])
    js_files = source_files(TARGETS['all'], JS_EXTENSIONS)

    # changed files
    changed_c = changed_files(c_files)
    changed_js = changed_files(js_files)
    if changed_c is None:
        print('Git or the last commit is not available, so every file is formatted.')
    else:
        c_files, js_files = changed_c, changed_js
    code = 0

    # C, C++ and Java
    if c_files:
        style_file, is_temporary = committed_rules(COMMIT_RULES_IN_GIT, COMMIT_RULES)
        try:
            print('Project rules of before-commit.clang-format' + (' as in the last commit.' if is_temporary else '.'))

            # java section check
            with open(style_file, encoding='utf-8') as file:
                is_java_section_present = re.search(r'^Language:\s*Java\s*$', file.read(), re.MULTILINE) is not None
            clang_files = c_files if is_java_section_present else [path for path in c_files if not path.endswith('.java')]
            if len(clang_files) < len(c_files):
                print(str(len(c_files) - len(clang_files)) + ' Java files are left unchanged, because the project rules in the last '
                      'commit have no Java section yet. They are formatted after tools/before-commit.clang-format with its Java '
                      'section is committed.')

            # file formatting
            if clang_files:
                program = find_program(policy, True)
                code = format_files('clang-format ' + str(program_version(program)), clang_files,
                                    lambda path: format_file(program, style_file, path, is_check), is_check, os.cpu_count() or 4)
        finally:
            if is_temporary:
                os.remove(style_file)

    # JavaScript
    if js_files:
        jsbeautifier = load_js_beautify(policy)
        rules_file, is_temporary = committed_rules(JS_RULES_IN_GIT, JS_RULES)
        try:
            with open(rules_file, encoding='utf-8') as file:
                rules = json.load(file)
        finally:
            if is_temporary:
                os.remove(rules_file)
        print('Project rules of before-commit.jsbeautifyrc' + (' as in the last commit.' if is_temporary else '.'))
        js_code = format_files('js-beautify ' + JS_VERSION, js_files,
                               lambda path: format_js_file(jsbeautifier, rules, path, is_check), is_check, 1)
        code = max(code, js_code)
    if not c_files and not js_files:
        print('No C, C++, Java or JavaScript file differs from the last commit.')

    # files in the index
    if not is_check and code == 0 and (c_files or js_files):
        print('Add changed files again with git add before the commit, because Git keeps the version that was added before.')
    return code


def main():
    parser = argparse.ArgumentParser(description='Formats the C, C++, Java and JavaScript sources. Before every commit, run it with --before-commit.')
    parser.add_argument('--before-commit', action='store_true', help='formats the changed C, C++, Java and JavaScript files with the project rules')
    parser.add_argument('--target', choices=list(TARGETS), help='the part to format with own rules, asked when omitted')
    parser.add_argument('--check', action='store_true', help='lists the files that would change and changes nothing')
    parser.add_argument('--list', action='store_true', help='lists the files that would be formatted and ends')
    parser.add_argument('--style-file', default=OWN_RULES, help='own rules, tools/.clang-format when omitted')
    parser.add_argument('--install', action='store_true', help='downloads clang-format and ends')
    policy = parser.add_mutually_exclusive_group()
    policy.add_argument('--download-tools', dest='policy', action='store_const', const='yes')
    policy.add_argument('--no-download-tools', dest='policy', action='store_const', const='no')
    arguments = parser.parse_args()

    # download only
    if arguments.install:
        if installed_program() is not None:
            print('clang-format ' + VERSION + ' is already installed in ' + program_folder() + '.')
        else:
            install()
        return 0

    # project rules
    if arguments.before_commit:
        return before_commit(arguments.policy or 'ask', arguments.check)

    # part to format
    target = arguments.target
    if target is None:
        print('Which part should be formatted with the own rules of tools/.clang-format? Folders named third-party are skipped.')
        print('1. server-source-code')
        print('2. client-source-code (all client projects)')
        print('3. both')
        while target is None:
            answer = ask('Choose [1/2/3]: ')
            if answer is None:
                fail('No answer was given. Pass --target server, client or all. No files were changed.')
            target = {'1': 'server', '2': 'client', '3': 'all'}.get(answer)

    # rules and files
    style_file = os.path.abspath(arguments.style_file)
    if not os.path.isfile(style_file):
        fail('The rules file ' + style_file + ' was not found. No files were changed.')
    files = source_files(TARGETS[target])
    if arguments.list:
        for path in files:
            print(relative(path))
        print(str(len(files)) + ' files')
        return 0

    # own rules
    program = find_program(arguments.policy or 'ask', False)
    print('Rules of ' + relative(style_file) + '.')
    code = format_files('clang-format ' + str(program_version(program)), files,
                        lambda path: format_file(program, style_file, path, arguments.check), arguments.check, os.cpu_count() or 4)
    print(COMMIT_REMINDER)
    return code


if __name__ == '__main__':
    sys.exit(main())
