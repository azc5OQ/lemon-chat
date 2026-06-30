@echo off


:: Builds chat-server.exe on Windows. Run this .bat from cmd.
::
:: All third-party libraries are bundled and built from source, in place (not from precompiled
:: binaries), so the whole build is auditable. The script builds every dependency, then the server.
::
:: Requires:
::   - w64devkit: a MinGW-based toolchain providing gcc, g++, ld, ninja, make, windres. Used because
::     the server must build on both Windows and Linux, so MSVC is not an option.
::   - cmake (installed separately).
::
:: Setup:
::   1. Download w64devkit (e.g. from its GitHub releases) and unpack it anywhere.
::   2. Point the MINGWPATH variable below at the w64devkit "bin" folder.


:: >>> EDIT THIS to point at your own w64devkit "bin" folder <<<
SET "MINGWPATH=C:\Users\babikp\Downloads\w64devkit\bin"
set "CMAKE_MAKE_PROGRAM=%MINGWPATH%\ninja.exe"
SET "PATH=%MINGWPATH%;%PATH%"

:: number of parallel build jobs passed to cmake/make (-j); lower it if you run out of RAM
set "BUILD_JOBS=2"



::list of libraries used in project

::mbedtls (.a)
::libtommath (.a)
::libtomcrypt (.a)
::libdatachannel (.dll)
::libviolet (.a)
::theldus-websocket (.a)
::chat-server (.exe)

::the libraries cJSON, the console logging library (log), kokke-tiny-aes and ITH-SHA
::are compiled directly into the .exe

::note, when using w64devkit toolchain, what would be a .lib file has an ".a" extension like on linux, not .lib .. .dll stays .dll


::sets current working directory
set "ROOT_DIRECTORY=%~dp0"

set "THIRD_PARTY_DIRECTORY=%ROOT_DIRECTORY%third-party"

echo.
echo You need these tools for building : cmake, w64devkit  [w64devkit provides gcc, g++, ld, ninja, make, windres]
echo The optional stunnel/wss build also needs a full Perl, e.g. Strawberry Perl.

:: ---- show the paths this build will use, and confirm before doing anything ----
echo.
echo The build will use these paths:
echo.
echo    MINGWPATH      = %MINGWPATH%
echo    ninja          = %CMAKE_MAKE_PROGRAM%
echo    gcc            = %MINGWPATH%/gcc.exe
echo    g++            = %MINGWPATH%/g++.exe
echo    ld             = %MINGWPATH%/ld.exe
echo    project root   = %ROOT_DIRECTORY%
echo    third-party    = %THIRD_PARTY_DIRECTORY%
echo.
echo  ====================================================================
echo    parallel build jobs (-j) : %BUILD_JOBS%
echo    to change, edit   set "BUILD_JOBS=N"   near the top of this script
echo  ====================================================================
echo.
set /p pathsok= "Are these paths correct? (y/n): "
if /i not "%pathsok%"=="y" (
  echo Aborting - edit MINGWPATH at the top of this script and run it again.
  pause
  exit /b 1
)

set /p choice= "clean files from previous build if any y/n: "

echo %choice%

