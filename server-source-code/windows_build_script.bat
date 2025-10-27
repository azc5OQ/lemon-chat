@echo off

::
::this is the script that is supposed to be run in order to build chat-server.exe on windows
::every library needed to build this project is already included here and configured for convenience
::this script builds third party libs here "in place", libraries used here are not binaries, they have to be build aswell, i thought it would be more trustworthy project if it build libs aswell instead of using precompiled ones, like other projects are doing
::basically, you should just run the .bat file from cmd and it should build the chat-server.exe for you with all nessecary dependencies
::
:: ****  BUT  *****
::
::nothing is really that easy is it.  This project uses w64devkit toolchain, which can be thought of as an alternative to microsofts visual studio C++ toolchain.
::toolchain is a collection of tools that build source code into binary files. (in case of this project, these tools are cmake, ld, gcc, gcc++, clang, ninja)
::why w64devkit specifically? Because some libraries in this project need that to be built. server source code needed to be compatible with windows and linux so other solutions that visual studio were needed
::I found w64devkit to be working while other not. Do what works for you
::
:: The steps: 
:: download w64devkit somewhere from internet, from github for example
:: install / unpack w64devkit to location you want
:: manually overwrite MINGWPATH variable in this .bat file, so that it poiints to your filesystem location of w64devkit bin folder. . My was C:/software/w64devkit/bin as you can see down below
::


SET "MINGWPATH=C:/software/w64devkit/bin" ::set this
set "CMAKE_MAKE_PROGRAM=%MINGWPATH%\ninja.exe"
SET "PATH=%MINGWPATH%;%PATH%"


::list of libraries used in project

::mbedtls (.a)
::libtommath (.a)
::libtomcrypt (.a)
::libdatachannel (.dll)
::libviolet (.a)
::theldus-websocket (.a)
::chat-server (.exe)

::there are a
::libraries cJSON , console logging library (log), kokke-tiny-aes and ITH-SHA are built into .exe

::note, when using w64devkit toolchain, what would be a .lib file has an ".a" extension like on linux, not .lib .. .dll stays .dll


::sets current working directory
set "ROOT_DIRECTORY=%~dp0"

set /p choice= "clean files from previous build if any y/n: "

echo %choice%

