#!/bin/bash


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
    echo -e "$LREDFailed$NC"
    exit 1
  fi
}

echo $build_directory


#QUESTION = "do you wish to delete any files?

ROOT_DIRECTORY="$PWD"
 

rm -f -v -r $ROOT_DIRECTORY/../buildresult/

mkdir $ROOT_DIRECTORY/../buildresult/


read -p "delete build files? " -n 1 -r
echo    # (optional) move to a new line
if [[ $REPLY =~ ^[Yy]$ ]]
then
    rm -fv "$ROOT_DIRECTORY/*.o"
    rm -rfv "$ROOT_DIRECTORY/mbedtls-3.5.1/CMakeFiles"
    rm -rfv "$ROOT_DIRECTORY/mbedtls-3.5.1/library/CMakeFiles"
    rm -fv "$ROOT_DIRECTORY/mbedtls-3.5.1/CMakeCache.txt"
    rm -rfv "$ROOT_DIRECTORY/mbedtls-3.5.1/output"
    rm -rfv "$ROOT_DIRECTORY/mbedtls-3.5.1/library/libmbedtls.a"
    rm -rfv "$ROOT_DIRECTORY/mbedtls-3.5.1/library/libmbedcrypto.a"
    rm -rfv "$ROOT_DIRECTORY/mbedtls-3.5.1/library/libmbedx509.a"
    rm -rfv "$ROOT_DIRECTORY/libdatachannel/CMakeFiles"
    rm -fv "$ROOT_DIRECTORY/libdatachannel/CMakeCache.txt"
    rm -rfv "$ROOT_DIRECTORY/libdatachannel/libdatachannel-static.a"
    rm -rfv "$ROOT_DIRECTORY/libdatachannel/libdatachannel.dll"
    rm -rfv "$ROOT_DIRECTORY/libdatachannel/libdatachannel.dll.a"
    rm -rfv "$ROOT_DIRECTORY/libtom/libtommath/cmake-build-release"
    rm -rfv "$ROOT_DIRECTORY/libtom/libtommath/CMakeFiles"
    rm -fv "$ROOT_DIRECTORY/libtom/libtommath/CMakeCache.txt"
    rm -fv "$ROOT_DIRECTORY/libtom/libtommath/libtommath.a" 
    rm -fv "$ROOT_DIRECTORY/libtom/libtomcrypt/libtomcrypt.a" 
    rm -rfv "$ROOT_DIRECTORY/theldus-websocket/CMakeFiles"
    rm -fv "$ROOT_DIRECTORY/theldus-websocket/CMakeCache.txt"
    rm -rfv "$ROOT_DIRECTORY/libviolet/CMakeFiles"
    rm -fv "$ROOT_DIRECTORY/libviolet/CMakeCache.txt"
    rm -fv "$ROOT_DIRECTORY/libviolet/libviolet.a"
    rm -rfv "$ROOT_DIRECTORY/main/linkage-files/windows/*"
    rm -rfv "$ROOT_DIRECTORY/main/linkage-files/linux/*"
    rm -rfv "$ROOT_DIRECTORY/main/CMakeFiles"
    rm -fv "$ROOT_DIRECTORY/main/CMakeCache.txt"
    rm -fv "$ROOT_DIRECTORY/main/cmake_install.cmake"
    rm -fv "$ROOT_DIRECTORY/main/build.ninja"
    rm -fv "$ROOT_DIRECTORY/main/.ninja_deps"
    rm -fv "$ROOT_DIRECTORY/main/.ninja_log"
    rm -fv "$ROOT_DIRECTORY/../buildresult/*"

fi

warning "files deleted"


#add exit if only file deletion is needed
#exit

CMAKE_CXX_COMPILER="g++"
CMAKE_C_COMPILER="gcc"
CMAKE_LINKER="ld"
CMAKE_C_FLAGS="-Wno-expansion-to-defined -Wno-shadow -Wnodeclaration-after-statement -DUSE_LTM -DLTM_DESC"
CMAKE_C_FLAGS_RELEASE="-DDEBUG -Wno-expansion-to-defined -Wno-shadow -Wno-declaration-after-statement"
BUILD_CONFIG="Release"
#BUILD_CONFIG="Debug"



