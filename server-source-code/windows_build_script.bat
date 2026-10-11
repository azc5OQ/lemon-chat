@echo off
setlocal

:: Build the Windows server and browser client into ..\buildresult.
:: Run this script from cmd; all paths are relative to this file.
::
:: Build order: toolchain -> eight libraries -> client -> server -> optional stunnel.
:: The bootstrap offers to download missing tools. Git for Windows is needed
:: for the optional stunnel build; its Bash and Perl are selected explicitly.
:: Stop the server before copying a new EXE or DLL into buildresult.

:: =============================================================================
:: SETUP | Paths and build options
:: =============================================================================

:: >>> EDIT THIS to point at your own w64devkit "bin" folder <<<
SET "MINGWPATH=%USERPROFILE%\Downloads\w64devkit\bin"
set "BUILD_CONFIG=Release"
set "BUILD_JOBS=2"
:: Use Debug for native debugging; lower BUILD_JOBS if the compiler runs out of RAM.
set "ROOT_DIRECTORY=%~dp0"
set "THIRD_PARTY_DIRECTORY=%ROOT_DIRECTORY%third-party"
set "CMAKE_MAKE_PROGRAM=%MINGWPATH%\ninja.exe"
SET "PATH=%MINGWPATH%;%PATH%"

:: third-party projects still declare ancient cmake_minimum_required versions; CMake 4
:: refuses those outright unless this floor is set (same fix the linux script uses)
set "CMAKE_POLICY_VERSION_MINIMUM=3.5"

:: =============================================================================
:: SETUP | Download missing build tools
:: =============================================================================

:: every download lands in %USERPROFILE%\Downloads and is skipped when the tool
:: (or the downloaded file) is already there, so this section is a no-op on a
:: machine that is already set up. versions are pinned - bump them here.
set "W64DEVKIT_VERSION=2.8.0"
set "NINJA_VERSION=1.12.1"
set "CMAKE_VERSION=4.3.1"
set "TOOLDOWNLOADS=%USERPROFILE%\Downloads"

:: ---- w64devkit (gcc, g++, ld, make, windres) ----
if exist "%MINGWPATH%\gcc.exe" goto bootstrap_have_w64devkit
echo.
echo w64devkit (the gcc toolchain) was not found at %MINGWPATH%
echo It can be downloaded now using bitsadmin (about 60 MB, from github.com/skeeto/w64devkit)
echo and unpacked into %TOOLDOWNLOADS%\w64devkit
set /p dlw64devkit= "download w64devkit v%W64DEVKIT_VERSION%? (y/n): "
if /i not "%dlw64devkit%"=="y" (
    echo Cannot build without a toolchain - download w64devkit yourself and point MINGWPATH at its bin folder.
    pause
    exit /b 1
)
if exist "%TOOLDOWNLOADS%\w64devkit-x64-%W64DEVKIT_VERSION%.7z.exe" goto bootstrap_unpack_w64devkit
bitsadmin /transfer w64devkit_download /download /priority FOREGROUND ^
    "https://github.com/skeeto/w64devkit/releases/download/v%W64DEVKIT_VERSION%/w64devkit-x64-%W64DEVKIT_VERSION%.7z.exe" ^
    "%TOOLDOWNLOADS%\w64devkit-x64-%W64DEVKIT_VERSION%.7z.exe"
if exist "%TOOLDOWNLOADS%\w64devkit-x64-%W64DEVKIT_VERSION%.7z.exe" goto bootstrap_unpack_w64devkit
echo bitsadmin failed, retrying with curl...
curl -L -o ^
    "%TOOLDOWNLOADS%\w64devkit-x64-%W64DEVKIT_VERSION%.7z.exe" ^
    "https://github.com/skeeto/w64devkit/releases/download/v%W64DEVKIT_VERSION%/w64devkit-x64-%W64DEVKIT_VERSION%.7z.exe"
if exist "%TOOLDOWNLOADS%\w64devkit-x64-%W64DEVKIT_VERSION%.7z.exe" goto bootstrap_unpack_w64devkit
echo ERROR: could not download w64devkit - get it manually from https://github.com/skeeto/w64devkit/releases
pause
exit /b 1

:bootstrap_unpack_w64devkit

