@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build.ps1" %*
set "BUILD_EXIT_CODE=%ERRORLEVEL%"
if "%~1"=="" if not defined LEMON_CLIENT_BUILD_MODE pause
exit /b %BUILD_EXIT_CODE%
