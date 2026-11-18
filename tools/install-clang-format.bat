@echo off
rem Downloads the clang-format version that format_codebase.py uses into this folder, without formatting
rem any file.
setlocal EnableExtensions DisableDelayedExpansion
where py >nul 2>nul
if errorlevel 1 goto use_python
py -3 "%~dp0format_codebase.py" --install %*
goto finished

:use_python
where python >nul 2>nul
if errorlevel 1 goto missing_python
python "%~dp0format_codebase.py" --install %*
goto finished

:missing_python
echo Python 3 is required to download clang-format. No files were changed.
set "SCRIPT_EXIT_CODE=1"
goto ended

:finished
set "SCRIPT_EXIT_CODE=%ERRORLEVEL%"

:ended
if "%~1"=="" pause
exit /b %SCRIPT_EXIT_CODE%