:: the .7z.exe is a self-extractor; -y -o extracts silently, creating %TOOLDOWNLOADS%\w64devkit\
"%TOOLDOWNLOADS%\w64devkit-x64-%W64DEVKIT_VERSION%.7z.exe" -y -o"%TOOLDOWNLOADS%"
if not exist "%MINGWPATH%\gcc.exe" (
    echo ERROR: w64devkit self-extraction failed - unpack %TOOLDOWNLOADS%\w64devkit-x64-%W64DEVKIT_VERSION%.7z.exe by hand.
    pause
    exit /b 1
)

:bootstrap_have_w64devkit

:: ---- ninja (goes into the w64devkit bin folder, where CMAKE_MAKE_PROGRAM expects it) ----
if exist "%MINGWPATH%\ninja.exe" goto bootstrap_have_ninja
echo.
echo ninja (the build tool cmake drives) was not found in %MINGWPATH%
echo It can be downloaded now using bitsadmin (under 1 MB, from github.com/ninja-build/ninja)
set /p dlninja= "download ninja v%NINJA_VERSION%? (y/n): "
if /i not "%dlninja%"=="y" (
    echo Cannot build without ninja - download ninja-win.zip yourself and unpack it into %MINGWPATH%
    pause
    exit /b 1
)
if exist "%TOOLDOWNLOADS%\ninja-win.zip" goto bootstrap_unpack_ninja
bitsadmin /transfer ninja_download /download /priority FOREGROUND ^
    "https://github.com/ninja-build/ninja/releases/download/v%NINJA_VERSION%/ninja-win.zip" ^
    "%TOOLDOWNLOADS%\ninja-win.zip"
if exist "%TOOLDOWNLOADS%\ninja-win.zip" goto bootstrap_unpack_ninja
echo bitsadmin failed, retrying with curl...
curl -L -o "%TOOLDOWNLOADS%\ninja-win.zip" "https://github.com/ninja-build/ninja/releases/download/v%NINJA_VERSION%/ninja-win.zip"
if exist "%TOOLDOWNLOADS%\ninja-win.zip" goto bootstrap_unpack_ninja
echo ERROR: could not download ninja - get ninja-win.zip manually from https://github.com/ninja-build/ninja/releases
pause
exit /b 1

:bootstrap_unpack_ninja

:: tar.exe ships with Windows 10+ and understands zip archives
tar -xf "%TOOLDOWNLOADS%\ninja-win.zip" -C "%MINGWPATH%"
if not exist "%MINGWPATH%\ninja.exe" (
    echo ERROR: could not unpack ninja-win.zip into %MINGWPATH%
    pause
    exit /b 1
)

:bootstrap_have_ninja

:: ---- cmake (only when no cmake is installed at all) ----
where cmake >nul 2>nul
if not errorlevel 1 goto bootstrap_have_cmake
if exist "%TOOLDOWNLOADS%\cmake-%CMAKE_VERSION%-windows-x86_64\bin\cmake.exe" goto bootstrap_path_cmake
echo.
echo cmake was not found on this machine
echo It can be downloaded now using bitsadmin (about 45 MB, from github.com/Kitware/CMake)
set /p dlcmake= "download cmake v%CMAKE_VERSION%? (y/n): "
if /i not "%dlcmake%"=="y" (
    echo Cannot build without cmake - install it manually from https://cmake.org/download/
    pause
    exit /b 1
)
if exist "%TOOLDOWNLOADS%\cmake-%CMAKE_VERSION%-windows-x86_64.zip" goto bootstrap_unpack_cmake
bitsadmin /transfer cmake_download /download /priority FOREGROUND ^
    "https://github.com/Kitware/CMake/releases/download/v%CMAKE_VERSION%/cmake-%CMAKE_VERSION%-windows-x86_64.zip" ^
    "%TOOLDOWNLOADS%\cmake-%CMAKE_VERSION%-windows-x86_64.zip"
if exist "%TOOLDOWNLOADS%\cmake-%CMAKE_VERSION%-windows-x86_64.zip" goto bootstrap_unpack_cmake
echo bitsadmin failed, retrying with curl...
curl -L -o ^
    "%TOOLDOWNLOADS%\cmake-%CMAKE_VERSION%-windows-x86_64.zip" ^
    "https://github.com/Kitware/CMake/releases/download/v%CMAKE_VERSION%/cmake-%CMAKE_VERSION%-windows-x86_64.zip"