IF /i "%choice%"=="Y" IF /i "%choice%"=="y" (
  
  ::delete any files that might be leftovers from previous build
  ::checking if they exist is not needed, would only waste space in .bat file

  rd "%ROOT_DIRECTORY%\mbedtls-3.5.1\CMakeFiles"  /S /Q
  rd "%ROOT_DIRECTORY%\mbedtls-3.5.1\library\CMakeFiles"  /S /Q
  del "%ROOT_DIRECTORY%\mbedtls-3.5.1\CMakeCache.txt"

  del "%ROOT_DIRECTORY%\*.o" /S /Q
  del "%ROOT_DIRECTORY%\*.a" /S /Q
  del "%ROOT_DIRECTORY%\*.ninja_deps" /S /Q
  del "%ROOT_DIRECTORY%\*.ninja_log" /S /Q


  rd "%ROOT_DIRECTORY%\libdatachannel\CMakeFiles"  /S /Q
  del "%ROOT_DIRECTORY%\libdatachannel\CMakeCache.txt"
  del "%ROOT_DIRECTORY%\libdatachannel\libdatachannel-static.a"
  del "%ROOT_DIRECTORY%\libdatachannel\libdatachannel.dll"
  del "%ROOT_DIRECTORY%\libdatachannel\libdatachannel.dll.a"

  del "%ROOT_DIRECTORY%\libdatachannel\deps\mbedtls\lib\libmbedcrypto.a"
  del "%ROOT_DIRECTORY%\libdatachannel\deps\mbedtls\lib\libmbedtls.a"
  del "%ROOT_DIRECTORY%\libdatachannel\deps\mbedtls\lib\libmbedx509.a"

  rd "%ROOT_DIRECTORY%\libtom\libtommath\cmake-build-release"   /S /Q
  rd "%ROOT_DIRECTORY%\libtom\libtommath\CMakeFiles"   /S /Q
  del "%ROOT_DIRECTORY%\libtom\libtommath\CMakeCache.txt"
  del "%ROOT_DIRECTORY%\libtom\libtommath\libtommath.a"

  del "%ROOT_DIRECTORY%\libtom\libtomcrypt\libtomcrypt.a"

  del "%ROOT_DIRECTORY%\main\linkage-files\windows\*" /S /Q
  del "%ROOT_DIRECTORY%\main\linkage-files\linux\*" /S /Q
  rd "%ROOT_DIRECTORY%\theldus-websocket\CMakeFiles"  /S /Q
  del "%ROOT_DIRECTORY%\theldus-websocket\CMakeCache.txt"

  rd "%ROOT_DIRECTORY%\libviolet\CMakeFiles"  /S /Q
  del "%ROOT_DIRECTORY%\libviolet\CMakeCache.txt"
  del "%ROOT_DIRECTORY%\libviolet\libviolet.a"


  del "%ROOT_DIRECTORY%\libmaxminddb-1.12.2\CMakeCache.txt"
  del "%ROOT_DIRECTORY%\libmaxminddb-1.12.2\libmaxminddb.a"

  rd "%ROOT_DIRECTORY%\main\CMakeFiles"  /S /Q
  del "%ROOT_DIRECTORY%\main\CMakeCache.txt"
  del "%ROOT_DIRECTORY%\main\cmake_install.cmake"
  del "%ROOT_DIRECTORY%\main\build.ninja"
  del "%ROOT_DIRECTORY%\main\.ninja_deps"
  del "%ROOT_DIRECTORY%\main\.ninja_log"
  del "%ROOT_DIRECTORY%\main\liblemon-chat-server.dll.a"
  del "%ROOT_DIRECTORY%\main\chat-server.exe"
  del "%ROOT_DIRECTORY%\main\chat-server"
  del "%ROOT_DIRECTORY%\main\cmake-build-debug"  /S /Q
  del "%ROOT_DIRECTORY%\main\.cmake"  /S /Q
  del "%ROOT_DIRECTORY%\main\.idea"  /S /Q
  del "%ROOT_DIRECTORY%\main\libchat-server.dll.a"
  del "%ROOT_DIRECTORY%\..\buildresult\*" /S /Q
	
  rd "%ROOT_DIRECTORY%\main\.cmake" /S /Q
  rd "%ROOT_DIRECTORY%\main\.idea" /S /Q

  rd "%ROOT_DIRECTORY%\main\cmake-build-debug" /S /Q
)



set BUILD_CONFIG=Release
::set BUILD_CONFIG=Debug



set "CMAKE_CXX_COMPILER=%MINGWPATH%/g++.exe"
set "CMAKE_C_COMPILER=%MINGWPATH%/gcc.exe"
set "CMAKE_C_COMPILER_AR=%MINGWPATH%/gcc-ar.exe"
set "CMAKE_C_COMPILER_RANLIN=%MINGWPATH%/gcc-ranlib.exe"
set "CMAKE_C_FLAGS= -Wno-expansion-to-defined -Wno-shadow -Wno-declaration-after-statement -DUSE_LTM -DLTM_DESC"
set "CMAKE_C_FLAGS_DEBUG= -g"
set "CMAKE_C_FLAGS_RELEASE=	-O3 -DNDEBUG -Wno-expansion-to-defined -Wno-shadow -Wno-declaration-after-statement"
set "CMAKE_C_FLAGS_MINSIZEREL= -Os -DNDEBUG"
set "CMAKE_C_FLAGS_RELWITHDEBINFO= -O2 -g -DNDEBUG"

set "CMAKE_LINKER=%MINGWPATH%\ld.exe"




::********************************************************
::****** mbedtls build                              ******
::********************************************************


::zmeni sa priecinok, aby sa pracovalo so spravnym CMakeLists.txt, nieje dobre spustat cmake prikaz v inom root priecinku
cd "%ROOT_DIRECTORY%\mbedtls-3.5.1\"


::this command will call build tool "ninja". This tool can also be called directly but, its better to call it through cmake, otherwise ninja would have to be added into path of operating system

