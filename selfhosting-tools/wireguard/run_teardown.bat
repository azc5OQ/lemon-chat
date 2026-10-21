@echo off
setlocal
:: lemon-chat selfhosting-tools\wireguard\run_teardown.bat - double-click this to remove the WireGuard setup (teardown.ps1)
:: on this machine and on the router. it asks for administrator rights; the window stays open.

cd /d "%~dp0"

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo requesting administrator rights ...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

:: Bypass applies only to this PowerShell process; no saved policy is changed.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0teardown.ps1"

echo.
pause
