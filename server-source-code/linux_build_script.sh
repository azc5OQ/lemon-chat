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
LPURPLE='\033[0;35m'
DGRAY='\033[1;30m'
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

command ()
{
  echo -e "$LCYAN$@$NC"
  $@
  if [ $? -ne 0 ]; then
    echo -e "${LRED}Failed${NC}"
    exit 1
  fi
}

ROOT_DIRECTORY="$PWD"
THIRD_PARTY_DIRECTORY="$PWD/third-party"

message "you need these tools for building : gcc, g++, cmake, ninja, make"
message "(the bundled stunnel/wss build also needs perl, curl, tar; --wsl also needs autoconf, automake, libtool)"
 

rm -f -v -r $ROOT_DIRECTORY/../buildresult/

mkdir $ROOT_DIRECTORY/../buildresult/


read -p "delete build files? " -n 1 -r
echo    # (optional) move to a new line
if [[ $REPLY =~ ^[Yy]$ ]]
then
    rm -fv "$ROOT_DIRECTORY/*.o"
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
    rm -rfv "$ROOT_DIRECTORY/main/linkage-files/windows/*"
    rm -rfv "$ROOT_DIRECTORY/main/linkage-files/linux/*"
    rm -rfv "$ROOT_DIRECTORY/main/CMakeFiles"
    rm -fv "$ROOT_DIRECTORY/main/CMakeCache.txt"
    rm -fv "$ROOT_DIRECTORY/main/cmake_install.cmake"
    rm -fv "$ROOT_DIRECTORY/main/build.ninja"
    rm -fv "$ROOT_DIRECTORY/main/.ninja_deps"
    rm -fv "$ROOT_DIRECTORY/main/.ninja_log"
    rm -rfv "$THIRD_PARTY_DIRECTORY/libopus-1.5.2/CMakeFiles"
    rm -fv "$THIRD_PARTY_DIRECTORY/libopus-1.5.2/CMakeCache.txt"
    rm -fv "$ROOT_DIRECTORY/../buildresult/*"

    # stunnel + its OpenSSL dependency (binaries + build trees)
    ( cd "$THIRD_PARTY_DIRECTORY/stunnel/stunnel-5.75" && make distclean ) >/dev/null 2>&1
    rm -rfv "$THIRD_PARTY_DIRECTORY/stunnel/openssl"
    rm -rfv "$THIRD_PARTY_DIRECTORY/stunnel/win-build"
    rm -fv "$THIRD_PARTY_DIRECTORY/stunnel/stunnel-5.75/src/stunnel"
    rm -fv "$THIRD_PARTY_DIRECTORY/stunnel/stunnel-5.75/bin/MGW32/stunnel.exe"
    rm -fv "$ROOT_DIRECTORY/../buildresult/stunnel"

fi

warning "files deleted"


#add exit if only file deletion is needed
#exit

CMAKE_CXX_COMPILER="g++"
CMAKE_C_COMPILER="gcc"
CMAKE_LINKER="ld"
CMAKE_C_FLAGS="-Wno-expansion-to-defined -Wno-shadow -Wno-declaration-after-statement -DUSE_LTM -DLTM_DESC"
CMAKE_C_FLAGS_RELEASE="-DDEBUG -Wno-expansion-to-defined -Wno-shadow -Wno-declaration-after-statement"
BUILD_CONFIG="Release"
#BUILD_CONFIG="Debug"


# ---- optional WSL mode: ./linux_build_script.sh --wsl -----------------------
# Building inside WSL on /mnt/c with a modern toolchain (GCC 15 / CMake 4) needs the
# tweaks the old _wsl_build.sh did: -std=gnu17, a CMake policy floor, fewer parallel
# jobs, the autotools regen for libmaxminddb, and a /mnt metadata-mount check.
WSL_MODE=0
case "$1" in --wsl|-w) WSL_MODE=1 ;; esac

