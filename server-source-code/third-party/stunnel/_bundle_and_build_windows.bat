@echo off
setlocal

:: =============================================================================
:: SHELL | Find Git for Windows instead of a different bash.exe on PATH
:: =============================================================================

:: Select Git's shell explicitly. w64devkit and WSL also provide bash.exe, but
:: neither supplies the MSYS Perl required by this OpenSSL build.
set "STUNNEL_GIT_ROOT="
for /f "delims=" %%G in ('where git.exe 2^>nul') do (
    call :try_git_root "%%~dpG.."
    call :try_git_root "%%~dpG..\.."
)
if not defined STUNNEL_GIT_ROOT call :try_git_root "%ProgramFiles%\Git"
if not defined STUNNEL_GIT_ROOT call :try_git_root "%ProgramFiles(x86)%\Git"
if not defined STUNNEL_GIT_ROOT call :try_git_root "%LocalAppData%\Programs\Git"
if not defined STUNNEL_GIT_ROOT (
    echo ERROR: Git for Windows with Bash and Perl was not found.
    echo Install Git for Windows or add its cmd directory to PATH.
    exit /b 2
)
:: =============================================================================
:: BUILD | Forward the toolchain and arguments to the shared shell script
:: =============================================================================

if not defined W64DEVKIT if defined MINGWPATH set "W64DEVKIT=%MINGWPATH%"
echo Using Git Bash: "%STUNNEL_GIT_ROOT%\bin\bash.exe"
"%STUNNEL_GIT_ROOT%\bin\bash.exe" --noprofile --norc "%~dp0_bundle_and_build_windows.sh" %*
if errorlevel 1 exit /b 1
exit /b 0

:: =============================================================================
:: HELPER | Accept only an installation containing both Git Bash and MSYS Perl
:: =============================================================================

:try_git_root
if defined STUNNEL_GIT_ROOT exit /b 0
if not exist "%~1\bin\bash.exe" exit /b 0
if not exist "%~1\usr\bin\perl.exe" exit /b 0
for %%R in ("%~1") do set "STUNNEL_GIT_ROOT=%%~fR"
exit /b 0
