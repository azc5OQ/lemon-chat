@echo off
node "%~dp0build.cjs" %*
exit /b %ERRORLEVEL%