#********************************************************
#****** mbedtls build                              ******
#********************************************************

message "building mbedtls (libmbedtls.a, libmbedx509.a, libmbedcrypto.a)"

cd mbedtls-3.5.1

cmake -G Ninja . -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" -DCMAKE_LINKER="$CMAKE_LINKER" -DCMAKE_C_COMPILER="$CMAKE_C_COMPILER" -DCMAKE_CXX_COMPILER="$CMAKE_CXX_COMPILER" -DCMAKE_C_FLAGS_RELEASE="$CMAKE_C_FLAGS_RELEASE" -DCMAKE_C_FLAGS="-fPIC" -DCMAKE_CXX_FLAGS="-fPIC"

cmake --build . -j32 --target mbedtls

message "mbedtls build finished, moving some mbedtls files to their destination"


cp -v "$ROOT_DIRECTORY/mbedtls-3.5.1/library/libmbedtls.a" "$ROOT_DIRECTORY/libdatachannel/deps/mbedtls/lib/libmbedtls.a"
cp -v "$ROOT_DIRECTORY/mbedtls-3.5.1/library/libmbedx509.a" "$ROOT_DIRECTORY/libdatachannel/deps/mbedtls/lib/libmbedx509.a"
cp -v "$ROOT_DIRECTORY/mbedtls-3.5.1/library/libmbedcrypto.a" "$ROOT_DIRECTORY/libdatachannel/deps/mbedtls/lib/libmbedcrypto.a"


#and also move them from resulting build directory of mbedtls-3.5.1 to dependencies directory of .exe itself, linkage-files (mbedtls is used for RSA encryption within chat server)

mkdir -p "$ROOT_DIRECTORY/main/linkage-files/linux"


cp -v "$ROOT_DIRECTORY/mbedtls-3.5.1/library/libmbedtls.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libmbedtls.a"
cp -v "$ROOT_DIRECTORY/mbedtls-3.5.1/library/libmbedx509.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libmbedx509.a"
cp -v "$ROOT_DIRECTORY/mbedtls-3.5.1/library/libmbedcrypto.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libmbedcrypto.a"


cd ../

#********************************************************
#****** libdatachannel.so/libdatachannel.dll build ******
#********************************************************

message "building libdatachannel.so and its dependencies (libjuice, libsrtp, usrsctp)"

cd libdatachannel

cmake -G Ninja . -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" -DCMAKE_LINKER="$CMAKE_LINKER" -DCMAKE_C_COMPILER="$CMAKE_C_COMPILER" -DCMAKE_CXX_COMPILER="$CMAKE_CXX_COMPILER" -DCMAKE_C_FLAGS_RELEASE="$CMAKE_C_FLAGS_RELEASE"

cmake --build . -j32 --target datachannel

cp -v "$ROOT_DIRECTORY/libdatachannel/libdatachannel.so" "$ROOT_DIRECTORY/main/linkage-files/linux/libdatachannel.so"
cp -v "$ROOT_DIRECTORY/libdatachannel/libdatachannel.so" "$ROOT_DIRECTORY/../buildresult/"

cd ../

#********************************************************
#******              libtommath.a build            ******
#********************************************************

message "building libtommath.a"


cd libtom/libtommath

cmake -G Ninja . -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" -DCMAKE_LINKER="$CMAKE_LINKER" -DCMAKE_C_COMPILER="$CMAKE_C_COMPILER" -DCMAKE_CXX_COMPILER="$CMAKE_CXX_COMPILER" -DCMAKE_C_FLAGS_RELEASE="$CMAKE_C_FLAGS_RELEASE"

cmake --build . -j32  --target libtommath

cp -v "$ROOT_DIRECTORY/libtom/libtommath/libtommath.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libtommath.a"

cd ../../

#********************************************************
#******              libtomcrypt.a build           ******
#********************************************************
 
