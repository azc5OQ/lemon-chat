@echo off
setlocal EnableExtensions DisableDelayedExpansion

:: Build and package the native desktop client without an IDE.
:: MINGWPATH may point at a w64devkit/MinGW bin directory.
:: LEMON_DESKTOP_BUILD_DIR and CMAKE_BUILD_PARALLEL_LEVEL override the defaults.
set "DESKTOP_SOURCE=%~dp0"
set "DESKTOP_CONFIG=RelWithDebInfo"
set "DESKTOP_TESTS=OFF"
set "DESKTOP_CLEAN_MODE=ask"
set "DESKTOP_TOOL_MODE=ask"
if not defined LEMON_DESKTOP_BUILD_DIR set "LEMON_DESKTOP_BUILD_DIR=%DESKTOP_SOURCE%build"
if not defined CMAKE_BUILD_PARALLEL_LEVEL for /f %%J in ('powershell.exe -NoProfile -Command "[Environment]::ProcessorCount"') do set "CMAKE_BUILD_PARALLEL_LEVEL=%%J"
if not defined CMAKE_BUILD_PARALLEL_LEVEL set "CMAKE_BUILD_PARALLEL_LEVEL=1"

:arguments
if "%~1"=="" goto tools
if /i "%~1"=="--help" goto help
if /i "%~1"=="-h" goto help
if /i "%~1"=="Debug" goto debug
if /i "%~1"=="Release" goto release
if /i "%~1"=="RelWithDebInfo" goto optimized_debug
if /i "%~1"=="--test" goto tests
if /i "%~1"=="--clean" goto clean
if /i "%~1"=="--keep-build" goto keep_build
if /i "%~1"=="--download-tools" goto download_tools
if /i "%~1"=="--no-download" goto no_download
echo ERROR: Unknown option: %1
exit /b 2

:debug
set "DESKTOP_CONFIG=Debug"
shift
goto arguments

:release
set "DESKTOP_CONFIG=Release"
shift
goto arguments

:optimized_debug
set "DESKTOP_CONFIG=RelWithDebInfo"
shift
goto arguments

:tests
set "DESKTOP_TESTS=ON"
shift
goto arguments

:clean
set "DESKTOP_CLEAN_MODE=clean"
shift
goto arguments

:keep_build
set "DESKTOP_CLEAN_MODE=keep"
shift
goto arguments

:tools
if "%DESKTOP_TESTS%"=="ON" if not exist "%DESKTOP_SOURCE%checks\attachments_check.c" goto missing_tests
set "DESKTOP_TOOLS_FILE=%TEMP%\lemon-desktop-tools-%RANDOM%-%RANDOM%.txt"
set "DESKTOP_TEST_TOOL_OPTION="
if "%DESKTOP_TESTS%"=="ON" set "DESKTOP_TEST_TOOL_OPTION=-WithTests"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%DESKTOP_SOURCE%tools\build-tools.ps1" -EnvironmentFile "%DESKTOP_TOOLS_FILE%" -Mode %DESKTOP_TOOL_MODE% %DESKTOP_TEST_TOOL_OPTION%
if errorlevel 1 exit /b 1
set "DESKTOP_TOOL_PATH="
set /p DESKTOP_TOOL_PATH=<"%DESKTOP_TOOLS_FILE%"
del /q "%DESKTOP_TOOLS_FILE%" >nul 2>nul
if not defined DESKTOP_TOOL_PATH exit /b 1
set "PATH=%DESKTOP_TOOL_PATH%;%PATH%"
goto cmake

:download_tools
set "DESKTOP_TOOL_MODE=download"
shift
goto arguments

:no_download
set "DESKTOP_TOOL_MODE=off"
shift
goto arguments

