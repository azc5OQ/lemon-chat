#!/bin/sh
# Build only the native Android server. Gradle uses the same CMake project when
# building the APK, so you do not need to run this script for Android Studio.
# Usage: NDK=/path/to/android-ndk sh build-native.sh
set -eu

NDK="${NDK:?Set NDK to the Android NDK root directory}"
ABI="${ABI:-arm64-v8a}"
API="${API:-26}"
CMAKE="${CMAKE:-cmake}"
NINJA="${NINJA:-ninja}"
JOBS="${JOBS:-4}"
HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
OUT="${OUT:-$HERE/out/$ABI}"

case "$ABI" in
    arm64-v8a|armeabi-v7a|x86_64) ;;
    *) printf '%s\n' 'ABI must be arm64-v8a, armeabi-v7a or x86_64.' >&2; exit 1 ;;
esac
case "$API" in
    ''|*[!0-9]*) printf '%s\n' 'API must be an integer of 26 or greater.' >&2; exit 1 ;;
esac
if [ "$API" -lt 26 ]; then
    printf '%s\n' 'The server app requires Android API 26 or greater.' >&2
    exit 1
fi
case "$JOBS" in
    ''|0|*[!0-9]*) printf '%s\n' 'JOBS must be a positive integer.' >&2; exit 1 ;;
esac

# Resolve output first so the package path remains absolute even with OUT=build.
mkdir -p "$OUT"
OUT="$(CDPATH= cd -- "$OUT" && pwd)"
"$CMAKE" -S "$HERE" -B "$OUT" -G Ninja \
    "-DCMAKE_MAKE_PROGRAM=$NINJA" \
    "-DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake" \
    "-DANDROID_ABI=$ABI" "-DANDROID_PLATFORM=android-$API" \
    -DANDROID_STL=c++_static -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
    -DCMAKE_BUILD_TYPE=Release "-DSERVER_OUTPUT_DIR=$OUT/package"
"$CMAKE" --build "$OUT" --target chat-server --parallel "$JOBS"
printf 'Native executable: %s\n' "$OUT/package/libchat-server.so"