if exist "%TOOLDOWNLOADS%\cmake-%CMAKE_VERSION%-windows-x86_64.zip" goto bootstrap_unpack_cmake
echo ERROR: could not download cmake - install it manually from https://cmake.org/download/
pause
exit /b 1

:bootstrap_unpack_cmake
tar -xf "%TOOLDOWNLOADS%\cmake-%CMAKE_VERSION%-windows-x86_64.zip" -C "%TOOLDOWNLOADS%"

:bootstrap_path_cmake
if not exist "%TOOLDOWNLOADS%\cmake-%CMAKE_VERSION%-windows-x86_64\bin\cmake.exe" (
    echo ERROR: could not unpack cmake - install it manually from https://cmake.org/download/
    pause
    exit /b 1
)
set "PATH=%TOOLDOWNLOADS%\cmake-%CMAKE_VERSION%-windows-x86_64\bin;%PATH%"

:bootstrap_have_cmake

:: =============================================================================
:: SETUP | Confirm the selected toolchain
:: =============================================================================

:: Static libraries use the MinGW .a suffix. libdatachannel also supplies a DLL.
echo.
echo Tools used for building : cmake, ninja, w64devkit  [gcc, g++, ld, make, windres]
echo Missing ones were just auto-downloaded to %TOOLDOWNLOADS% by the bootstrap above.
echo The optional stunnel/wss build runs in Git for Windows' bash (no extra Perl needed).

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

:: =============================================================================
:: CLEAN | Optional removal of previous build artifacts
:: =============================================================================

:: Only this section removes previous build files. Answering n keeps the files
:: so the build tools can compile just what is missing or has changed.
:: Answering y also empties buildresult, including runtime files.
set "choice=n"
set /p choice= "clean files from previous build if any y/n [n]: "
echo %choice%
IF /i "%choice%"=="y" (
    :: Remove generated CMake state, compiled libraries, and staged output.
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
    rd "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\deps\mbedtls\include" /Q /S
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
    rd "%THIRD_PARTY_DIRECTORY%\libmaxminddb-1.12.2\CMakeFiles" /S /Q
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
    del "%ROOT_DIRECTORY%\..\buildresult\tstunnel.exe"
)

:: =============================================================================
:: OPTIONS | Include stunnel before confirming the build
:: =============================================================================

:: The chat server can build without stunnel. Building stunnel also builds its
:: OpenSSL dependency when needed, which can take a long time on the first run.
:: Press Enter or answer n to skip both. Existing stunnel binaries stay in place
:: if you chose to keep previous build files above.
set "BUILD_STUNNEL=n"
set /p BUILD_STUNNEL= "Build stunnel and its OpenSSL dependency too? (y/n) [n]: "

set /p buildok= "Proceed with the build? (y/n): "
if /i not "%buildok%"=="y" (
    echo Build cancelled - no build performed.
    pause
    exit /b 0
)

:: =============================================================================
:: SETUP | Compiler flags shared by CMake projects
:: =============================================================================

mkdir "%ROOT_DIRECTORY%\main\linkage-files\windows\"
mkdir "%ROOT_DIRECTORY%\..\buildresult\"
set "CMAKE_CXX_COMPILER=%MINGWPATH%/g++.exe"
set "CMAKE_C_COMPILER=%MINGWPATH%/gcc.exe"
set "CMAKE_C_COMPILER_AR=%MINGWPATH%/gcc-ar.exe"
set "CMAKE_C_COMPILER_RANLIB=%MINGWPATH%/gcc-ranlib.exe"
set "CMAKE_C_FLAGS= -Wno-expansion-to-defined -Wno-shadow -Wno-declaration-after-statement -DUSE_LTM -DLTM_DESC"
set "CMAKE_C_FLAGS_DEBUG= -g"
set "CMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -Wno-expansion-to-defined -Wno-shadow -Wno-declaration-after-statement"
set "CMAKE_C_FLAGS_MINSIZEREL= -Os -DNDEBUG"
set "CMAKE_C_FLAGS_RELWITHDEBINFO= -O2 -g -DNDEBUG"
set "CMAKE_LINKER=%MINGWPATH%\ld.exe"

