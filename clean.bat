@echo off
setlocal EnableExtensions DisableDelayedExpansion
where py >nul 2>nul
if errorlevel 1 goto use_python
py -3 "%~dp0tools\clean.py" %*
exit /b %errorlevel%

:use_python
where python >nul 2>nul
if errorlevel 1 goto missing_python
python "%~dp0tools\clean.py" %*
exit /b %errorlevel%

:missing_python
echo Python 3 is required to run the cleanup script. No files were removed.
exit /b 1