cmake -G Ninja . -DCMAKE_BUILD_TYPE=%BUILD_CONFIG% "-DCMAKE_MAKE_PROGRAM=%CMAKE_MAKE_PROGRAM%" "-DCMAKE_LINKER=%CMAKE_LINKER%" -DCMAKE_C_COMPILER="%CMAKE_C_COMPILER%" -DCMAKE_CXX_COMPILER="%CMAKE_CXX_COMPILER%" "-DCMAKE_C_COMPILER_AR=%CMAKE_C_COMPILER_AR%" "-DCMAKE_C_COMPILER_RANLIB=%CMAKE_C_COMPILER_RANLIN%" "-DCMAKE_C_FLAGS=%CMAKE_C_FLAGS%" "-DCMAKE_C_FLAGS_DEBUG=%CMAKE_C_FLAGS_DEBUG%" "-DCMAKE_C_FLAGS_RELEASE=%CMAKE_C_FLAGS_RELEASE%" "-DCMAKE_C_FLAGS_MINSIZEREL=%CMAKE_C_FLAGS_MINSIZEREL%" "-DCMAKE_C_FLAGS_RELWITHDEBINFO=%CMAKE_C_FLAGS_RELWITHDEBINFO%"
cmake --build . -j32 --target mbedtls

::after the build of mbedtls is done, few more things
::libdatachannel needs three static libraries to build (libmbedtls.a, libmbedx509.a, libmbedcrypto.a)
::move them from resulting build directory of mbedtls-3.5.1 to dependencies directory of libdatachannel, so libdatachannel can detect these libraries

copy "%ROOT_DIRECTORY%\mbedtls-3.5.1\library\libmbedtls.a" "%ROOT_DIRECTORY%\libdatachannel\deps\mbedtls\lib\libmbedtls.a"
copy "%ROOT_DIRECTORY%\mbedtls-3.5.1\library\libmbedx509.a" "%ROOT_DIRECTORY%\libdatachannel\deps\mbedtls\lib\libmbedx509.a"
copy "%ROOT_DIRECTORY%\mbedtls-3.5.1\library\libmbedcrypto.a" "%ROOT_DIRECTORY%\libdatachannel\deps\mbedtls\lib\libmbedcrypto.a"


::and also move them from resulting build directory of mbedtls-3.5.1 to dependencies directory of .exe itself, linkage-files (mbedtls is used for RSA encryption within chat-server.exe)
copy "%ROOT_DIRECTORY%\mbedtls-3.5.1\library\libmbedtls.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libmbedtls.a"
copy "%ROOT_DIRECTORY%\mbedtls-3.5.1\library\libmbedx509.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libmbedx509.a"
copy "%ROOT_DIRECTORY%\mbedtls-3.5.1\library\libmbedcrypto.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libmbedcrypto.a"



cd ../

::********************************************************
::****** libdatachannel.so/libdatachannel.dll build ******
::********************************************************


cd libdatachannel

cmake -G Ninja . -DCMAKE_BUILD_TYPE=%BUILD_CONFIG% "-DCMAKE_MAKE_PROGRAM=%CMAKE_MAKE_PROGRAM%" "-DCMAKE_LINKER=%CMAKE_LINKER%" -DCMAKE_C_COMPILER="%CMAKE_C_COMPILER%" -DCMAKE_CXX_COMPILER="%CMAKE_CXX_COMPILER%" "-DCMAKE_C_COMPILER_AR=%CMAKE_C_COMPILER_AR%" "-DCMAKE_C_COMPILER_RANLIB=%CMAKE_C_COMPILER_RANLIN%" "-DCMAKE_C_FLAGS=%CMAKE_C_FLAGS%" "-DCMAKE_C_FLAGS_DEBUG=%CMAKE_C_FLAGS_DEBUG%" "-DCMAKE_C_FLAGS_RELEASE=%CMAKE_C_FLAGS_RELEASE%" "-DCMAKE_C_FLAGS_MINSIZEREL=%CMAKE_C_FLAGS_MINSIZEREL%" "-DCMAKE_C_FLAGS_RELWITHDEBINFO=%CMAKE_C_FLAGS_RELWITHDEBINFO%"
cmake --build . -j32  --target datachannel
::in case cmake --build . -j32  --target datachannel wasnt called, somehow old .a file was still generated from leftover files , I thought I cleared the cache


copy "%ROOT_DIRECTORY%\libdatachannel\libdatachannel.dll" "%ROOT_DIRECTORY%\main\linkage-files\windows\libdatachannel.dll"
::the .dll.a file is needed during linking its type of dynamic library that needs, i cant explain it well but its different kind of .a file that is needed for certain use of .dll, mingw produces that file
copy "%ROOT_DIRECTORY%\libdatachannel\libdatachannel.dll.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libdatachannel.dll.a"