IF /i "%choice%"=="y" (
  
  ::delete any files that might be leftovers from previous build
  ::checking if they exist is not needed, would only waste space in .bat file

  rd "%THIRD_PARTY_DIRECTORY%\mbedtls-3.6.6\CMakeFiles"  /S /Q
  rd "%THIRD_PARTY_DIRECTORY%\mbedtls-3.6.6\library\CMakeFiles"  /S /Q
  del "%THIRD_PARTY_DIRECTORY%\mbedtls-3.6.6\CMakeCache.txt"

  del "%ROOT_DIRECTORY%\*.o" /S /Q
  del "%ROOT_DIRECTORY%\*.a" /S /Q
  del "%ROOT_DIRECTORY%\*.ninja_deps" /S /Q
  del "%ROOT_DIRECTORY%\*.ninja_log" /S /Q

  rd "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\CMakeFiles"  /S /Q
  del "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\CMakeCache.txt"
  del "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\libdatachannel-static.a"
  del "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\libdatachannel.dll"
  del "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\libdatachannel.dll.a"

  del "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\deps\mbedtls\lib\libmbedcrypto.a"
  del "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\deps\mbedtls\lib\libmbedtls.a"
  del "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\deps\mbedtls\lib\libmbedx509.a"

  rd "%THIRD_PARTY_DIRECTORY%\libtom\libtommath\cmake-build-release"   /S /Q
  rd "%THIRD_PARTY_DIRECTORY%\libtom\libtommath\CMakeFiles"   /S /Q
  del "%THIRD_PARTY_DIRECTORY%\libtom\libtommath\CMakeCache.txt"
  del "%THIRD_PARTY_DIRECTORY%\libtom\libtommath\libtommath.a"

  del "%THIRD_PARTY_DIRECTORY%\libtom\libtomcrypt\libtomcrypt.a"

  rd "%THIRD_PARTY_DIRECTORY%\theldus-websocket\CMakeFiles"  /S /Q
  del "%THIRD_PARTY_DIRECTORY%\theldus-websocket\CMakeCache.txt"

  rd "%THIRD_PARTY_DIRECTORY%\libviolet-0.5.4\CMakeFiles"  /S /Q
  del "%THIRD_PARTY_DIRECTORY%\libviolet-0.5.4\CMakeCache.txt"
  del "%THIRD_PARTY_DIRECTORY%\libviolet-0.5.4\libviolet.a"

  del "%THIRD_PARTY_DIRECTORY%\libmaxminddb-1.12.2\CMakeCache.txt"
  del "%THIRD_PARTY_DIRECTORY%\libmaxminddb-1.12.2\libmaxminddb.a"

  del "%THIRD_PARTY_DIRECTORY%\libopus-1.5.2\CMakeCache.txt"
  rd "%THIRD_PARTY_DIRECTORY%\libopus-1.5.2\CMakeFiles"  /S /Q
  del "%THIRD_PARTY_DIRECTORY%\libopus-1.5.2\libopus.a"

  del "%ROOT_DIRECTORY%\main\linkage-files\windows\*" /S /Q
  del "%ROOT_DIRECTORY%\main\linkage-files\linux\*" /S /Q

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

  :: stunnel + its OpenSSL dependency (binaries + build trees)
  rd "%THIRD_PARTY_DIRECTORY%\stunnel\win-build" /S /Q
  rd "%THIRD_PARTY_DIRECTORY%\stunnel\openssl" /S /Q
  rd "%THIRD_PARTY_DIRECTORY%\stunnel\stunnel-5.75\obj" /S /Q
  rd "%THIRD_PARTY_DIRECTORY%\stunnel\stunnel-5.75\bin" /S /Q
  del "%ROOT_DIRECTORY%\..\buildresult\stunnel.exe"
)


set /p buildok= "Proceed with the build? (y/n): "
if /i not "%buildok%"=="y" (
  echo Build cancelled - no build performed.
  pause
  exit /b 0
)


set BUILD_CONFIG=Release
::set BUILD_CONFIG=Debug


mkdir "%ROOT_DIRECTORY%\main\linkage-files\windows\"
mkdir "%ROOT_DIRECTORY%\..\buildresult\"

cd "%ROOT_DIRECTORY%\main\"


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


cd "%THIRD_PARTY_DIRECTORY%"
cd mbedtls-3.6.6
::this command will call build tool "ninja". This tool can also be called directly, but its better to call it through cmake, otherwise ninja would have to be added into path of operating system

cmake -G Ninja . -DCMAKE_BUILD_TYPE=%BUILD_CONFIG% "-DCMAKE_MAKE_PROGRAM=%CMAKE_MAKE_PROGRAM%" "-DCMAKE_LINKER=%CMAKE_LINKER%" -DCMAKE_C_COMPILER="%CMAKE_C_COMPILER%" -DCMAKE_CXX_COMPILER="%CMAKE_CXX_COMPILER%" "-DCMAKE_C_COMPILER_AR=%CMAKE_C_COMPILER_AR%" "-DCMAKE_C_COMPILER_RANLIB=%CMAKE_C_COMPILER_RANLIN%" "-DCMAKE_C_FLAGS=%CMAKE_C_FLAGS%" "-DCMAKE_C_FLAGS_DEBUG=%CMAKE_C_FLAGS_DEBUG%" "-DCMAKE_C_FLAGS_RELEASE=%CMAKE_C_FLAGS_RELEASE%" "-DCMAKE_C_FLAGS_MINSIZEREL=%CMAKE_C_FLAGS_MINSIZEREL%" "-DCMAKE_C_FLAGS_RELWITHDEBINFO=%CMAKE_C_FLAGS_RELWITHDEBINFO%"
cmake --build . -j%BUILD_JOBS% --target mbedtls


