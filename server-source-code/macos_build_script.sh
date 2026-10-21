#!/bin/bash
#
# Build the lemon-chat server + bundled stunnel into ../buildresult/ on macOS.
#   ./macos_build_script.sh
#
#
# Prerequisites
#   xcode-select --install              # clang, make, ld, install_name_tool, otool (Xcode command line tools)
#   brew install cmake                  # if not already installed
#   brew install ninja                  # if not already installed
#   brew install pkg-config             # if not already installed
#
set -o pipefail
LRED='\033[01;31m'
GREEN='\033[0;32m'
LCYAN='\033[1;36m'
LBLUE='\033[1;34m'
NC='\033[0m'
message()
{
    echo
    echo -e " $LBLUE>>> $LCYAN $* $NC"
}

warning()
{
    echo
    echo -e " $LCYAN>>> $LRED $* $NC"
}

ROOT_DIRECTORY="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
THIRD_PARTY_DIRECTORY="$ROOT_DIRECTORY/third-party"
LINK_DIR="$ROOT_DIRECTORY/main/linkage-files/macos"

# =============================================================================
# SETUP | Compiler and build options
# =============================================================================

CMAKE_C_COMPILER="clang"
CMAKE_CXX_COMPILER="clang++"
CMAKE_LINKER="ld"
CMAKE_C_FLAGS="-Wno-expansion-to-defined -Wno-shadow -Wno-declaration-after-statement -DUSE_LTM -DLTM_DESC"
CMAKE_C_FLAGS_RELEASE="-DDEBUG -Wno-expansion-to-defined -Wno-shadow -Wno-declaration-after-statement"
BUILD_CONFIG="Release"

# parallel build jobs (-j); override with: JOBS=8 ./macos_build_script.sh
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

if ! xcode-select -p >/dev/null 2>&1
then
    warning "Xcode command line tools not found. Run: xcode-select --install"
fi

# the three brew tools this build relies on; warn (with the exact install line) if any is missing
for required_tool in cmake ninja pkg-config
do
    if ! command -v "$required_tool" >/dev/null 2>&1
    then
        warning "$required_tool not found. Install the build tools with:  brew install cmake ninja pkg-config"
        break
    fi
done

# =============================================================================
# OUTPUT | Recreate the runtime output directory
# =============================================================================

# This existing step removes buildresult, including saved runtime files.
rm -f -v -r "$ROOT_DIRECTORY/../buildresult/"
mkdir -p "$ROOT_DIRECTORY/../buildresult/"
mkdir -p "$LINK_DIR"

# =============================================================================
# CLEAN | Optional removal of dependency build artifacts
# =============================================================================

