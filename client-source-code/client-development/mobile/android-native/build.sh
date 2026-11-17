#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
CHECK_ONLY=false
INTERACTIVE=true
ABI=""
VARIANT=debug
VARIANT_GIVEN=false
JAVA_DIR=""
SDK_DIR=""
SDK_MANAGER=""
TEMP_DIR=""
MISSING_PACKAGES=()

usage()
{
    cat <<'HELP'
Build the native Lemon Chat Android app on Linux x86_64 or macOS.

Usage: ./build.sh [--check] [--abi ABI] [--variant debug|release] [--non-interactive]

  --check             Check local prerequisites without downloads or file changes
  --abi ABI           all, arm64-v8a, armeabi-v7a, or x86_64
  --variant VARIANT   debug (installable) or release (unsigned)
  --non-interactive   Use existing tools and licenses without installation prompts
  --help              Show this help

Interactive builds offer architecture and build type menus unless set by flags.
ARMv6 is unsupported. armeabi-v7a requires ARMv7 and Android 8.0 or newer.
The SDK comes from local.properties first, then ANDROID_HOME/ANDROID_SDK_ROOT.
Keep this script inside the complete lemon-chat repository checkout.
Downloads and shell environment changes require your agreement.
Gradle downloads its wrapper and project dependencies during the build.
Set LEMON_BUILD_HOME to choose the directory for downloaded build tools.
HELP
}

fail()
{
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

confirm()
{
    local answer
    if ! "$INTERACTIVE" || "$CHECK_ONLY"; then
        return 1
    fi
    printf '%s [1 yes / 2 no, default 2] ' "$*"
    read -r answer || return 1
    case "$answer" in
        1|y|Y|yes|YES) return 0 ;;
        *) return 1 ;;
    esac
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --check) CHECK_ONLY=true ;;
        --non-interactive) INTERACTIVE=false ;;
        --abi|--variant)
            [ "$#" -ge 2 ] || fail "$1 requires a value"
            case "$1" in
                --abi) ABI="$2" ;;
                --variant) VARIANT="$2"; VARIANT_GIVEN=true ;;
            esac
            shift
            ;;
        *) fail "Unknown option: $1. Use --help for usage" ;;
    esac
    shift
done

case "$ABI" in
    ""|all|arm64-v8a|armeabi-v7a|x86_64) ;;
    armv6|armeabi) fail 'ARMv6 is unsupported by this app and the selected Android NDK' ;;
    *) fail "Unsupported ABI: $ABI" ;;
esac
case "$VARIANT" in
    debug|release) ;;
    *) fail "Unsupported variant: $VARIANT" ;;
esac
if [ ! -t 0 ]; then
    INTERACTIVE=false
fi

HOST_OS="$(uname -s)"
HOST_ARCH="$(uname -m)"
case "$HOST_OS:$HOST_ARCH" in
    Linux:x86_64)
        ADOPTIUM_OS=linux
        ADOPTIUM_ARCH=x64
        NDK_HOST=linux-x86_64
        TOOL_ROOT="${XDG_DATA_HOME:-$HOME/.local/share}/lemon-android-tools"
        SDK_ARCHIVE=commandlinetools-linux-13114758_latest.zip
        SDK_SHA256=7ec965280a073311c339e571cd5de778b9975026cfcbe79f2b1cdcb1e15317ee
        ;;
    Darwin:x86_64|Darwin:arm64)
        if [ -x /opt/homebrew/bin/brew ]; then
            export PATH="/opt/homebrew/bin:$PATH"
        elif [ -x /usr/local/bin/brew ]; then
            export PATH="/usr/local/bin:$PATH"
        fi
        ADOPTIUM_OS=mac
        ADOPTIUM_ARCH=x64
        NDK_HOST=darwin-x86_64
        if [ "$HOST_ARCH" = arm64 ]; then
            ADOPTIUM_ARCH=aarch64
        fi
        TOOL_ROOT="$HOME/Library/Application Support/Lemon Android Tools"
        SDK_ARCHIVE=commandlinetools-mac-13114758_latest.zip
        SDK_SHA256=5673201e6f3869f418eeed3b5cb6c4be7401502bd0aae1b12a29d164d647a54e
        ;;
    *) fail "Unsupported build host: $HOST_OS $HOST_ARCH. Use Linux x86_64, macOS, or build.bat on Windows" ;;
