@echo off
rem Formats the C, C++ and Java sources of server-source-code, client-source-code or both with clang-format and
rem the own rules in .clang-format of this folder. It asks which part to format and offers to download
rem clang-format when it is missing. Options of format_codebase.py can be added, for example --check.
rem Before every commit, run format-before-commit, which restores the project rules.
setlocal EnableExtensions DisableDelayedExpansion
where py >nul 2>nul
if errorlevel 1 goto use_python
py -3 "%~dp0format_codebase.py" %*
goto finished

:use_python
where python >nul 2>nul
if errorlevel 1 goto missing_python
python "%~dp0format_codebase.py" %*
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
