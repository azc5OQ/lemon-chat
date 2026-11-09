@echo off
setlocal EnableExtensions DisableDelayedExpansion
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\..\tools\format-source.ps1" -SourceDirectory "%~dp0." %*
exit /b %errorlevel%