esac
TOOL_ROOT="${LEMON_BUILD_HOME:-$TOOL_ROOT}"
case "$TOOL_ROOT" in
    /*) ;;
    *) TOOL_ROOT="$PWD/$TOOL_ROOT" ;;
esac
REPOSITORY_DIR="$(cd -- "$PROJECT_DIR/../../../.." && pwd -P)"
for required_source in client-source-code/client-development/desktop/main/crypto/crypto_session.c server-source-code/third-party/libopus-1.6.1/CMakeLists.txt server-source-code/third-party/libdatachannel-0.24.2/CMakeLists.txt client-source-code/client-development/browser/main/scripts/app/audio/sounds.js; do
    [ -f "$REPOSITORY_DIR/$required_source" ] || fail "Missing shared source: $required_source. Clone the complete lemon-chat repository"
done
[ -f "$PROJECT_DIR/gradle/wrapper/gradle-wrapper.jar" ] && [ -f "$PROJECT_DIR/gradlew" ] || fail 'The Gradle wrapper is missing. Restore the complete project from Git'

install_homebrew()
{
    local installer
    printf 'Python 3 is needed to verify and unpack tool downloads. Homebrew can install it.\n'
    printf 'Homebrew installation: https://docs.brew.sh/Installation\n'
    confirm 'Download and run the official Homebrew installer? It may request your administrator password' || fail 'Helper installation declined'
    installer="$(mktemp "${TMPDIR:-/tmp}/lemon-homebrew.XXXXXXXX")"
    if ! /usr/bin/curl --fail --location --proto '=https' --proto-redir '=https' --tlsv1.2 --output "$installer" https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh; then
        rm -f -- "$installer"
        fail 'Homebrew installer download failed'
    fi
    if ! /bin/bash "$installer"; then
        rm -f -- "$installer"
        fail 'Homebrew installation did not finish'
    fi
    rm -f -- "$installer"
    if [ -x /opt/homebrew/bin/brew ]; then
        export PATH="/opt/homebrew/bin:$PATH"
    elif [ -x /usr/local/bin/brew ]; then
        export PATH="/usr/local/bin:$PATH"
    else
        fail 'Homebrew was not found after installation'
    fi
}

install_host_dependencies()
{
    local manager="" packages=() privilege=()
    if command -v apt-get >/dev/null 2>&1; then
        manager=apt-get
        packages=(install python3 curl ca-certificates)
    elif command -v dnf >/dev/null 2>&1; then
        manager=dnf
        packages=(install python3 curl ca-certificates)
    elif command -v pacman >/dev/null 2>&1; then
        manager=pacman
        packages=(-S --needed python curl ca-certificates)
    elif command -v zypper >/dev/null 2>&1; then
        manager=zypper
        packages=(install python3 curl ca-certificates)
    elif command -v brew >/dev/null 2>&1; then
        manager=brew
        packages=(install python curl)
    elif [ "$HOST_OS" = Darwin ]; then
        install_homebrew
        manager=brew
        packages=(install python curl)
    else
        fail 'Install Python 3 and curl using your system package manager, then run this script again'
    fi
    if [ "$manager" != brew ] && [ "$(id -u)" != 0 ]; then
        command -v sudo >/dev/null 2>&1 || fail "Run $manager yourself to install Python 3 and curl"
        privilege=(sudo)
    fi
    printf 'Required helper command:'
    printf ' %q' "${privilege[@]}" "$manager" "${packages[@]}"
    printf '\n'
    confirm 'Run this package manager command?' || fail 'Required helpers are missing. No packages were installed'
    "${privilege[@]}" "$manager" "${packages[@]}"
}

if ! command -v python3 >/dev/null 2>&1 || ! command -v curl >/dev/null 2>&1; then
    if "$CHECK_ONLY" || ! "$INTERACTIVE"; then
        fail 'Python 3 and curl are required. Check mode and non-interactive mode do not install them'
    fi
    install_host_dependencies
fi
if ! command -v python3 >/dev/null 2>&1 || ! command -v curl >/dev/null 2>&1; then
    fail 'Python 3 or curl is still unavailable'
fi
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)' || fail 'Python 3.8 or newer is required'

java_candidate()
{
    local candidate="$1" java_version compiler_version major
    [ -x "$candidate/bin/java" ] && [ -x "$candidate/bin/javac" ] || return 1
    java_version="$("$candidate/bin/java" -version 2>&1)" || return 1
    compiler_version="$("$candidate/bin/javac" -version 2>&1)" || return 1
    if [[ "$java_version" =~ version\ \"(17|21)([.\"]|$) ]]; then
        major="${BASH_REMATCH[1]}"
        if [[ "$compiler_version" =~ javac\ (17|21)([.[:space:]]|$) ]] && [ "${BASH_REMATCH[1]}" = "$major" ]; then
            JAVA_DIR="$(cd -- "$candidate" && pwd -P)"
            return 0
        fi
    fi
    return 1
}

find_java()
{
    local candidate compiler
    if [ -n "${JAVA_HOME:-}" ] && java_candidate "$JAVA_HOME"; then
        return
    fi
    if command -v javac >/dev/null 2>&1; then
        compiler="$(python3 -c 'import os,sys; print(os.path.dirname(os.path.dirname(os.path.realpath(sys.argv[1]))))' "$(command -v javac)")"
        if java_candidate "$compiler"; then
            return
        fi
    fi
    if [ "$HOST_OS" = Darwin ] && [ -x /usr/libexec/java_home ]; then
        for candidate in 21 17; do
            if compiler="$(/usr/libexec/java_home -v "$candidate" 2>/dev/null)" && java_candidate "$compiler"; then
                return
            fi
        done
    fi
    for candidate in "$TOOL_ROOT/jdk-21" "$TOOL_ROOT/jdk-21/Contents/Home" /usr/lib/jvm/* /Library/Java/JavaVirtualMachines/*/Contents/Home "/Applications/Android Studio.app/Contents/jbr/Contents/Home" /opt/android-studio/jbr "$HOME/android-studio/jbr"; do
        if java_candidate "$candidate"; then
            return
        fi
    done
}