::copy include folder from mbedtls dir to deps
rd "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\deps\mbedtls\include" /Q /S
mkdir "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\deps\mbedtls\include"
xcopy "%THIRD_PARTY_DIRECTORY%\mbedtls-3.6.6\include" "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\deps\mbedtls\include" /E /H /I /Y

::after the build of mbedtls is done, few more things
::libdatachannel needs three static libraries to build (libmbedtls.a, libmbedx509.a, libmbedcrypto.a)
::move them from resulting build directory of mbedtls-3.6.6 to dependencies directory of libdatachannel, so libdatachannel can detect these libraries
copy "%THIRD_PARTY_DIRECTORY%\mbedtls-3.6.6\library\libmbedtls.a" "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\deps\mbedtls\lib\libmbedtls.a"
copy "%THIRD_PARTY_DIRECTORY%\mbedtls-3.6.6\library\libmbedx509.a" "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\deps\mbedtls\lib\libmbedx509.a"
copy "%THIRD_PARTY_DIRECTORY%\mbedtls-3.6.6\library\libmbedcrypto.a" "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\deps\mbedtls\lib\libmbedcrypto.a"


::also move them from resulting build directory of mbedtls-3.6.6 to dependencies directory of .exe itself, linkage-files (mbedtls is used for RSA encryption within chat-server.exe)
copy "%THIRD_PARTY_DIRECTORY%\mbedtls-3.6.6\library\libmbedtls.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libmbedtls.a"
copy "%THIRD_PARTY_DIRECTORY%\mbedtls-3.6.6\library\libmbedx509.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libmbedx509.a"
copy "%THIRD_PARTY_DIRECTORY%\mbedtls-3.6.6\library\libmbedcrypto.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libmbedcrypto.a"

cd ../

::********************************************************
::****** libdatachannel.so/libdatachannel.dll build ******
::********************************************************


cd libdatachannel-0.24.2

cmake -G Ninja . -DCMAKE_BUILD_TYPE=%BUILD_CONFIG% "-DCMAKE_MAKE_PROGRAM=%CMAKE_MAKE_PROGRAM%" "-DCMAKE_LINKER=%CMAKE_LINKER%" -DCMAKE_C_COMPILER="%CMAKE_C_COMPILER%" -DCMAKE_CXX_COMPILER="%CMAKE_CXX_COMPILER%" "-DCMAKE_C_COMPILER_AR=%CMAKE_C_COMPILER_AR%" "-DCMAKE_C_COMPILER_RANLIB=%CMAKE_C_COMPILER_RANLIN%" "-DCMAKE_C_FLAGS=%CMAKE_C_FLAGS%" "-DCMAKE_C_FLAGS_DEBUG=%CMAKE_C_FLAGS_DEBUG%" "-DCMAKE_C_FLAGS_RELEASE=%CMAKE_C_FLAGS_RELEASE%" "-DCMAKE_C_FLAGS_MINSIZEREL=%CMAKE_C_FLAGS_MINSIZEREL%" "-DCMAKE_C_FLAGS_RELWITHDEBINFO=%CMAKE_C_FLAGS_RELWITHDEBINFO%"
cmake --build . -j%BUILD_JOBS%  --target datachannel
::building the explicit "datachannel" target avoids picking up a stale .a from leftover files


copy "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\libdatachannel.dll" "%ROOT_DIRECTORY%\main\linkage-files\windows\libdatachannel.dll"
::the .dll.a import library is needed at link time; mingw produces it alongside the .dll so other code can link against the DLL
copy "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\libdatachannel.dll.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libdatachannel.dll.a"

cd ../

::********************************************************
::******              libtommath.a build            ******
::********************************************************

cd libtom/libtommath

