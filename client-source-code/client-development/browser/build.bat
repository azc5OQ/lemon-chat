@echo off
setlocal
where node >nul 2>nul
if errorlevel 1 (
    echo Node.js was not found. Install it from https://nodejs.org/ and reopen this terminal.
    echo Minified builds additionally need: npm install --global terser
    exit /b 1
)
if not "%~1"=="" goto explicit_options
if defined LEMON_CLIENT_BUILD_MODE goto explicit_options

node "%~dp0build.cjs" --check-minifier >nul 2>nul
if errorlevel 1 goto without_minifier
echo 1. Minified
echo 2. Unminified - readable JavaScript and CSS
choice /c 12 /n /m "Choose [1/2]: "
if errorlevel 3 exit /b 1
if errorlevel 2 goto readable
if errorlevel 1 goto minified
exit /b 1

:without_minifier
echo 1. Minified [disabled: Terser is not installed or cannot run]
echo 2. Unminified - readable JavaScript and CSS
echo To enable minification: npm install --global terser
choice /c 2 /n /m "Choose [2]: "
if errorlevel 2 exit /b 1
if errorlevel 1 goto readable
exit /b 1

:minified
node "%~dp0build.cjs" --minified
goto finished

:readable
node "%~dp0build.cjs" --unminified
goto finished

:explicit_options
node "%~dp0build.cjs" %*
goto finished

:finished
set "BUILD_EXIT_CODE=%ERRORLEVEL%"
if "%~1"=="" if not defined LEMON_CLIENT_BUILD_MODE pause
exit /b %BUILD_EXIT_CODE%