# decode local.properties without running its contents
SDK_DIR="$(python3 - "$PROJECT_DIR/local.properties" <<'PY'
import os, re, sys
path = sys.argv[1]
if not os.path.isfile(path):
    sys.exit(0)
def decode(value):
    def replacement(match):
        token = match.group(1)
        if token.startswith('u'):
            return chr(int(token[1:], 16))
        return {'t': '\t', 'n': '\n', 'r': '\r', 'f': '\f'}.get(token, token)
    return re.sub(r'\\(u[0-9a-fA-F]{4}|.)', replacement, value)
logical = ''
result = ''
with open(path, encoding='iso-8859-1') as source:
    for line in source:
        line = line.rstrip('\r\n')
        if not logical and line.lstrip().startswith(('#', '!')):
            continue
        logical += line.lstrip() if logical else line
        if (len(logical) - len(logical.rstrip('\\'))) % 2:
            logical = logical[:-1]
            continue
        match = re.match(r'^\s*sdk\.dir\s*(?:[=:]\s*|\s+)(.*)$', logical)
        if match:
            result = decode(match.group(1))
        logical = ''
if result:
    if any(char in result for char in '\n\r\0'):
        sys.exit('Invalid sdk.dir in local.properties')
    print(os.path.abspath(os.path.join(os.path.dirname(path), result)))
PY
)"
if [ -n "$SDK_DIR" ]; then
    printf 'Using sdk.dir from local.properties: %s\n' "$SDK_DIR"
elif [ -n "${ANDROID_HOME:-}" ]; then
    SDK_DIR="$ANDROID_HOME"