cmake -G Ninja . -DCMAKE_BUILD_TYPE=%BUILD_CONFIG% "-DCMAKE_MAKE_PROGRAM=%CMAKE_MAKE_PROGRAM%" "-DCMAKE_LINKER=%CMAKE_LINKER%" -DCMAKE_C_COMPILER="%CMAKE_C_COMPILER%" -DCMAKE_CXX_COMPILER="%CMAKE_CXX_COMPILER%" "-DCMAKE_C_COMPILER_AR=%CMAKE_C_COMPILER_AR%" "-DCMAKE_C_COMPILER_RANLIB=%CMAKE_C_COMPILER_RANLIN%" "-DCMAKE_C_FLAGS=%CMAKE_C_FLAGS%" "-DCMAKE_C_FLAGS_DEBUG=%CMAKE_C_FLAGS_DEBUG%" "-DCMAKE_C_FLAGS_RELEASE=%CMAKE_C_FLAGS_RELEASE%" "-DCMAKE_C_FLAGS_MINSIZEREL=%CMAKE_C_FLAGS_MINSIZEREL%" "-DCMAKE_C_FLAGS_RELWITHDEBINFO=%CMAKE_C_FLAGS_RELWITHDEBINFO%"
cmake --build . -j%BUILD_JOBS%  --target libtommath

copy "%THIRD_PARTY_DIRECTORY%\libtom\libtommath\libtommath.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libtommath.a"

cd ../../

::********************************************************
::****** libtomcrypt.a build                        ******
::********************************************************

cd libtom/libtomcrypt

:: libtomcrypt does not have a CMakeLists.txt, so it cannot be built using CMake.
:: Why doesn’t it have one? Because nobody has created it yet, but there is a pull request for it on GitHub.
:: From my experience, it does not work with the CLion toolchain; you need to download the full MinGW toolchain.

make -f makefile.mingw -j%BUILD_JOBS%

copy "%THIRD_PARTY_DIRECTORY%\libtom\libtomcrypt\libtomcrypt.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libtomcrypt.a"

cd ../../


::********************************************************
::****** theldus-websocket build	            ******
::********************************************************


cd theldus-websocket

cmake -G Ninja . -DCMAKE_BUILD_TYPE=%BUILD_CONFIG% "-DCMAKE_MAKE_PROGRAM=%CMAKE_MAKE_PROGRAM%" "-DCMAKE_LINKER=%CMAKE_LINKER%" -DCMAKE_C_COMPILER="%CMAKE_C_COMPILER%" -DCMAKE_CXX_COMPILER="%CMAKE_CXX_COMPILER%" "-DCMAKE_C_COMPILER_AR=%CMAKE_C_COMPILER_AR%" "-DCMAKE_C_COMPILER_RANLIB=%CMAKE_C_COMPILER_RANLIN%" "-DCMAKE_C_FLAGS=%CMAKE_C_FLAGS%" "-DCMAKE_C_FLAGS_DEBUG=%CMAKE_C_FLAGS_DEBUG%" "-DCMAKE_C_FLAGS_RELEASE=%CMAKE_C_FLAGS_RELEASE%" "-DCMAKE_C_FLAGS_MINSIZEREL=%CMAKE_C_FLAGS_MINSIZEREL%" "-DCMAKE_C_FLAGS_RELWITHDEBINFO=%CMAKE_C_FLAGS_RELWITHDEBINFO%"
cmake --build . -j%BUILD_JOBS%  --target ws

copy "%THIRD_PARTY_DIRECTORY%\theldus-websocket\libws.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libws.a"

cd ../


cd libviolet-0.5.4

::********************************************************
::****** libviolet build                            ******
::********************************************************


cmake -G Ninja . -DCMAKE_BUILD_TYPE=%BUILD_CONFIG% "-DCMAKE_MAKE_PROGRAM=%CMAKE_MAKE_PROGRAM%" "-DCMAKE_LINKER=%CMAKE_LINKER%" -DCMAKE_C_COMPILER="%CMAKE_C_COMPILER%" -DCMAKE_CXX_COMPILER="%CMAKE_CXX_COMPILER%" "-DCMAKE_C_COMPILER_AR=%CMAKE_C_COMPILER_AR%" "-DCMAKE_C_COMPILER_RANLIB=%CMAKE_C_COMPILER_RANLIN%" "-DCMAKE_C_FLAGS=%CMAKE_C_FLAGS%" "-DCMAKE_C_FLAGS_DEBUG=%CMAKE_C_FLAGS_DEBUG%" "-DCMAKE_C_FLAGS_RELEASE=%CMAKE_C_FLAGS_RELEASE%" "-DCMAKE_C_FLAGS_MINSIZEREL=%CMAKE_C_FLAGS_MINSIZEREL%" "-DCMAKE_C_FLAGS_RELWITHDEBINFO=%CMAKE_C_FLAGS_RELWITHDEBINFO%"
cmake --build . -j%BUILD_JOBS%  --target violet