:cmake
where cmake >nul 2>nul
if errorlevel 1 goto missing_cmake
:: Make relative override paths independent of the caller's current directory.
pushd "%DESKTOP_SOURCE%"
if errorlevel 1 exit /b 1
for %%D in ("%LEMON_DESKTOP_BUILD_DIR%") do set "DESKTOP_BUILD=%%~fD"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%DESKTOP_SOURCE%tools\clean-build.ps1" -SourceDirectory "%DESKTOP_SOURCE%." -BuildDirectory "%DESKTOP_BUILD%" -Mode %DESKTOP_CLEAN_MODE%
if errorlevel 1 goto failed
set "DESKTOP_GENERATOR="
if exist "%DESKTOP_BUILD%\CMakeCache.txt" goto configure
if defined CMAKE_GENERATOR goto configure
where ninja >nul 2>nul
if not errorlevel 1 goto ninja
where mingw32-make >nul 2>nul
if not errorlevel 1 goto mingw
:: Without Ninja or MinGW make, let CMake find installed Visual Studio Build Tools.
goto configure

:ninja
set "DESKTOP_GENERATOR=-G Ninja"
goto configure

:mingw
set "DESKTOP_GENERATOR=-G "MinGW Makefiles""

:configure
echo Configuring desktop %DESKTOP_CONFIG% with checks %DESKTOP_TESTS%, using %CMAKE_BUILD_PARALLEL_LEVEL% build jobs...
cmake -S "%DESKTOP_SOURCE%." -B "%DESKTOP_BUILD%" %DESKTOP_GENERATOR% -DCMAKE_BUILD_TYPE=%DESKTOP_CONFIG% -DBUILD_TESTING=%DESKTOP_TESTS% -DCMAKE_POLICY_VERSION_MINIMUM=3.5
if errorlevel 1 goto failed
cmake --build "%DESKTOP_BUILD%" --config %DESKTOP_CONFIG% --parallel "%CMAKE_BUILD_PARALLEL_LEVEL%"
if errorlevel 1 goto failed
if "%DESKTOP_TESTS%"=="OFF" goto package
ctest --test-dir "%DESKTOP_BUILD%" -C %DESKTOP_CONFIG% --output-on-failure --parallel 1
if errorlevel 1 goto failed

:package
cmake --install "%DESKTOP_BUILD%" --config %DESKTOP_CONFIG% --prefix "%DESKTOP_BUILD%\package"
if errorlevel 1 goto failed
echo.
echo Built: "%DESKTOP_BUILD%\package\lemon-chat-desktop.exe"
echo Keep the shared libraries, assets and fonts beside the executable.
popd
exit /b 0

:failed
echo.
echo Desktop build failed. See the first error above.
echo A C11 toolchain and CMake 3.22 or newer are required.
echo Use w64devkit/MinGW on PATH, set MINGWPATH, or use a Visual Studio developer command prompt.
popd
exit /b 1

:missing_cmake
echo ERROR: CMake 3.22 or newer is required. Install CMake and add its bin directory to PATH.
exit /b 1

:missing_tests
echo ERROR: The local desktop checks are absent. They are intentionally ignored by Git.
echo Build without --test, or restore your local checks directory to run them.
exit /b 1

:help
echo Usage: build.bat [Debug^|Release^|RelWithDebInfo] [--test] [--clean^|--keep-build] [--download-tools^|--no-download]
echo.
echo Default: optimized build with debug symbols, one compiler job per detected logical CPU, no tests.
echo Output: build\package\lemon-chat-desktop.exe and its shared libraries/assets.
echo --test runs the small desktop checks, including an owned D3D11 fixture.
echo Existing builds prompt to clean all generated files: Y/n, default Yes.
echo --clean cleans without prompting; --keep-build keeps the incremental build.
echo Missing tools offer a verified portable download. No admin installation is needed.
echo --download-tools accepts that download; --no-download only uses installed tools.
echo Redirected input keeps build files unless --clean is explicitly supplied.
echo --test also offers Node.js if missing; normal client builds do not need Node.js.
echo.
echo Optional environment variables:
echo   MINGWPATH                   Compiler bin directory
echo   LEMON_DESKTOP_BUILD_DIR     Build directory, relative to this script or absolute
echo   CMAKE_BUILD_PARALLEL_LEVEL  Overrides the detected logical CPU count
echo   CMAKE_GENERATOR            Generator for a new build directory
exit /b 0