# number of parallel build jobs passed to cmake/make (-j); override with: JOBS=8 ./linux_build_script.sh
JOBS="${JOBS:-32}"
WSL_CMAKE=""
if [ "$WSL_MODE" = "1" ]; then
  JOBS="${NJOBS:-4}"
  WSL_CMAKE="-DCMAKE_POLICY_VERSION_MINIMUM=3.5"            # CMake 4 dropped <3.5 compat
  CMAKE_C_FLAGS_RELEASE="$CMAKE_C_FLAGS_RELEASE -std=gnu17"  # GCC 15 defaults to C23
  case "$ROOT_DIRECTORY" in
    /mnt/*)
      mp="$(printf '%s' "$ROOT_DIRECTORY" | cut -d/ -f1-3)"
      findmnt -no OPTIONS "$mp" 2>/dev/null | grep -q metadata || {
        echo "ERROR: $mp is not mounted with 'metadata' - CMake will fail."
        echo "Add [automount] options=\"metadata\" to /etc/wsl.conf, then run 'wsl --shutdown'."
        exit 1; }
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


cd "$THIRD_PARTY_DIRECTORY"


#********************************************************
#****** mbedtls build                              ******
#********************************************************

message "building mbedtls (libmbedtls.a, libmbedx509.a, libmbedcrypto.a)"

cd mbedtls-3.6.6

cmake -G Ninja . -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" -DCMAKE_LINKER="$CMAKE_LINKER" -DCMAKE_C_COMPILER="$CMAKE_C_COMPILER" -DCMAKE_CXX_COMPILER="$CMAKE_CXX_COMPILER" -DCMAKE_C_FLAGS_RELEASE="$CMAKE_C_FLAGS_RELEASE" $WSL_CMAKE -DCMAKE_C_FLAGS="-fPIC" -DCMAKE_CXX_FLAGS="-fPIC"

cmake --build . -j"$JOBS" --target mbedtls

message "mbedtls build finished, moving some mbedtls files to their destination"


#copy include folder from mbedtls dir to deps

#a fresh clone has no empty deps/mbedtls/lib (git does not track empty directories), so the copies below would fail with "No such file or directory"; create it first (-p also makes the parent deps/mbedtls that the include copy needs)
mkdir -p "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/deps/mbedtls/lib"

rm -rf "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/deps/mbedtls/include"
cp -r "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/include" "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/deps/mbedtls/"

cp -v "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/library/libmbedtls.a" "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/deps/mbedtls/lib/libmbedtls.a"
cp -v "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/library/libmbedx509.a" "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/deps/mbedtls/lib/libmbedx509.a"
cp -v "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/library/libmbedcrypto.a" "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/deps/mbedtls/lib/libmbedcrypto.a"


# also copy them from the mbedtls-3.6.6 build directory into linkage-files (the chat server's own dependency directory); mbedtls is used for RSA encryption in the chat server

mkdir -p "$ROOT_DIRECTORY/main/linkage-files/linux"


cp -v "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/library/libmbedtls.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libmbedtls.a"
cp -v "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/library/libmbedx509.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libmbedx509.a"
cp -v "$THIRD_PARTY_DIRECTORY/mbedtls-3.6.6/library/libmbedcrypto.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libmbedcrypto.a"


cd ../

#********************************************************
#****** libdatachannel.so/libdatachannel.dll build ******
#********************************************************

message "building libdatachannel.so and its dependencies (libjuice, libsrtp, usrsctp)"

cd libdatachannel-0.24.2

cmake -G Ninja . -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" -DCMAKE_LINKER="$CMAKE_LINKER" -DCMAKE_C_COMPILER="$CMAKE_C_COMPILER" -DCMAKE_CXX_COMPILER="$CMAKE_CXX_COMPILER" -DCMAKE_C_FLAGS_RELEASE="$CMAKE_C_FLAGS_RELEASE" $WSL_CMAKE

cmake --build . -j"$JOBS" --target datachannel

cp -v "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/libdatachannel.so" "$ROOT_DIRECTORY/main/linkage-files/linux/libdatachannel.so"
cp -v "$THIRD_PARTY_DIRECTORY/libdatachannel-0.24.2/libdatachannel.so" "$ROOT_DIRECTORY/../buildresult/"

cd ../

#********************************************************
#******              libtommath.a build            ******
#********************************************************

message "building libtommath.a"


cd libtom/libtommath

cmake -G Ninja . -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" -DCMAKE_LINKER="$CMAKE_LINKER" -DCMAKE_C_COMPILER="$CMAKE_C_COMPILER" -DCMAKE_CXX_COMPILER="$CMAKE_CXX_COMPILER" -DCMAKE_C_FLAGS_RELEASE="$CMAKE_C_FLAGS_RELEASE" $WSL_CMAKE

cmake --build . -j"$JOBS"  --target libtommath

cp -v "$THIRD_PARTY_DIRECTORY/libtom/libtommath/libtommath.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libtommath.a"

cd ../../

#********************************************************
#******              libtomcrypt.a build           ******
#********************************************************
 
message "building libtomcrypt.a"

cd libtom/libtomcrypt
 
make -f makefile.unix -j"$JOBS"
cp -v "$THIRD_PARTY_DIRECTORY/libtom/libtomcrypt/libtomcrypt.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libtomcrypt.a"
cd ../../



#********************************************************
#****** theldus-websocket build                    ******
#********************************************************

message "building theldus-websocket (libws.a)"

cd theldus-websocket

cmake -G Ninja . -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" -DCMAKE_LINKER="$CMAKE_LINKER" -DCMAKE_C_COMPILER="$CMAKE_C_COMPILER" -DCMAKE_CXX_COMPILER="$CMAKE_CXX_COMPILER" -DCMAKE_C_FLAGS_RELEASE="$CMAKE_C_FLAGS_RELEASE" $WSL_CMAKE

cmake --build . -j"$JOBS"  --target ws
cp -r -v "$THIRD_PARTY_DIRECTORY/theldus-websocket/libws.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libws.a"

cd ../

 
#********************************************************
#****** libviolet build                            ******
#********************************************************

message "building libviolet.a"

cd libviolet-0.5.4

cmake -G Ninja . -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" -DCMAKE_LINKER="$CMAKE_LINKER" -DCMAKE_C_COMPILER="$CMAKE_C_COMPILER" -DCMAKE_CXX_COMPILER="$CMAKE_CXX_COMPILER" -DCMAKE_C_FLAGS_RELEASE="$CMAKE_C_FLAGS_RELEASE" $WSL_CMAKE

cmake --build . -j"$JOBS"  --target violet
cp -v "$THIRD_PARTY_DIRECTORY/libviolet-0.5.4/libviolet.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libviolet.a"

cd ../


#********************************************************
#****** libmaxminddb build                            ******
#********************************************************

message "building libmaxminddb-1.12.2"

cd libmaxminddb-1.12.2

if [ "$WSL_MODE" = "1" ]; then
  autoreconf -fi          # regen so stale /mnt/c timestamps don't trigger maintainer-mode
  ./configure
  make -C src -j"$JOBS"   # src/ only; the vendored tree lacks the test suite
else
  make clean
  chmod +x configure
  ./configure
  make
fi


cp -v "$THIRD_PARTY_DIRECTORY/libmaxminddb-1.12.2/src/.libs/libmaxminddb.a" "$ROOT_DIRECTORY/main/linkage-files/linux/"


cd ../

message "building libopus-1.5.2"

cd libopus-1.5.2

cmake -G Ninja . -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" -DCMAKE_LINKER="$CMAKE_LINKER" -DCMAKE_C_COMPILER="$CMAKE_C_COMPILER" -DCMAKE_CXX_COMPILER="$CMAKE_CXX_COMPILER" -DCMAKE_C_FLAGS_RELEASE="$CMAKE_C_FLAGS_RELEASE" $WSL_CMAKE

cmake --build . -j"$JOBS"  --target opus
cp -r -v "$THIRD_PARTY_DIRECTORY/libopus-1.5.2/libopus.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libopus.a"

cd ../../

#********************************************************
#******  build the browser client (client.html)   ******
#********************************************************
# the bundled http server serves client.html out of the buildresult directory, so build it here
# (build.py is fast, about a second) and copy it in next to the server binary below.

message "building the browser client (client.html)"
if type python3 >/dev/null 2>&1; then
  python3 "$ROOT_DIRECTORY/../client-source-code/client-development/build.py"
else
  warning "python3 not found - skipping client build, the served client.html may be stale or missing"
fi


#********************************************************
#******  at last, main-chat-server build          ******
#********************************************************

message "building chat server executable"


cd main

cmake -G Ninja . -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" -DCMAKE_LINKER="$CMAKE_LINKER" -DCMAKE_C_COMPILER="$CMAKE_C_COMPILER" -DCMAKE_CXX_COMPILER="$CMAKE_CXX_COMPILER" -DCMAKE_C_FLAGS_RELEASE="$CMAKE_C_FLAGS_RELEASE" $WSL_CMAKE
cmake --build . -j"$JOBS"


cp -v "$ROOT_DIRECTORY/main/chat-server" "$ROOT_DIRECTORY/../buildresult/chat-server.bin"
cp -v "$ROOT_DIRECTORY/main/linkage-files/linux/libdatachannel.so" "$ROOT_DIRECTORY/../buildresult/libdatachannel.so"

# On Linux the bare name libdatachannel.so is not enough; the executable looks for the soname (libdatachannel.so.0.24).



# (an absolute-path symlink did not work here, so we cd into buildresult and use a relative one)

# so cd into buildresult and create the symlink

cd ../../

echo "current directory is $PWD"

cd buildresult
ln -s libdatachannel.so libdatachannel.so.0.24

cd ../
cd server-source-code


#copy start script there

cp -v "$ROOT_DIRECTORY/main/unix_start_template" "$ROOT_DIRECTORY/../buildresult/start_server.sh"

cp -v "$THIRD_PARTY_DIRECTORY/libmaxminddb-1.12.2/dbip-country-lite-2025-06.mmdb" "$ROOT_DIRECTORY/../buildresult/"

# the page the bundled http server serves (built above)
if [ -f "$ROOT_DIRECTORY/../client-source-code/client.html" ]; then
  cp -v "$ROOT_DIRECTORY/../client-source-code/client.html" "$ROOT_DIRECTORY/../buildresult/"
else
  warning "client.html not found - the bundled http server will have no page to serve"
fi

chmod +x "$ROOT_DIRECTORY/../buildresult/start_server.sh"


#********************************************************
#****** bundled stunnel (wss front-end) build+copy ******
#********************************************************
# main.c can launch this to serve wss alongside ws, so the stunnel binary must
# sit next to chat-server.bin in buildresult. buildresult is wiped at the top of
# this script, so stunnel has to be (re)built and copied here on every build.

message "building + collecting bundled stunnel (wss front-end)"

STUNNEL_DIRECTORY="$THIRD_PARTY_DIRECTORY/stunnel"
if [ -f "$STUNNEL_DIRECTORY/_bundle_and_build.sh" ]; then
    bash "$STUNNEL_DIRECTORY/_bundle_and_build.sh"
    cp -v "$STUNNEL_DIRECTORY/stunnel-5.75/src/stunnel" "$ROOT_DIRECTORY/../buildresult/stunnel"
    chmod +x "$ROOT_DIRECTORY/../buildresult/stunnel"
else
    warning "stunnel/_bundle_and_build.sh not found - wss auto-launch will be unavailable"
fi

cd ../

message "build finished. Try running start_server.sh in buildresult directory"


#strip -v "$ROOT_DIRECTORY/../buildresult/chat-server.bin"