copy "%THIRD_PARTY_DIRECTORY%\libviolet-0.5.4\libviolet.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libviolet.a"


::********************************************************
::****** libmaxminddb build                        ******
::********************************************************

cd "%THIRD_PARTY_DIRECTORY%\libmaxminddb-1.12.2\"

make clean

cmake -G Ninja . -DCMAKE_BUILD_TYPE=%BUILD_CONFIG% "-DCMAKE_MAKE_PROGRAM=%CMAKE_MAKE_PROGRAM%" "-DCMAKE_LINKER=%CMAKE_LINKER%" -DCMAKE_C_COMPILER="%CMAKE_C_COMPILER%" -DCMAKE_CXX_COMPILER="%CMAKE_CXX_COMPILER%" "-DCMAKE_C_COMPILER_AR=%CMAKE_C_COMPILER_AR%" "-DCMAKE_C_COMPILER_RANLIB=%CMAKE_C_COMPILER_RANLIN%" "-DCMAKE_C_FLAGS=%CMAKE_C_FLAGS%" "-DCMAKE_C_FLAGS_DEBUG=%CMAKE_C_FLAGS_DEBUG%" "-DCMAKE_C_FLAGS_RELEASE=%CMAKE_C_FLAGS_RELEASE%" "-DCMAKE_C_FLAGS_MINSIZEREL=%CMAKE_C_FLAGS_MINSIZEREL%" "-DCMAKE_C_FLAGS_RELWITHDEBINFO=%CMAKE_C_FLAGS_RELWITHDEBINFO%"
cmake --build . -j%BUILD_JOBS% --target maxminddb

copy "%THIRD_PARTY_DIRECTORY%\libmaxminddb-1.12.2\libmaxminddb.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libmaxminddb.a"


cd ../


::********************************************************
::****** libopus build                        ******
::********************************************************

cd "%THIRD_PARTY_DIRECTORY%\libopus-1.5.2\"

make clean

cmake -G Ninja . -DCMAKE_BUILD_TYPE=%BUILD_CONFIG% "-DCMAKE_MAKE_PROGRAM=%CMAKE_MAKE_PROGRAM%" "-DCMAKE_LINKER=%CMAKE_LINKER%" -DCMAKE_C_COMPILER="%CMAKE_C_COMPILER%" -DCMAKE_CXX_COMPILER="%CMAKE_CXX_COMPILER%" "-DCMAKE_C_COMPILER_AR=%CMAKE_C_COMPILER_AR%" "-DCMAKE_C_COMPILER_RANLIB=%CMAKE_C_COMPILER_RANLIN%" "-DCMAKE_C_FLAGS=%CMAKE_C_FLAGS%" "-DCMAKE_C_FLAGS_DEBUG=%CMAKE_C_FLAGS_DEBUG%" "-DCMAKE_C_FLAGS_RELEASE=%CMAKE_C_FLAGS_RELEASE%" "-DCMAKE_C_FLAGS_MINSIZEREL=%CMAKE_C_FLAGS_MINSIZEREL%" "-DCMAKE_C_FLAGS_RELWITHDEBINFO=%CMAKE_C_FLAGS_RELWITHDEBINFO%"
cmake --build . -j%BUILD_JOBS% --target opus

copy "%THIRD_PARTY_DIRECTORY%\libopus-1.5.2\libopus.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libopus.a"


cd ../../


::********************************************************
::******  build the browser client (client.html)  ******
::********************************************************
:: the bundled http server serves client.html out of the buildresult directory, so build it here and copy
:: it in next to the exe below. build.py is fast (about a second) and needs python on PATH.