cd ../

::********************************************************
::******              libtommath.a build            ******
::********************************************************

cd libtom/libtommath

cmake -G Ninja . -DCMAKE_BUILD_TYPE=%BUILD_CONFIG% "-DCMAKE_MAKE_PROGRAM=%CMAKE_MAKE_PROGRAM%" "-DCMAKE_LINKER=%CMAKE_LINKER%" -DCMAKE_C_COMPILER="%CMAKE_C_COMPILER%" -DCMAKE_CXX_COMPILER="%CMAKE_CXX_COMPILER%" "-DCMAKE_C_COMPILER_AR=%CMAKE_C_COMPILER_AR%" "-DCMAKE_C_COMPILER_RANLIB=%CMAKE_C_COMPILER_RANLIN%" "-DCMAKE_C_FLAGS=%CMAKE_C_FLAGS%" "-DCMAKE_C_FLAGS_DEBUG=%CMAKE_C_FLAGS_DEBUG%" "-DCMAKE_C_FLAGS_RELEASE=%CMAKE_C_FLAGS_RELEASE%" "-DCMAKE_C_FLAGS_MINSIZEREL=%CMAKE_C_FLAGS_MINSIZEREL%" "-DCMAKE_C_FLAGS_RELWITHDEBINFO=%CMAKE_C_FLAGS_RELWITHDEBINFO%"
cmake --build . -j32  --target libtommath

copy "%ROOT_DIRECTORY%\libtom\libtommath\libtommath.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libtommath.a"

cd ../../

::********************************************************
::****** libtomcrypt.a build                        ******
::********************************************************

cd libtom/libtomcrypt

::libtomcrypt nema cmakelists.txt, a teda sa neda sa nan pouzit cmake. Preco nema? Pretoze nikto ho zatial nevytvoril, ale je na githube je na to pull request
::s mojej skusenosti ale nefunguje s clion toolchainom, treba stiahnut kompletny mingw toolchain

make -f makefile.mingw -j32

copy "%ROOT_DIRECTORY%\libtom\libtomcrypt\libtomcrypt.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libtomcrypt.a"

cd ../../


::********************************************************
::****** theldus-websocket build	            ******
::********************************************************


cd theldus-websocket

cmake -G Ninja . -DCMAKE_BUILD_TYPE=%BUILD_CONFIG% "-DCMAKE_MAKE_PROGRAM=%CMAKE_MAKE_PROGRAM%" "-DCMAKE_LINKER=%CMAKE_LINKER%" -DCMAKE_C_COMPILER="%CMAKE_C_COMPILER%" -DCMAKE_CXX_COMPILER="%CMAKE_CXX_COMPILER%" "-DCMAKE_C_COMPILER_AR=%CMAKE_C_COMPILER_AR%" "-DCMAKE_C_COMPILER_RANLIB=%CMAKE_C_COMPILER_RANLIN%" "-DCMAKE_C_FLAGS=%CMAKE_C_FLAGS%" "-DCMAKE_C_FLAGS_DEBUG=%CMAKE_C_FLAGS_DEBUG%" "-DCMAKE_C_FLAGS_RELEASE=%CMAKE_C_FLAGS_RELEASE%" "-DCMAKE_C_FLAGS_MINSIZEREL=%CMAKE_C_FLAGS_MINSIZEREL%" "-DCMAKE_C_FLAGS_RELWITHDEBINFO=%CMAKE_C_FLAGS_RELWITHDEBINFO%"
cmake --build . -j32  --target ws

copy "%ROOT_DIRECTORY%\theldus-websocket\libws.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libws.a"

cd ../


cd libviolet

::********************************************************
::****** libviolet build                            ******
::********************************************************