elif [ -n "${ANDROID_SDK_ROOT:-}" ]; then
    SDK_DIR="$ANDROID_SDK_ROOT"
elif [ "$HOST_OS" = Darwin ] && [ -d "$HOME/Library/Android/sdk" ]; then
    SDK_DIR="$HOME/Library/Android/sdk"
elif [ -d "$HOME/Android/Sdk" ]; then
    SDK_DIR="$HOME/Android/Sdk"
else
    SDK_DIR="$TOOL_ROOT/sdk"
fi
SDK_DIR="$(python3 -c 'import os,sys; print(os.path.abspath(sys.argv[1]))' "$SDK_DIR")"

cleanup()
{
    if [ -n "$TEMP_DIR" ]; then
        case "$TEMP_DIR" in
            "$TOOL_ROOT"/.install.*) rm -rf -- "$TEMP_DIR" ;;
        esac
    fi
}
trap cleanup EXIT

prepare_download()
{
    mkdir -p -- "$TOOL_ROOT"
    if [ -z "$TEMP_DIR" ]; then
        TEMP_DIR="$(mktemp -d "$TOOL_ROOT/.install.XXXXXXXX")"
    fi
}

download()
{
    curl --fail --location --proto '=https' --proto-redir '=https' --tlsv1.2 --retry 3 --connect-timeout 30 --output "$2" "$1"
}

verify_archive()
{
    python3 - "$1" "$2" <<'PY'
import hashlib, re, sys
if not re.fullmatch(r'[a-fA-F0-9]{64}', sys.argv[2]):
    sys.exit('Invalid SHA256 checksum metadata')
digest = hashlib.sha256()
with open(sys.argv[1], 'rb') as source:
    for block in iter(lambda: source.read(1024 * 1024), b''):
        digest.update(block)
if digest.hexdigest() != sys.argv[2].lower():
    sys.exit('Download checksum mismatch. Archive was not installed')
PY
}

extract_archive()
{
    python3 - "$1" "$2" <<'PY'
import os, pathlib, stat, sys, tarfile, zipfile
source, target = sys.argv[1:]
os.makedirs(target)
root = os.path.realpath(target)
def inside(name):
    path = os.path.realpath(os.path.join(root, name))
    if os.path.commonpath([root, path]) != root:
        sys.exit('Archive contains an unsafe path')
if zipfile.is_zipfile(source):
    with zipfile.ZipFile(source) as archive:
        for member in archive.infolist():
            inside(member.filename)
            if stat.S_ISLNK(member.external_attr >> 16):
                sys.exit('Unexpected symbolic link in command line tools archive')
        for member in archive.infolist():
            path = archive.extract(member, target)
            mode = (member.external_attr >> 16) & 0o777
            if mode:
                os.chmod(path, mode)
else:
    with tarfile.open(source, 'r:gz') as archive:
        for member in archive.getmembers():
            inside(member.name)
            if member.issym():
                inside(os.path.join(os.path.dirname(member.name), member.linkname))
            elif member.islnk():
                inside(member.linkname)
            elif not member.isfile() and not member.isdir():
                sys.exit('Unexpected special file in JDK archive')
        archive.extractall(target)
PY
}