:: =============================================================================
:: DEPENDENCY 1/8 | Mbed TLS: TLS and RSA libraries
:: =============================================================================

cd /d "%THIRD_PARTY_DIRECTORY%\mbedtls-3.6.6"
if errorlevel 1 goto build_failed

:: CMake selects Ninja and uses the shared compiler settings in :configure_cmake.
call :configure_cmake
if errorlevel 1 goto build_failed
cmake --build . -j%BUILD_JOBS% --target mbedtls
if errorlevel 1 goto build_failed

:: libdatachannel reads these headers from its deps tree. Keep existing copies
:: and copy only missing or newer headers so an unchanged build can reuse them.
if not exist "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\deps\mbedtls\include" mkdir "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\deps\mbedtls\include"
if not exist "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\deps\mbedtls\lib" mkdir "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\deps\mbedtls\lib"
xcopy "%THIRD_PARTY_DIRECTORY%\mbedtls-3.6.6\include" "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\deps\mbedtls\include" /E /H /I /Y /D

:: Stage all three Mbed TLS archives for libdatachannel.
copy ^
    "%THIRD_PARTY_DIRECTORY%\mbedtls-3.6.6\library\libmbedtls.a" ^
    "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\deps\mbedtls\lib\libmbedtls.a"
copy ^
    "%THIRD_PARTY_DIRECTORY%\mbedtls-3.6.6\library\libmbedx509.a" ^
    "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\deps\mbedtls\lib\libmbedx509.a"
copy ^
    "%THIRD_PARTY_DIRECTORY%\mbedtls-3.6.6\library\libmbedcrypto.a" ^
    "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\deps\mbedtls\lib\libmbedcrypto.a"

:: The server also links these archives directly for RSA and media encryption.
copy "%THIRD_PARTY_DIRECTORY%\mbedtls-3.6.6\library\libmbedtls.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libmbedtls.a"
copy "%THIRD_PARTY_DIRECTORY%\mbedtls-3.6.6\library\libmbedx509.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libmbedx509.a"
copy "%THIRD_PARTY_DIRECTORY%\mbedtls-3.6.6\library\libmbedcrypto.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libmbedcrypto.a"

:: =============================================================================
:: DEPENDENCY 2/8 | libdatachannel: WebRTC transport
:: =============================================================================

cd /d "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2"
if errorlevel 1 goto build_failed
call :configure_cmake
if errorlevel 1 goto build_failed
cmake --build . -j%BUILD_JOBS%  --target datachannel
if errorlevel 1 goto build_failed

::building the explicit "datachannel" target avoids picking up a stale .a from leftover files
copy ^
    "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\libdatachannel.dll" ^
    "%ROOT_DIRECTORY%\main\linkage-files\windows\libdatachannel.dll"

::the .dll.a import library is needed at link time; mingw produces it alongside the .dll so other code can link against the DLL
copy ^
    "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\libdatachannel.dll.a" ^
    "%ROOT_DIRECTORY%\main\linkage-files\windows\libdatachannel.dll.a"

:: =============================================================================
:: DEPENDENCY 3/8 | LibTomMath: integer arithmetic
:: =============================================================================

cd /d "%THIRD_PARTY_DIRECTORY%\libtom\libtommath"
if errorlevel 1 goto build_failed
call :configure_cmake
if errorlevel 1 goto build_failed
cmake --build . -j%BUILD_JOBS%  --target libtommath
if errorlevel 1 goto build_failed
copy "%THIRD_PARTY_DIRECTORY%\libtom\libtommath\libtommath.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libtommath.a"

:: =============================================================================
:: DEPENDENCY 4/8 | LibTomCrypt: cryptographic primitives
:: =============================================================================

cd /d "%THIRD_PARTY_DIRECTORY%\libtom\libtomcrypt"
if errorlevel 1 goto build_failed

:: LibTomCrypt uses its supplied MinGW makefile instead of CMake.
make -f makefile.mingw -j%BUILD_JOBS%
if errorlevel 1 goto build_failed
copy "%THIRD_PARTY_DIRECTORY%\libtom\libtomcrypt\libtomcrypt.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libtomcrypt.a"