where python >nul 2>nul
if errorlevel 1 (
  echo   WARNING: python not on PATH - skipping client build, the served client.html may be stale or missing.
) else (
  echo Building the browser client client.html ...
  python "%ROOT_DIRECTORY%\..\client-source-code\client-development\build.py"
)


::********************************************************
::******  at last, chat-server.exe build          ******
::********************************************************


cd main

cmake -G Ninja . -DCMAKE_BUILD_TYPE=%BUILD_CONFIG% "-DCMAKE_MAKE_PROGRAM=%CMAKE_MAKE_PROGRAM%" "-DCMAKE_LINKER=%CMAKE_LINKER%" -DCMAKE_C_COMPILER="%CMAKE_C_COMPILER%" -DCMAKE_CXX_COMPILER="%CMAKE_CXX_COMPILER%" "-DCMAKE_C_COMPILER_AR=%CMAKE_C_COMPILER_AR%" "-DCMAKE_C_COMPILER_RANLIB=%CMAKE_C_COMPILER_RANLIN%" "-DCMAKE_C_FLAGS=%CMAKE_C_FLAGS%" "-DCMAKE_C_FLAGS_DEBUG=%CMAKE_C_FLAGS_DEBUG%" "-DCMAKE_C_FLAGS_RELEASE=%CMAKE_C_FLAGS_RELEASE%" "-DCMAKE_C_FLAGS_MINSIZEREL=%CMAKE_C_FLAGS_MINSIZEREL%" "-DCMAKE_C_FLAGS_RELWITHDEBINFO=%CMAKE_C_FLAGS_RELWITHDEBINFO%"
cmake --build . -j%BUILD_JOBS%


copy "%THIRD_PARTY_DIRECTORY%\libmaxminddb-1.12.2\dbip-country-lite-2025-06.mmdb" "%ROOT_DIRECTORY%\..\buildresult\"
move "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\libdatachannel.dll" "%ROOT_DIRECTORY%\..\buildresult\"
move "%ROOT_DIRECTORY%\main\chat-server.exe" "%ROOT_DIRECTORY%\..\buildresult\"

:: the page the bundled http server serves (built above)
if exist "%ROOT_DIRECTORY%\..\client-source-code\client.html" (
  copy /Y "%ROOT_DIRECTORY%\..\client-source-code\client.html" "%ROOT_DIRECTORY%\..\buildresult\"
) else (
  echo   WARNING: client.html not found - the bundled http server will have no page to serve.
)


::********************************************************
::****** optional: bundled stunnel - wss front-end ******
::********************************************************
:: stunnel.exe lets the server also serve wss:// on Windows, but building it needs
:: a FULL Perl for OpenSSL. Git-for-Windows' Perl is too minimal; Strawberry Perl
:: works. If no suitable Perl is found we SKIP stunnel and still finish the build -
:: the server runs fine over ws:// without it.

echo.
perl -MExtUtils::MakeMaker -MLocale::Maketext::Simple -e "exit 0" 2>nul
if errorlevel 1 goto stunnel_noperl
echo Perl found - building the bundled stunnel wss front-end...
where bash >nul 2>nul
if errorlevel 1 goto stunnel_nobash
bash "%THIRD_PARTY_DIRECTORY%\stunnel\_bundle_and_build_windows.sh"
if exist "%THIRD_PARTY_DIRECTORY%\stunnel\stunnel-5.75\bin\MGW32\stunnel.exe" copy /Y "%THIRD_PARTY_DIRECTORY%\stunnel\stunnel-5.75\bin\MGW32\stunnel.exe" "%ROOT_DIRECTORY%\..\buildresult\stunnel.exe"
goto stunnel_done

:stunnel_nobash
echo   bash not on PATH - run third-party\stunnel\_bundle_and_build_windows.sh manually to build stunnel.
goto stunnel_done

:stunnel_noperl
echo ==========================================================================
echo  NOTE: no suitable Perl found - SKIPPING the optional stunnel wss build.
echo  The chat server is built and runs over ws:// without it. For wss on Windows
echo  install Strawberry Perl from https://strawberryperl.com and then run
echo  third-party\stunnel\_bundle_and_build_windows.sh with perl + w64devkit on PATH.
echo ==========================================================================

:stunnel_done
echo.


::cd ../

pause
