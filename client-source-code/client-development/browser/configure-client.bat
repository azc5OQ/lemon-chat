@echo off
setlocal

:: starts tools\configure-client.py with Python 3.8 or newer
:: without options the script asks its questions; with options it asks nothing
:: example: configure-client.bat --help

:: find a working python
set "PYTHON_COMMAND="
call :try_python python
call :try_python py -3
call :try_python python3
if not defined PYTHON_COMMAND goto python_missing

:: run the script
%PYTHON_COMMAND% "%~dp0tools\configure-client.py" %*
set "CONFIGURE_EXIT_CODE=%ERRORLEVEL%"

:: keep the window open after a double click
if "%~1"=="" pause
exit /b %CONFIGURE_EXIT_CODE%

:python_missing
echo [!] Python 3.8 or newer was not found.
echo     Install it from https://www.python.org/downloads/ and start this file again.
if "%~1"=="" pause
exit /b 1

:try_python
:: running code refuses Python 2, old releases and the Store alias that has no interpreter
if defined PYTHON_COMMAND exit /b 0
%* -c "import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)" >nul 2>nul
if not errorlevel 1 set "PYTHON_COMMAND=%*"
exit /b 0