:: =============================================================================
:: DEPENDENCY 5/8 | WebSocket transport
:: =============================================================================

cd /d "%THIRD_PARTY_DIRECTORY%\theldus-websocket"
if errorlevel 1 goto build_failed
call :configure_cmake
if errorlevel 1 goto build_failed
cmake --build . -j%BUILD_JOBS%  --target ws
if errorlevel 1 goto build_failed
copy "%THIRD_PARTY_DIRECTORY%\theldus-websocket\libws.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libws.a"

:: =============================================================================
:: DEPENDENCY 6/8 | Violet
:: =============================================================================

cd /d "%THIRD_PARTY_DIRECTORY%\libviolet-0.5.4"
if errorlevel 1 goto build_failed
call :configure_cmake
if errorlevel 1 goto build_failed
cmake --build . -j%BUILD_JOBS%  --target violet
if errorlevel 1 goto build_failed
copy "%THIRD_PARTY_DIRECTORY%\libviolet-0.5.4\libviolet.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libviolet.a"

:: =============================================================================
:: DEPENDENCY 7/8 | MaxMind DB: country lookup
:: =============================================================================

cd /d "%THIRD_PARTY_DIRECTORY%\libmaxminddb-1.12.2"
if errorlevel 1 goto build_failed
call :configure_cmake
if errorlevel 1 goto build_failed
cmake --build . -j%BUILD_JOBS% --target maxminddb
if errorlevel 1 goto build_failed
copy "%THIRD_PARTY_DIRECTORY%\libmaxminddb-1.12.2\libmaxminddb.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libmaxminddb.a"

:: =============================================================================
:: DEPENDENCY 8/8 | Opus: audio codec
:: =============================================================================

cd /d "%THIRD_PARTY_DIRECTORY%\libopus-1.5.2"
if errorlevel 1 goto build_failed
call :configure_cmake
if errorlevel 1 goto build_failed
cmake --build . -j%BUILD_JOBS% --target opus
if errorlevel 1 goto build_failed
copy "%THIRD_PARTY_DIRECTORY%\libopus-1.5.2\libopus.a" "%ROOT_DIRECTORY%\main\linkage-files\windows\libopus.a"

:: =============================================================================
:: CLIENT | Generate the browser page
:: =============================================================================

:: the bundled http server serves client.html out of the buildresult directory, so build it here and copy
:: it in next to the exe below. build.py is fast (about a second) and needs python on PATH.
where python >nul 2>nul
if errorlevel 1 (
    echo   WARNING: python not on PATH - skipping client build, the served client.html may be stale or missing.
) else (
    echo Building the browser client client.html ...
    python "%ROOT_DIRECTORY%\..\client-source-code\client-development\browser\build.py"
)

:: =============================================================================
:: SERVER | Build chat-server
:: =============================================================================

cd /d "%ROOT_DIRECTORY%\main"
if errorlevel 1 goto build_failed
call :configure_cmake
if errorlevel 1 goto build_failed
cmake --build . -j%BUILD_JOBS%
if errorlevel 1 goto build_failed

:: =============================================================================
:: PACKAGE | Copy runtime files into buildresult
:: =============================================================================

copy "%THIRD_PARTY_DIRECTORY%\libmaxminddb-1.12.2\dbip-country-lite-2025-06.mmdb" "%ROOT_DIRECTORY%\..\buildresult\"
copy /Y "%THIRD_PARTY_DIRECTORY%\libdatachannel-0.24.2\libdatachannel.dll" "%ROOT_DIRECTORY%\..\buildresult\"
if errorlevel 1 goto deployment_failed
copy /Y "%ROOT_DIRECTORY%\main\chat-server.exe" "%ROOT_DIRECTORY%\..\buildresult\"
if errorlevel 1 goto deployment_failed

:: the page the bundled http server serves (built above)
if exist "%ROOT_DIRECTORY%\..\client-source-code\client.html" (
    copy /Y "%ROOT_DIRECTORY%\..\client-source-code\client.html" "%ROOT_DIRECTORY%\..\buildresult\"
) else (
    echo   WARNING: client.html not found - the bundled http server will have no page to serve.
)

