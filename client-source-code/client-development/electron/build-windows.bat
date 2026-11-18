@echo off
rem Builds client.html and the Electron app for Windows, client-electron-amd64.exe, client-electron-arm64.exe and
rem client-electron-i386.exe in buildresult. It runs the client build in ..\browser with --electron, which takes the
rem same options, for example --minified.
setlocal
call "%~dp0..\browser\build-windows.bat" --electron %*
set "BUILD_EXIT_CODE=%ERRORLEVEL%"
if "%~1"=="" if not defined LEMON_CLIENT_BUILD_MODE pause
exit /b %BUILD_EXIT_CODE%
