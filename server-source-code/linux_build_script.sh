#!/bin/bash
#
# Build the lemon-chat server + bundled stunnel into ../buildresult/.
#   ./linux_build_script.sh          normal Linux build
#   ./linux_build_script.sh --wsl    building inside WSL on /mnt/c with a modern
#                                    toolchain (GCC 15 / CMake 4)
#
LRED='\033[01;31m'
GREEN='\033[0;32m'
LCYAN='\033[1;36m'
LBLUE='\033[1;34m'
NC='\033[0m' # No Color
message ()
{
    echo
    echo -e " $LBLUE>>> $LCYAN $@ $NC"
}

warning ()
{
    echo
    echo -e " $LCYAN>>> $LRED $@ $NC"
}

ROOT_DIRECTORY="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
THIRD_PARTY_DIRECTORY="$ROOT_DIRECTORY/third-party"

# =============================================================================
# SETUP | Compiler and build options
# =============================================================================

CMAKE_CXX_COMPILER="g++"
CMAKE_C_COMPILER="gcc"
CMAKE_LINKER="ld"
CMAKE_C_FLAGS="-Wno-expansion-to-defined -Wno-shadow -Wno-declaration-after-statement -DUSE_LTM -DLTM_DESC"
CMAKE_C_FLAGS_RELEASE="-DDEBUG -Wno-expansion-to-defined -Wno-shadow -Wno-declaration-after-statement"
BUILD_CONFIG="Release"

#BUILD_CONFIG="Debug"
# optional WSL mode: ./linux_build_script.sh --wsl  (modern toolchain on /mnt/c: -std=gnu17,
# a CMake policy floor, fewer parallel jobs, libmaxminddb autotools regen, /mnt metadata check)
WSL_MODE=0
case "${1:-}" in
    --wsl|-w)
        WSL_MODE=1
    ;;
esac

# parallel build jobs (-j); override with: JOBS=8 ./linux_build_script.sh
JOBS="${JOBS:-32}"

message "Debian/Ubuntu: install the build packages with: apt install build-essential cmake ninja-build pkg-config perl curl tar autoconf automake libtool"

# =============================================================================
# OUTPUT | Recreate the runtime output directory
# =============================================================================

# This existing step removes buildresult, including saved runtime files.
rm -f -v -r "$ROOT_DIRECTORY/../buildresult/"
mkdir -p "$ROOT_DIRECTORY/../buildresult/"

# =============================================================================
# CLEAN | Optional removal of dependency build artifacts
# =============================================================================