install_java()
{
    local metadata url checksum candidate
    confirm 'No JDK 17 or 21 was found. Download and install Eclipse Temurin JDK 21 for this user?' || fail 'A full JDK 17 or 21 is required. A JRE is not sufficient'
    prepare_download
    metadata="$TEMP_DIR/jdk.json"
    download "https://api.adoptium.net/v3/assets/latest/21/hotspot?architecture=$ADOPTIUM_ARCH&image_type=jdk&os=$ADOPTIUM_OS&vendor=eclipse" "$metadata"
    python3 - "$metadata" "$ADOPTIUM_OS" "$ADOPTIUM_ARCH" > "$TEMP_DIR/jdk-package.txt" <<'PY'
import json, re, sys, urllib.parse
with open(sys.argv[1], encoding='utf-8') as source:
    assets = json.load(source)
for asset in assets:
    binary = asset.get('binary', {})
    if asset.get('version', {}).get('major') != 21 or binary.get('os') != sys.argv[2] or binary.get('architecture') != sys.argv[3] or binary.get('image_type') != 'jdk':
        continue
    package = binary['package']
    url = urllib.parse.urlsplit(package['link'])
    checksum = package['checksum']
    if url.scheme != 'https' or url.hostname != 'github.com' or not url.path.startswith('/adoptium/temurin21-binaries/releases/download/') or not re.fullmatch('[0-9a-fA-F]{64}', checksum):
        sys.exit('Invalid official JDK download metadata')
    if not url.path.endswith('.tar.gz'):
        sys.exit('Unexpected JDK archive format')
    print(package['link'])
    print(checksum)
    break
else:
    sys.exit('No matching JDK 21 download was returned')
PY
    url="$(sed -n '1p' "$TEMP_DIR/jdk-package.txt")"
    checksum="$(sed -n '2p' "$TEMP_DIR/jdk-package.txt")"
    download "$url" "$TEMP_DIR/jdk.tar.gz"
    verify_archive "$TEMP_DIR/jdk.tar.gz" "$checksum"
    extract_archive "$TEMP_DIR/jdk.tar.gz" "$TEMP_DIR/jdk"
    for candidate in "$TEMP_DIR/jdk"/*; do
        if [ -x "$candidate/bin/javac" ] || [ -x "$candidate/Contents/Home/bin/javac" ]; then
            [ ! -e "$TOOL_ROOT/jdk-21" ] || fail "An unusable JDK already exists at $TOOL_ROOT/jdk-21. Move it aside before retrying"
            mv -- "$candidate" "$TOOL_ROOT/jdk-21"
            java_candidate "$TOOL_ROOT/jdk-21" || java_candidate "$TOOL_ROOT/jdk-21/Contents/Home" || fail 'The downloaded JDK cannot run on this host'
            return
        fi
    done
    fail 'The JDK archive did not contain a Java compiler'
}

find_sdk_manager()
{
    local candidate
    SDK_MANAGER=""
    for candidate in "$SDK_DIR/cmdline-tools/19.0/bin/sdkmanager" "$SDK_DIR/cmdline-tools/latest/bin/sdkmanager" "$SDK_DIR"/cmdline-tools/*/bin/sdkmanager; do
        if [ -x "$candidate" ]; then
            SDK_MANAGER="$candidate"
            return
        fi
    done
}

find_missing_packages()
{
    MISSING_PACKAGES=()
    if [ ! -f "$SDK_DIR/platforms/android-36/android.jar" ]; then
        MISSING_PACKAGES+=("platforms;android-36")
    fi
    if [ ! -x "$SDK_DIR/build-tools/35.0.0/aapt2" ] || [ ! -f "$SDK_DIR/build-tools/35.0.0/lib/apksigner.jar" ]; then
        MISSING_PACKAGES+=("build-tools;35.0.0")
    fi
    if [ ! -f "$SDK_DIR/ndk/27.2.12479018/build/cmake/android.toolchain.cmake" ] || [ ! -x "$SDK_DIR/ndk/27.2.12479018/toolchains/llvm/prebuilt/$NDK_HOST/bin/clang" ]; then
        MISSING_PACKAGES+=("ndk;27.2.12479018")
    fi
    if [ ! -x "$SDK_DIR/cmake/3.22.1/bin/cmake" ] || [ ! -x "$SDK_DIR/cmake/3.22.1/bin/ninja" ]; then
        MISSING_PACKAGES+=("cmake;3.22.1")
    fi
}

host_tools_work()
{
    local executable
    for executable in "$SDK_DIR/cmake/3.22.1/bin/cmake" "$SDK_DIR/cmake/3.22.1/bin/ninja" "$SDK_DIR/ndk/27.2.12479018/toolchains/llvm/prebuilt/$NDK_HOST/bin/clang"; do
        if ! "$executable" --version >/dev/null 2>&1; then
            printf 'Cannot run: %s\n' "$executable" >&2
            return 1
        fi
    done
    if ! "$SDK_DIR/build-tools/35.0.0/aapt2" version >/dev/null 2>&1; then
        printf 'Cannot run: %s\n' "$SDK_DIR/build-tools/35.0.0/aapt2" >&2
        return 1
    fi
}

