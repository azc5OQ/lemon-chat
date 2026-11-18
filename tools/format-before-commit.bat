@echo off
rem Run this before every commit
rem See docs/committing.md
setlocal EnableExtensions DisableDelayedExpansion
where py >nul 2>nul
if errorlevel 1 goto use_python
py -3 "%~dp0format_codebase.py" --before-commit %*
goto finished

:use_python
where python >nul 2>nul
if errorlevel 1 goto missing_python
python "%~dp0format_codebase.py" --before-commit %*
goto finished

:missing_python
echo Python 3 is required to format the sources. No files were changed.
set "SCRIPT_EXIT_CODE=1"
goto ended

:finished
set "SCRIPT_EXIT_CODE=%ERRORLEVEL%"

:ended
if "%~1"=="" pause
exit /b %SCRIPT_EXIT_CODE%