:: =============================================================================
:: TLS FRONT-END | Build and collect stunnel
:: =============================================================================

:: stunnel.exe and tstunnel.exe let the server also serve wss:// and https:// on Windows.
:: the build runs in Git for Windows' bash with its own perl (the perl modules OpenSSL
:: needs are vendored in third-party\stunnel\perl-extra-lib). without bash the stunnel
:: build is SKIPPED and the server still builds - it runs over ws:// without it.
if /i not "%BUILD_STUNNEL%"=="y" goto stunnel_skipped
echo.
echo building the bundled stunnel wss/https front-end...
call "%THIRD_PARTY_DIRECTORY%\stunnel\_bundle_and_build_windows.bat"
if errorlevel 2 goto stunnel_nobash
if errorlevel 1 goto stunnel_failed
if exist "%THIRD_PARTY_DIRECTORY%\stunnel\stunnel-5.75\bin\MGW32\stunnel.exe" (
    copy /Y ^
        "%THIRD_PARTY_DIRECTORY%\stunnel\stunnel-5.75\bin\MGW32\stunnel.exe" ^
        "%ROOT_DIRECTORY%\..\buildresult\stunnel.exe"
    if errorlevel 1 goto deployment_failed
)

:: the console build is what the server starts by preference (no tray icon, no window); the gui build above is the fallback
if exist "%THIRD_PARTY_DIRECTORY%\stunnel\stunnel-5.75\bin\MGW32\tstunnel.exe" (
    copy /Y ^
        "%THIRD_PARTY_DIRECTORY%\stunnel\stunnel-5.75\bin\MGW32\tstunnel.exe" ^
        "%ROOT_DIRECTORY%\..\buildresult\tstunnel.exe"
    if errorlevel 1 goto deployment_failed
)
goto stunnel_done

:stunnel_skipped
echo.
echo   Skipping stunnel and OpenSSL as selected at the start.
goto stunnel_done

:stunnel_nobash
echo   Git for Windows not found - the optional stunnel build was skipped.
goto stunnel_done

:stunnel_done
echo.
pause
exit /b 0

:: =============================================================================
:: ERROR HANDLING | Leave build outputs available for retry
:: =============================================================================

:build_failed
echo.
echo ERROR: The build failed. See the command output above.
pause
exit /b 1

:deployment_failed
echo.
echo ERROR: Could not update the server files in buildresult.
echo Stop chat-server.exe and its stunnel processes, then run this build again.
echo Running programs lock their EXE and DLL files.
echo The newly built files are still in their build directories.
pause
exit /b 1

:stunnel_failed
echo.
echo ERROR: The stunnel build failed. See the error above; existing stunnel files were not copied.
pause
exit /b 1

:: =============================================================================
:: HELPERS | Configure the current CMake project
:: =============================================================================

:configure_cmake
cmake -G Ninja . ^
    -DCMAKE_BUILD_TYPE=%BUILD_CONFIG% ^
    "-DCMAKE_MAKE_PROGRAM=%CMAKE_MAKE_PROGRAM%" ^
    "-DCMAKE_LINKER=%CMAKE_LINKER%" ^
    -DCMAKE_C_COMPILER="%CMAKE_C_COMPILER%" ^
    -DCMAKE_CXX_COMPILER="%CMAKE_CXX_COMPILER%" ^
    "-DCMAKE_C_COMPILER_AR=%CMAKE_C_COMPILER_AR%" ^
    "-DCMAKE_C_COMPILER_RANLIB=%CMAKE_C_COMPILER_RANLIB%" ^
    "-DCMAKE_C_FLAGS=%CMAKE_C_FLAGS%" ^
    "-DCMAKE_C_FLAGS_DEBUG=%CMAKE_C_FLAGS_DEBUG%" ^
    "-DCMAKE_C_FLAGS_RELEASE=%CMAKE_C_FLAGS_RELEASE%" ^
    "-DCMAKE_C_FLAGS_MINSIZEREL=%CMAKE_C_FLAGS_MINSIZEREL%" ^
    "-DCMAKE_C_FLAGS_RELWITHDEBINFO=%CMAKE_C_FLAGS_RELWITHDEBINFO%"
exit /b %errorlevel%