find_java
find_sdk_manager
find_missing_packages
printf '\nJDK: %s\nAndroid SDK: %s\n' "${JAVA_DIR:-missing}" "$SDK_DIR"
printf 'SDK manager: %s\n' "${SDK_MANAGER:-missing}"
if [ "${#MISSING_PACKAGES[@]}" -gt 0 ]; then
    printf 'Missing SDK package: %s\n' "${MISSING_PACKAGES[@]}"
fi

if "$CHECK_ONLY"; then
    [ -n "$JAVA_DIR" ] && [ -n "$SDK_MANAGER" ] && [ "${#MISSING_PACKAGES[@]}" -eq 0 ] || fail 'Prerequisites are missing. Nothing was downloaded or changed'
    [ -s "$SDK_DIR/licenses/android-sdk-license" ] || fail 'No saved Android SDK license acceptance was found. Run interactively to review licenses'
    host_tools_work || fail 'SDK host tools could not run. Apple Silicon may need Rosetta for the pinned tools'
    printf 'Required local tools and a saved SDK license were found. No build or network check was performed.\n'
    exit 0
fi

if [ -z "$JAVA_DIR" ]; then
    install_java
fi
export JAVA_HOME="$JAVA_DIR"
export PATH="$JAVA_HOME/bin:$PATH"
export ANDROID_HOME="$SDK_DIR"
export ANDROID_SDK_ROOT="$SDK_DIR"

if [ -z "$SDK_MANAGER" ]; then
    printf '\nAndroid SDK license: https://developer.android.com/studio#terms-and-conditions\n'
    confirm "Have you read and accepted the Android SDK license and want to install command line tools 19.0 in $SDK_DIR?" || fail 'Android command line tools are required. No license was accepted by this script'
    prepare_download
    download "https://dl.google.com/android/repository/$SDK_ARCHIVE" "$TEMP_DIR/tools.zip"
    verify_archive "$TEMP_DIR/tools.zip" "$SDK_SHA256"
    extract_archive "$TEMP_DIR/tools.zip" "$TEMP_DIR/tools"
    [ ! -e "$SDK_DIR/cmdline-tools/19.0" ] || fail 'The SDK already contains an incomplete cmdline-tools/19.0 directory. Move it aside before retrying'
    mkdir -p -- "$SDK_DIR/cmdline-tools"
    mv -- "$TEMP_DIR/tools/cmdline-tools" "$SDK_DIR/cmdline-tools/19.0"
    find_sdk_manager
    [ -n "$SDK_MANAGER" ] || fail 'The command line tools archive did not contain sdkmanager'
fi

if [ "${#MISSING_PACKAGES[@]}" -gt 0 ] || [ ! -s "$SDK_DIR/licenses/android-sdk-license" ]; then
    "$INTERACTIVE" || fail 'SDK packages or saved licenses are missing. Run interactively to install and review them'
    confirm 'Review Android SDK licenses now? You can decline each license in the SDK manager' || fail 'SDK license review was declined'
    "$SDK_MANAGER" --sdk_root="$SDK_DIR" --licenses
    if [ "${#MISSING_PACKAGES[@]}" -gt 0 ]; then
        printf 'Packages to install:\n'
        printf '  %s\n' "${MISSING_PACKAGES[@]}"
        confirm 'Download and install these SDK packages? The NDK can require several GB of disk space' || fail 'SDK installation was declined'
        "$SDK_MANAGER" --sdk_root="$SDK_DIR" "${MISSING_PACKAGES[@]}"
    fi
    find_missing_packages
    [ "${#MISSING_PACKAGES[@]}" -eq 0 ] || fail 'Required SDK packages are still missing. Check the SDK manager output and license answers'
fi

