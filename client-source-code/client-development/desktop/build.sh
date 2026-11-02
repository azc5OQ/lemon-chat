#!/bin/sh
# Build and package the native desktop client without an IDE.
set -eu

desktop_source=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
desktop_config=RelWithDebInfo
desktop_tests=OFF

for desktop_argument in "$@"
do
    case "$desktop_argument" in
        Debug|debug) desktop_config=Debug ;;
        Release|release) desktop_config=Release ;;
        RelWithDebInfo|relwithdebinfo) desktop_config=RelWithDebInfo ;;
        --test) desktop_tests=ON ;;
        --help|-h)
            cat <<'HELP'
Usage: sh build.sh [Debug|Release|RelWithDebInfo] [--test]

Default: optimized build with debug symbols, one job per detected logical CPU, no tests.
Output: build-linux/package or build-macos/package, beside this script.
--test runs the small desktop checks. Node.js enables the optional WebSocket check.

Required: a C11 compiler, CMake 3.22+, Ninja or make, and libcurl development
files with ws/wss support (7.86+). Linux also needs pkg-config and X11/EGL/GLES
development files; macOS needs Xcode command-line tools.

Optional environment variables:
  LEMON_DESKTOP_BUILD_DIR     Build directory, relative to this script or absolute
  CMAKE_BUILD_PARALLEL_LEVEL  Overrides the detected logical CPU count
  CMAKE_GENERATOR            Generator for a new build directory
  CMAKE_PREFIX_PATH          Additional dependency prefixes, e.g. Homebrew curl
  CC                         C compiler for a new build directory
HELP
            exit 0
            ;;
        *) printf 'ERROR: Unknown option: %s\n' "$desktop_argument" >&2; exit 2 ;;
    esac
done

if [ "$desktop_tests" = ON ] && [ ! -f "$desktop_source/checks/attachments_check.c" ]
then
    printf '%s\n' 'ERROR: Local checks are absent (ignored by Git). Build without --test or restore your local checks.' >&2
    exit 1
fi

case "$(uname -s)" in
    Linux) desktop_default_build=build-linux ;;
    Darwin) desktop_default_build=build-macos ;;
    *) printf '%s\n' 'Use build.bat on Windows; build.sh supports Linux and macOS.' >&2; exit 1 ;;
esac

if [ -z "${CMAKE_BUILD_PARALLEL_LEVEL:-}" ]
then
    desktop_jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')
    if command -v nproc >/dev/null 2>&1
    then
        desktop_jobs=$(nproc)
    elif command -v sysctl >/dev/null 2>&1
    then
        desktop_jobs=$(sysctl -n hw.logicalcpu 2>/dev/null || printf '%s' "$desktop_jobs")
    fi
    case "$desktop_jobs" in
        ''|0|*[!0-9]*) desktop_jobs=1 ;;
    esac
    CMAKE_BUILD_PARALLEL_LEVEL=$desktop_jobs
fi

if ! command -v cmake >/dev/null 2>&1
then
    printf '%s\n' 'ERROR: Install CMake 3.22 or newer and add it to PATH.' >&2
    exit 1
fi

cd "$desktop_source"
desktop_build=${LEMON_DESKTOP_BUILD_DIR:-$desktop_default_build}
case "$desktop_build" in
    /*) ;;
    *) desktop_build="$desktop_source/$desktop_build" ;;
esac

# An existing build keeps its generator and dependency paths.
set --
if [ ! -f "$desktop_build/CMakeCache.txt" ] && [ -z "${CMAKE_GENERATOR:-}" ]
then
    if command -v ninja >/dev/null 2>&1
    then
        set -- -G Ninja
    fi
fi

printf 'Building with %s jobs.\n' "$CMAKE_BUILD_PARALLEL_LEVEL"
cmake -S "$desktop_source" -B "$desktop_build" "$@" \
    -DCMAKE_BUILD_TYPE="$desktop_config" -DBUILD_TESTING="$desktop_tests" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build "$desktop_build" --config "$desktop_config" \
    --parallel "$CMAKE_BUILD_PARALLEL_LEVEL"
if [ "$desktop_tests" = ON ]
then
    ctest --test-dir "$desktop_build" -C "$desktop_config" --output-on-failure --parallel 1
fi
cmake --install "$desktop_build" --config "$desktop_config" --prefix "$desktop_build/package"
printf '\nBuilt: %s/package/lemon-chat-desktop\n' "$desktop_build"
printf '%s\n' 'Keep the shared libraries, assets and fonts beside the executable.'