cmake -G Ninja . -DCMAKE_BUILD_TYPE=%BUILD_CONFIG% "-DCMAKE_MAKE_PROGRAM=%CMAKE_MAKE_PROGRAM%" "-DCMAKE_LINKER=%CMAKE_LINKER%" -DCMAKE_C_COMPILER="%CMAKE_C_COMPILER%" -DCMAKE_CXX_COMPILER="%CMAKE_CXX_COMPILER%" "-DCMAKE_C_COMPILER_AR=%CMAKE_C_COMPILER_AR%" "-DCMAKE_C_COMPILER_RANLIB=%CMAKE_C_COMPILER_RANLIN%" "-DCMAKE_C_FLAGS=%CMAKE_C_FLAGS%" "-DCMAKE_C_FLAGS_DEBUG=%CMAKE_C_FLAGS_DEBUG%" "-DCMAKE_C_FLAGS_RELEASE=%CMAKE_C_FLAGS_RELEASE%" "-DCMAKE_C_FLAGS_MINSIZEREL=%CMAKE_C_FLAGS_MINSIZEREL%" "-DCMAKE_C_FLAGS_RELWITHDEBINFO=%CMAKE_C_FLAGS_RELWITHDEBINFO%"
cmake --build . -j32  --target violet


copy "%ROOT_DIRECTORY%\libviolet\libviolet.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libviolet.a"


::********************************************************
::****** libmaxminddb builld                        ******
::********************************************************

cd "%ROOT_DIRECTORY%\libmaxminddb-1.12.2\"

make clean

cmake -G Ninja . -DCMAKE_BUILD_TYPE=%BUILD_CONFIG% "-DCMAKE_MAKE_PROGRAM=%CMAKE_MAKE_PROGRAM%" "-DCMAKE_LINKER=%CMAKE_LINKER%" -DCMAKE_C_COMPILER="%CMAKE_C_COMPILER%" -DCMAKE_CXX_COMPILER="%CMAKE_CXX_COMPILER%" "-DCMAKE_C_COMPILER_AR=%CMAKE_C_COMPILER_AR%" "-DCMAKE_C_COMPILER_RANLIB=%CMAKE_C_COMPILER_RANLIN%" "-DCMAKE_C_FLAGS=%CMAKE_C_FLAGS%" "-DCMAKE_C_FLAGS_DEBUG=%CMAKE_C_FLAGS_DEBUG%" "-DCMAKE_C_FLAGS_RELEASE=%CMAKE_C_FLAGS_RELEASE%" "-DCMAKE_C_FLAGS_MINSIZEREL=%CMAKE_C_FLAGS_MINSIZEREL%" "-DCMAKE_C_FLAGS_RELWITHDEBINFO=%CMAKE_C_FLAGS_RELWITHDEBINFO%"
cmake --build . -j32 --target maxminddb

copy "%ROOT_DIRECTORY%\libmaxminddb-1.12.2\libmaxminddb.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libmaxminddb.a"


cd ../


::********************************************************
::******  at last, chat-server.exe build          ******
::********************************************************


cd main

cmake -G Ninja . -DCMAKE_BUILD_TYPE=%BUILD_CONFIG% "-DCMAKE_MAKE_PROGRAM=%CMAKE_MAKE_PROGRAM%" "-DCMAKE_LINKER=%CMAKE_LINKER%" -DCMAKE_C_COMPILER="%CMAKE_C_COMPILER%" -DCMAKE_CXX_COMPILER="%CMAKE_CXX_COMPILER%" "-DCMAKE_C_COMPILER_AR=%CMAKE_C_COMPILER_AR%" "-DCMAKE_C_COMPILER_RANLIB=%CMAKE_C_COMPILER_RANLIN%" "-DCMAKE_C_FLAGS=%CMAKE_C_FLAGS%" "-DCMAKE_C_FLAGS_DEBUG=%CMAKE_C_FLAGS_DEBUG%" "-DCMAKE_C_FLAGS_RELEASE=%CMAKE_C_FLAGS_RELEASE%" "-DCMAKE_C_FLAGS_MINSIZEREL=%CMAKE_C_FLAGS_MINSIZEREL%" "-DCMAKE_C_FLAGS_RELWITHDEBINFO=%CMAKE_C_FLAGS_RELWITHDEBINFO%"
cmake --build . -j32


copy "%ROOT_DIRECTORY%\libmaxminddb-1.12.2\dbip-country-lite-2025-06.mmdb" "%ROOT_DIRECTORY%\..\buildresult\"
move "%ROOT_DIRECTORY%\libdatachannel\libdatachannel.dll" "%ROOT_DIRECTORY%\..\buildresult\"
copy "%ROOT_DIRECTORY%\main\chat-server.exe" "%ROOT_DIRECTORY%\..\buildresult\"


::cd ../

pause