if ! host_tools_work; then
    if [ "$HOST_OS:$HOST_ARCH" = Darwin:arm64 ]; then
        printf 'The pinned Android host tools may need Rosetta on Apple Silicon.\n'
        confirm 'Run Apple softwareupdate to install Rosetta? Apple will show its license prompt' || fail 'Install the required host compatibility tools before building'
        /usr/sbin/softwareupdate --install-rosetta
        host_tools_work || fail 'Android SDK host tools still cannot run on this Mac'
    else
        fail 'Android SDK host tools cannot run. Check their system library requirements'
    fi
fi

if [ -z "$ABI" ]; then
    ABI=all
    if "$INTERACTIVE"; then
        printf '\nDevice architectures:\n  1) All supported architectures\n  2) ARM64 (most current phones)\n  3) ARMv7 (older 32-bit phones, not ARMv6)\n  4) x86_64 (emulators and compatible devices)\n'
        read -r -p 'Choose [1]: ' choice
        case "${choice:-1}" in
            1) ABI=all ;;
            2) ABI=arm64-v8a ;;
            3) ABI=armeabi-v7a ;;
            4) ABI=x86_64 ;;
            *) fail 'Choose an architecture from 1 to 4' ;;
        esac
    fi
fi
ABI_LIST="$ABI"
if [ "$ABI" = all ]; then
    ABI_LIST=arm64-v8a,armeabi-v7a,x86_64
fi

if "$INTERACTIVE" && ! "$VARIANT_GIVEN"; then
    printf '\nBuild type:\n  1) Debug APK (recommended, ready to install)\n  2) Release APK (unsigned, requires your signing key)\n'
    read -r -p 'Choose [1]: ' choice
    case "${choice:-1}" in
        1) VARIANT=debug ;;
        2) VARIANT=release ;;
        *) fail 'Choose build type 1 or 2' ;;
    esac
fi

if confirm "Write or replace $TOOL_ROOT/env.sh for future shells? Your shell profile will not be changed"; then
    mkdir -p -- "$TOOL_ROOT"
    {
        printf 'export JAVA_HOME=%q\n' "$JAVA_HOME"
        printf 'export ANDROID_HOME=%q\n' "$ANDROID_HOME"
        printf 'export ANDROID_SDK_ROOT=%q\n' "$ANDROID_SDK_ROOT"
        printf 'export PATH=%q:' "$(dirname -- "$SDK_MANAGER")"
        printf '%s\n' "\"\$JAVA_HOME/bin:\$ANDROID_HOME/platform-tools:\$PATH\""
    } > "$TOOL_ROOT/env.sh"
    printf 'To load it in Bash or Zsh: source %q\n' "$TOOL_ROOT/env.sh"
fi

BUILD_ROOT="$PROJECT_DIR/build-output/$ABI/$VARIANT"
TASK=assembleDebug
APK_NAME=app-debug.apk
if [ "$VARIANT" = release ]; then
    TASK=assembleRelease
    APK_NAME=app-release-unsigned.apk
    printf '\nRelease output is unsigned. Sign it with your own key before installation.\n'
fi
printf '\nBuilding %s for %s\n' "$VARIANT" "$ABI_LIST"
export GRADLE_USER_HOME="${GRADLE_USER_HOME:-$TOOL_ROOT/gradle}"
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
cd -- "$PROJECT_DIR"
bash "$PROJECT_DIR/gradlew" --console=plain --max-workers=2 "-PnativeAbis=$ABI_LIST" "-PnativeBuildRoot=$BUILD_ROOT" -Pandroid.builder.sdkDownload=false "$TASK"
APK_PATH="$BUILD_ROOT/app/outputs/apk/$VARIANT/$APK_NAME"
[ -f "$APK_PATH" ] || fail "Gradle finished but the expected APK was not found: $APK_PATH"
FINAL_APK="$BUILD_ROOT/lemon-native-$ABI-$VARIANT.apk"
cp -- "$APK_PATH" "$FINAL_APK"
printf '\nAPK: %s\n' "$FINAL_APK"
if [ "$VARIANT" = debug ]; then
    printf 'This debug APK can be installed on Android 8.0 or newer with a matching architecture.\n'
fi