read -p "delete build files? " -n 1 -r
echo    # (optional) move to a new line
if [[ $REPLY =~ ^[Yy]$ ]]
then
    rm -fv "$ROOT_DIRECTORY"/*.o
    rm -rfv "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/CMakeFiles"
    rm -rfv "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/library/CMakeFiles"
    rm -fv "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/CMakeCache.txt"
    rm -rfv "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/output"
    rm -rfv "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/library/libmbedtls.a"
    rm -rfv "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/library/libmbedcrypto.a"
    rm -rfv "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/library/libmbedx509.a"
    rm -rfv "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/CMakeFiles"
    rm -fv "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/CMakeCache.txt"
    rm -rfv "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/libdatachannel-static.a"
    rm -rfv "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/libdatachannel.dll"
    rm -rfv "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/libdatachannel.dll.a"
    rm -rfv "$THIRD_PARTY_DIRECTORY/libtom/libtommath/cmake-build-release"
    rm -rfv "$THIRD_PARTY_DIRECTORY/libtom/libtommath/CMakeFiles"
    rm -fv "$THIRD_PARTY_DIRECTORY/libtom/libtommath/CMakeCache.txt"
    rm -fv "$THIRD_PARTY_DIRECTORY/libtom/libtommath/libtommath.a"
    rm -fv "$THIRD_PARTY_DIRECTORY/libtom/libtomcrypt/libtomcrypt.a"
    rm -rfv "$THIRD_PARTY_DIRECTORY/theldus-websocket/CMakeFiles"
    rm -fv "$THIRD_PARTY_DIRECTORY/theldus-websocket/CMakeCache.txt"
    rm -rfv "$THIRD_PARTY_DIRECTORY/libviolet-0.5.4/CMakeFiles"
    rm -fv "$THIRD_PARTY_DIRECTORY/libviolet-0.5.4/CMakeCache.txt"
    rm -fv "$THIRD_PARTY_DIRECTORY/libviolet-0.5.4/libviolet.a"
    rm -rfv "$ROOT_DIRECTORY/main/linkage-files/windows/"*
    rm -rfv "$ROOT_DIRECTORY/main/linkage-files/linux/"*
    rm -rfv "$ROOT_DIRECTORY/main/CMakeFiles"
    rm -fv "$ROOT_DIRECTORY/main/CMakeCache.txt"
    rm -fv "$ROOT_DIRECTORY/main/cmake_install.cmake"
    rm -fv "$ROOT_DIRECTORY/main/build.ninja"
    rm -fv "$ROOT_DIRECTORY/main/.ninja_deps"
    rm -fv "$ROOT_DIRECTORY/main/.ninja_log"
    rm -rfv "$THIRD_PARTY_DIRECTORY/libopus-1.5.2/CMakeFiles"
    rm -fv "$THIRD_PARTY_DIRECTORY/libopus-1.5.2/CMakeCache.txt"
    rm -fv "$ROOT_DIRECTORY/../buildresult/"*
    # stunnel + its OpenSSL dependency (binaries + build trees)
    ( cd "$THIRD_PARTY_DIRECTORY/stunnel/stunnel-5.75" && make distclean ) >/dev/null 2>&1
    rm -rfv "$THIRD_PARTY_DIRECTORY/stunnel/openssl"
    rm -rfv "$THIRD_PARTY_DIRECTORY/stunnel/win-build"
    rm -fv "$THIRD_PARTY_DIRECTORY/stunnel/stunnel-5.75/src/stunnel"
    rm -fv "$THIRD_PARTY_DIRECTORY/stunnel/stunnel-5.75/bin/MGW32/stunnel.exe"
    rm -fv "$ROOT_DIRECTORY/../buildresult/stunnel"
fi

# Dependency cleanup is complete; continue with the selected build settings.

# =============================================================================
# SETUP | WSL compatibility adjustments
# =============================================================================

WSL_CMAKE=()
if [ "$WSL_MODE" = "1" ]
then
    JOBS="${NJOBS:-4}"
    WSL_CMAKE=(-DCMAKE_POLICY_VERSION_MINIMUM=3.5)            # CMake 4 dropped <3.5 compat
    CMAKE_C_FLAGS_RELEASE="$CMAKE_C_FLAGS_RELEASE -std=gnu17"  # GCC 15 defaults to C23
    case "$ROOT_DIRECTORY" in
        /mnt/*)
            mp="$(printf '%s' "$ROOT_DIRECTORY" | cut -d/ -f1-3)"
            if ! findmnt -no OPTIONS "$mp" 2>/dev/null | grep -q metadata
            then
                echo "ERROR: $mp is not mounted with 'metadata' - CMake will fail."
                echo "Add [automount] options=\"metadata\" to /etc/wsl.conf, then run 'wsl --shutdown'."
                exit 1
            fi
        ;;
    esac
    message "WSL mode: -j$JOBS, gnu17, CMake policy >=3.5"
fi

echo
echo -e "${LCYAN} ====================================================================${NC}"
echo -e "${LCYAN}   parallel build jobs (-j): ${GREEN}$JOBS${NC}"
echo -e "${LCYAN}   to change, edit JOBS (or NJOBS for --wsl) near the top of this${NC}"
echo -e "${LCYAN}   script, or set it as an env var: ${GREEN}JOBS=N ./linux_build_script.sh${NC}"
echo -e "${LCYAN} ====================================================================${NC}"
echo

# =============================================================================
# HELPERS | Shared CMake configuration
# =============================================================================

# Additional options remain separate arguments, so paths and flags keep their spaces.
cmake_lib()
{
    cmake -G Ninja . \
        -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" \
        -DCMAKE_LINKER="$CMAKE_LINKER" \
        -DCMAKE_C_COMPILER="$CMAKE_C_COMPILER" \
        -DCMAKE_CXX_COMPILER="$CMAKE_CXX_COMPILER" \
        -DCMAKE_C_FLAGS_RELEASE="$CMAKE_C_FLAGS_RELEASE" \
        "${WSL_CMAKE[@]}" \
        "$@"
}

# =============================================================================
# DEPENDENCY 1/8 | Mbed TLS: TLS and RSA libraries
# =============================================================================

message "building mbedtls (libmbedtls.a, libmbedx509.a, libmbedcrypto.a)"
cd "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6"
cmake_lib -DCMAKE_C_FLAGS="-fPIC" -DCMAKE_CXX_FLAGS="-fPIC"
cmake --build . -j"$JOBS" --target mbedtls
message "mbedtls build finished, moving some mbedtls files to their destination"

# Stage Mbed TLS headers and libraries for libdatachannel.
# The lib directory may not exist in a fresh checkout.
mkdir -p "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/deps/mbedtls/lib"
rm -rf "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/deps/mbedtls/include"
cp -r "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/include" "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/deps/mbedtls/"
cp -v \
    "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/library/libmbedtls.a" \
    "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/deps/mbedtls/lib/libmbedtls.a"
cp -v \
    "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/library/libmbedx509.a" \
    "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/deps/mbedtls/lib/libmbedx509.a"
cp -v \
    "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/library/libmbedcrypto.a" \
    "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/deps/mbedtls/lib/libmbedcrypto.a"

# The server also links Mbed TLS directly for RSA and media encryption.
mkdir -p "$ROOT_DIRECTORY/main/linkage-files/linux"
cp -v "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/library/libmbedtls.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libmbedtls.a"
cp -v "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/library/libmbedx509.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libmbedx509.a"
cp -v "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/library/libmbedcrypto.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libmbedcrypto.a"

# =============================================================================
# DEPENDENCY 2/8 | libdatachannel: WebRTC transport
# =============================================================================

message "building libdatachannel.so and its dependencies (libjuice, libsrtp, usrsctp)"
cd "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2"
cmake_lib
cmake --build . -j"$JOBS" --target datachannel
cp -v "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/libdatachannel.so" "$ROOT_DIRECTORY/main/linkage-files/linux/libdatachannel.so"
cp -v "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/libdatachannel.so" "$ROOT_DIRECTORY/../buildresult/"

# =============================================================================
# DEPENDENCY 3/8 | LibTomMath: integer arithmetic
# =============================================================================

message "building libtommath.a"
cd "$THIRD_PARTY_DIRECTORY/libtom/libtommath"
cmake_lib
cmake --build . -j"$JOBS"  --target libtommath
cp -v "$THIRD_PARTY_DIRECTORY/libtom/libtommath/libtommath.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libtommath.a"

# =============================================================================
# DEPENDENCY 4/8 | LibTomCrypt: cryptographic primitives
# =============================================================================

message "building libtomcrypt.a"
cd "$THIRD_PARTY_DIRECTORY/libtom/libtomcrypt"
make -f makefile.unix -j"$JOBS"
cp -v "$THIRD_PARTY_DIRECTORY/libtom/libtomcrypt/libtomcrypt.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libtomcrypt.a"

# =============================================================================
# DEPENDENCY 5/8 | WebSocket transport
# =============================================================================

message "building theldus-websocket (libws.a)"
cd "$THIRD_PARTY_DIRECTORY/theldus-websocket"
cmake_lib
cmake --build . -j"$JOBS"  --target ws
cp -r -v "$THIRD_PARTY_DIRECTORY/theldus-websocket/libws.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libws.a"

# =============================================================================
# DEPENDENCY 6/8 | Violet
# =============================================================================

message "building libviolet.a"
cd "$THIRD_PARTY_DIRECTORY/libviolet-0.5.4"
cmake_lib
cmake --build . -j"$JOBS"  --target violet
cp -v "$THIRD_PARTY_DIRECTORY/libviolet-0.5.4/libviolet.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libviolet.a"

# =============================================================================
# DEPENDENCY 7/8 | MaxMind DB: country lookup
# =============================================================================

message "building libmaxminddb-1.12.2"
cd "$THIRD_PARTY_DIRECTORY/libmaxminddb-1.12.2"
if [ "$WSL_MODE" = "1" ]
then
    autoreconf -fi          # regen so stale /mnt/c timestamps don't trigger maintainer-mode
    ./configure
    make -C src -j"$JOBS"   # src/ only; the vendored tree lacks the test suite
else
    make clean ACLOCAL=: AUTOCONF=: AUTOHEADER=: AUTOMAKE=:
    chmod +x configure
    ./configure
    # Keep the committed configure files. Fresh checkout timestamps can otherwise
    # trigger unavailable autotools versions; the colon makes regeneration a no-op.
    make ACLOCAL=: AUTOCONF=: AUTOHEADER=: AUTOMAKE=:
fi

cp -v "$THIRD_PARTY_DIRECTORY/libmaxminddb-1.12.2/src/.libs/libmaxminddb.a" "$ROOT_DIRECTORY/main/linkage-files/linux/"

# =============================================================================
# DEPENDENCY 8/8 | Opus: audio codec
# =============================================================================

message "building libopus-1.5.2"
cd "$THIRD_PARTY_DIRECTORY/libopus-1.5.2"
cmake_lib
cmake --build . -j"$JOBS"  --target opus
cp -r -v "$THIRD_PARTY_DIRECTORY/libopus-1.5.2/libopus.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libopus.a"

# =============================================================================
# CLIENT | Generate the browser page
# =============================================================================

# the bundled http server serves client.html out of the buildresult directory, so build it here
# (build.py is fast, about a second) and copy it in next to the server binary below.
message "building the browser client (client.html)"
if type python3 >/dev/null 2>&1
then
    python3 "$ROOT_DIRECTORY/../client-source-code/client-development/browser/build.py"
else
    warning "python3 not found - skipping client build, the served client.html may be stale or missing"
fi

# =============================================================================
# SERVER | Build chat-server
# =============================================================================

message "building chat server executable"
cd "$ROOT_DIRECTORY/main"
cmake_lib
cmake --build . -j"$JOBS"

# =============================================================================
# PACKAGE | Server, shared library, launcher, and browser page
# =============================================================================

cp -v "$ROOT_DIRECTORY/main/chat-server" "$ROOT_DIRECTORY/../buildresult/chat-server.bin"
cp -v "$ROOT_DIRECTORY/main/linkage-files/linux/libdatachannel.so" "$ROOT_DIRECTORY/../buildresult/libdatachannel.so"

# The loader looks for the versioned SONAME; its target must stay relative.
ln -s libdatachannel.so "$ROOT_DIRECTORY/../buildresult/libdatachannel.so.0.24"

# Include the launcher that sets the shared-library path and handles restarts.
cp -v "$ROOT_DIRECTORY/main/unix_start_template" "$ROOT_DIRECTORY/../buildresult/start_server.sh"
chmod +x "$ROOT_DIRECTORY/../buildresult/start_server.sh"
cp -v "$THIRD_PARTY_DIRECTORY/libmaxminddb-1.12.2/dbip-country-lite-2025-06.mmdb" "$ROOT_DIRECTORY/../buildresult/"

# the page the bundled http server serves (built above)
if [ -f "$ROOT_DIRECTORY/../client-source-code/client.html" ]
then
    cp -v "$ROOT_DIRECTORY/../client-source-code/client.html" "$ROOT_DIRECTORY/../buildresult/"
else
    warning "client.html not found - the bundled http server will have no page to serve"
fi

# =============================================================================
# TLS FRONT-END | Build and collect stunnel
# =============================================================================

# main.c can launch this to serve wss alongside ws, so the stunnel binary must
# sit next to chat-server.bin in buildresult. buildresult is wiped at the top of
# this script, so stunnel has to be (re)built and copied here on every build.
message "building + collecting bundled stunnel (wss front-end)"
STUNNEL_DIRECTORY="$THIRD_PARTY_DIRECTORY/stunnel"
if [ -f "$STUNNEL_DIRECTORY/_bundle_and_build.sh" ]
then
    bash "$STUNNEL_DIRECTORY/_bundle_and_build.sh"
    cp -v "$STUNNEL_DIRECTORY/stunnel-5.75/src/stunnel" "$ROOT_DIRECTORY/../buildresult/stunnel"
    chmod +x "$ROOT_DIRECTORY/../buildresult/stunnel"
else
    warning "stunnel/_bundle_and_build.sh not found - wss auto-launch will be unavailable"
fi

message "build finished. Try running start_server.sh in buildresult directory"