message "building libtomcrypt.a"

cd libtom/libtomcrypt
 
make -f makefile.unix -j32
cp -v "$ROOT_DIRECTORY/libtom/libtomcrypt/libtomcrypt.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libtomcrypt.a"
cd ../../



#********************************************************
#****** theldus-websocket build                    ******
#********************************************************

message "building theldus-websocket (libws.a)"

cd theldus-websocket

cmake -G Ninja . -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" -DCMAKE_LINKER="$CMAKE_LINKER" -DCMAKE_C_COMPILER="$CMAKE_C_COMPILER" -DCMAKE_CXX_COMPILER="$CMAKE_CXX_COMPILER" -DCMAKE_C_FLAGS_RELEASE="$CMAKE_C_FLAGS_RELEASE"

cmake --build . -j32  --target ws
cp -r -v "$ROOT_DIRECTORY/theldus-websocket/libws.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libws.a"

cd ../

 
#********************************************************
#****** libviolet build                            ******
#********************************************************

message "building libviolet.a"

cd libviolet

cmake -G Ninja . -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" -DCMAKE_LINKER="$CMAKE_LINKER" -DCMAKE_C_COMPILER="$CMAKE_C_COMPILER" -DCMAKE_CXX_COMPILER="$CMAKE_CXX_COMPILER" -DCMAKE_C_FLAGS_RELEASE="$CMAKE_C_FLAGS_RELEASE"

cmake --build . -j32  --target violet
cp -v "$ROOT_DIRECTORY/libviolet/libviolet.a" "$ROOT_DIRECTORY/main/linkage-files/linux/libviolet.a"

cd ../


#********************************************************
#****** libmaxminddb build                            ******
#********************************************************

message "building libmaxminddb-1.12.2"

cd libmaxminddb-1.12.2

make clean
./configure
make


cp -v "$ROOT_DIRECTORY/libmaxminddb-1.12.2/src/.libs/libmaxminddb.a" "$ROOT_DIRECTORY/main/linkage-files/linux/"


cd ../




#********************************************************
#******  at last, main-chat-server build          ******
#********************************************************

message "building chat server executable"


cd main

cmake -G Ninja . -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" -DCMAKE_LINKER="$CMAKE_LINKER" -DCMAKE_C_COMPILER="$CMAKE_C_COMPILER" -DCMAKE_CXX_COMPILER="$CMAKE_CXX_COMPILER" -DCMAKE_C_FLAGS_RELEASE="$CMAKE_C_FLAGS_RELEASE"
cmake --build . -j32


cp -v "$ROOT_DIRECTORY/main/chat-server" "$ROOT_DIRECTORY/../buildresult/chat-server.bin"
cp -v "$ROOT_DIRECTORY/main/linkage-files/linux/libdatachannel.so" "$ROOT_DIRECTORY/../buildresult/libdatachannel.so"

#in linux, name libdatachannel.so is not enough, create symlink with that name pointing to libdatachannel.so, or executable wont find it



# this didnt work ln -s "$ROOT_DIRECTORY/../buildresult/libdatachannel.so" "$ROOT_DIRECTORY/../buildresult/libdatachannel.so.0.19"

#so cd there and set symlink

cd ../../

echo "cutrentdirectory is $PWD"

cd buildresult
ln -s libdatachannel.so libdatachannel.so.0.23

cd ../
cd server-source-code


#copy start script there

cp -v "$ROOT_DIRECTORY/main/unix_start_template" "$ROOT_DIRECTORY/../buildresult/start_server.sh"

cp -v "$ROOT_DIRECTORY/libmaxminddb-1.12.2/dbip-country-lite-2025-06.mmdb" "$ROOT_DIRECTORY/../buildresult/"

chmod +x "$ROOT_DIRECTORY/../buildresult/start_server.sh"

cd ../

message "build finished. Try running start_server.sh in buildresult directory"


strip -v "$ROOT_DIRECTORY/../buildresult/chat-server.bin"

message "add stripping binary from symbols"