read -p "delete build files? " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]
then
    rm -fv "$ROOT_DIRECTORY"/*.o
    rm -rfv "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/CMakeFiles" "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/library/CMakeFiles"
    rm -fv  "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/CMakeCache.txt"
    rm -rfv "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/library/libmbedtls.a" "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/library/libmbedx509.a" "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/library/libmbedcrypto.a"
    rm -rfv "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/CMakeFiles"
    rm -fv  "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/CMakeCache.txt"
    rm -fv  "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/"libdatachannel*.dylib
    rm -rfv "$THIRD_PARTY_DIRECTORY/libtom/libtommath/CMakeFiles" "$THIRD_PARTY_DIRECTORY/libtom/libtommath/CMakeCache.txt" "$THIRD_PARTY_DIRECTORY/libtom/libtommath/libtommath.a"
    rm -fv  "$THIRD_PARTY_DIRECTORY/libtom/libtomcrypt/libtomcrypt.a"
    rm -rfv "$THIRD_PARTY_DIRECTORY/theldus-websocket/CMakeFiles" "$THIRD_PARTY_DIRECTORY/theldus-websocket/CMakeCache.txt"
    rm -rfv "$THIRD_PARTY_DIRECTORY/libviolet-0.5.4/CMakeFiles" "$THIRD_PARTY_DIRECTORY/libviolet-0.5.4/CMakeCache.txt" "$THIRD_PARTY_DIRECTORY/libviolet-0.5.4/libviolet.a"
    rm -rfv "$THIRD_PARTY_DIRECTORY/libopus-1.5.2/CMakeFiles" "$THIRD_PARTY_DIRECTORY/libopus-1.5.2/CMakeCache.txt"
    rm -rfv "$LINK_DIR"/*
    rm -rfv "$ROOT_DIRECTORY/build/macos"
    rm -rfv "$ROOT_DIRECTORY/main/CMakeFiles" "$ROOT_DIRECTORY/main/CMakeCache.txt" "$ROOT_DIRECTORY/main/cmake_install.cmake" "$ROOT_DIRECTORY/main/build.ninja" "$ROOT_DIRECTORY/main/.ninja_deps" "$ROOT_DIRECTORY/main/.ninja_log"
    ( cd "$THIRD_PARTY_DIRECTORY/stunnel/stunnel-5.75" && make distclean ) >/dev/null 2>&1
    rm -rfv "$THIRD_PARTY_DIRECTORY/stunnel/openssl"
    rm -fv  "$THIRD_PARTY_DIRECTORY/stunnel/stunnel-5.75/src/stunnel"
fi

# Dependency cleanup is complete; continue with the selected build settings.
echo
echo -e "${LCYAN}  parallel build jobs (-j): ${GREEN}$JOBS${NC}   (override: JOBS=N ./macos_build_script.sh)${NC}"
echo

# =============================================================================
# HELPERS | Shared CMake configuration
# =============================================================================

# Pass each additional CMake option as its own argument.
cmake_lib()
{
    cmake -G Ninja . \
        -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" \
        -DCMAKE_LINKER="$CMAKE_LINKER" \
        -DCMAKE_C_COMPILER="$CMAKE_C_COMPILER" \
        -DCMAKE_CXX_COMPILER="$CMAKE_CXX_COMPILER" \
        -DCMAKE_C_FLAGS_RELEASE="$CMAKE_C_FLAGS_RELEASE" \
        "$@"
}

# =============================================================================
# DEPENDENCY 1/8 | Mbed TLS: TLS and RSA libraries
# =============================================================================

message "building mbedtls"
cd "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6"
cmake_lib -DCMAKE_C_FLAGS="-fPIC" -DCMAKE_CXX_FLAGS="-fPIC"
cmake --build . -j"$JOBS" --target mbedtls

# datachannel consumes mbedtls headers+libs from its deps/ tree
mkdir -p "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/deps/mbedtls/lib"
rm -rf "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/deps/mbedtls/include"
cp -r "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/include" "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/deps/mbedtls/"
for L in libmbedtls libmbedx509 libmbedcrypto
do
    cp -v "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/library/$L.a" "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/deps/mbedtls/lib/$L.a"
    cp -v "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/library/$L.a" "$LINK_DIR/$L.a"
done

# =============================================================================
# DEPENDENCY 2/8 | libdatachannel: WebRTC transport
# =============================================================================

message "building libdatachannel.dylib and its deps (libjuice, libsrtp, usrsctp)"
cd "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2"
cmake_lib
cmake --build . -j"$JOBS" --target datachannel

# cmake produces libdatachannel.<version>.dylib plus a libdatachannel.dylib symlink; cp -L
# copies the real file. then pin its install_name so the server records @rpath/libdatachannel.dylib.
cp -L "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/libdatachannel.dylib" "$LINK_DIR/libdatachannel.dylib"
install_name_tool -id "@rpath/libdatachannel.dylib" "$LINK_DIR/libdatachannel.dylib"
cp -v "$LINK_DIR/libdatachannel.dylib" "$ROOT_DIRECTORY/../buildresult/"

# =============================================================================
# DEPENDENCY 3/8 | LibTomMath: integer arithmetic
# =============================================================================

message "building libtommath.a"
cd "$THIRD_PARTY_DIRECTORY/libtom/libtommath"
cmake_lib
cmake --build . -j"$JOBS" --target libtommath
cp -v "$THIRD_PARTY_DIRECTORY/libtom/libtommath/libtommath.a" "$LINK_DIR/libtommath.a"

# =============================================================================
# DEPENDENCY 4/8 | LibTomCrypt: cryptographic primitives
# =============================================================================

message "building libtomcrypt.a"
cd "$THIRD_PARTY_DIRECTORY/libtom/libtomcrypt"
make -f makefile.unix -j"$JOBS" CC="$CMAKE_C_COMPILER"
cp -v "$THIRD_PARTY_DIRECTORY/libtom/libtomcrypt/libtomcrypt.a" "$LINK_DIR/libtomcrypt.a"

# =============================================================================
# DEPENDENCY 5/8 | WebSocket transport
# =============================================================================

message "building theldus-websocket (libws.a)"
cd "$THIRD_PARTY_DIRECTORY/theldus-websocket"
cmake_lib
cmake --build . -j"$JOBS" --target ws
cp -v "$THIRD_PARTY_DIRECTORY/theldus-websocket/libws.a" "$LINK_DIR/libws.a"

# =============================================================================
# DEPENDENCY 6/8 | Violet
# =============================================================================

message "building libviolet.a"
cd "$THIRD_PARTY_DIRECTORY/libviolet-0.5.4"
cmake_lib
cmake --build . -j"$JOBS" --target violet
cp -v "$THIRD_PARTY_DIRECTORY/libviolet-0.5.4/libviolet.a" "$LINK_DIR/libviolet.a"

# =============================================================================
# DEPENDENCY 7/8 | MaxMind DB: country lookup
# =============================================================================

message "building libmaxminddb"
cd "$THIRD_PARTY_DIRECTORY/libmaxminddb-1.12.2"
make clean ACLOCAL=: AUTOCONF=: AUTOHEADER=: AUTOMAKE=: >/dev/null 2>&1
chmod +x configure
./configure CC="$CMAKE_C_COMPILER"

# ':' no-ops the autotools regen so fresh-checkout mtimes don't trigger aclocal/automake
make ACLOCAL=: AUTOCONF=: AUTOHEADER=: AUTOMAKE=:
cp -v "$THIRD_PARTY_DIRECTORY/libmaxminddb-1.12.2/src/.libs/libmaxminddb.a" "$LINK_DIR/"

# =============================================================================
# DEPENDENCY 8/8 | Opus: audio codec
# =============================================================================

message "building libopus.a"
cd "$THIRD_PARTY_DIRECTORY/libopus-1.5.2"
cmake_lib
cmake --build . -j"$JOBS" --target opus
cp -v "$THIRD_PARTY_DIRECTORY/libopus-1.5.2/libopus.a" "$LINK_DIR/libopus.a"

# =============================================================================
# CLIENT | Generate the browser page
# =============================================================================

message "building the browser client (client.html)"
if command -v python3 >/dev/null 2>&1
then
    python3 "$ROOT_DIRECTORY/../client-source-code/client-development/browser/build.py"
else
    warning "python3 not found - skipping client build; served client.html may be stale/missing"
fi

# =============================================================================
# SERVER | Build chat-server
# =============================================================================

message "building chat server executable"
cd "$ROOT_DIRECTORY/main"
# Keep compiler output and CMake's cache outside the source directory.
cmake_lib -B "$ROOT_DIRECTORY/build/macos"
cmake --build "$ROOT_DIRECTORY/build/macos" -j"$JOBS"
cp -v "$ROOT_DIRECTORY/build/macos/chat-server" "$ROOT_DIRECTORY/../buildresult/chat-server.bin"

# =============================================================================
# PACKAGE | Runtime files and macOS launcher
# =============================================================================

# the http server serves client.html out of buildresult
if [ -f "$ROOT_DIRECTORY/../client-source-code/client.html" ]
then
    cp -v "$ROOT_DIRECTORY/../client-source-code/client.html" "$ROOT_DIRECTORY/../buildresult/"
else
    warning "client.html not found - the bundled http server will have no page to serve"
fi

cp -v "$THIRD_PARTY_DIRECTORY/libmaxminddb-1.12.2/dbip-country-lite-2025-06.mmdb" "$ROOT_DIRECTORY/../buildresult/" 2>/dev/null || warning "geoip mmdb not found (country flags will be unavailable)"

# macOS start script: DYLD_LIBRARY_PATH (not LD_LIBRARY_PATH); the @loader_path rpath already
# resolves libdatachannel.dylib, this is a belt-and-braces fallback. keeps the restart-on-crash loop.
cat > "$ROOT_DIRECTORY/../buildresult/start_server.sh" <<'STARTSCRIPT'
#!/bin/bash
cd "$(dirname "$0")"
chmod +x chat-server.bin
ulimit -n 65535 2>/dev/null || ulimit -n 10240 2>/dev/null || true
export DYLD_LIBRARY_PATH="$DYLD_LIBRARY_PATH:$PWD"
if [ -f server_settings.json ] && grep -Eq '"is_restart_on_crash_enabled"[[:space:]]*:[[:space:]]*true' server_settings.json
then
    trap 'echo; echo "stopping (signal received)."; exit 0' INT TERM
    while true
    do
        ./chat-server.bin
        EXIT_CODE=$?
        if [ "$EXIT_CODE" -eq 0 ]
        then
            echo "server exited cleanly; not restarting."
            break
        fi
        echo "$(date '+%Y-%m-%d %H:%M:%S') server crashed (exit code $EXIT_CODE), restarting" >> crashes.txt
        echo "server crashed (exit code $EXIT_CODE) - restarting in 3s (Ctrl+C to stop)..."
        sleep 3
    done
else
    ./chat-server.bin
fi
STARTSCRIPT
chmod +x "$ROOT_DIRECTORY/../buildresult/start_server.sh"

# =============================================================================
# TLS FRONT-END | Build and collect stunnel
# =============================================================================

# the vendored OpenSSL builds via ./config, which auto-detects macOS, so this bundler is reused as-is.
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

message "build finished. Run ./start_server.sh in the buildresult directory."
