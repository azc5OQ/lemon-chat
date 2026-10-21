@echo off
setlocal
:: lemon-chat selfhosting-tools\wireguard\run_status.bat - double-click this to show the tunnel state (status.ps1).
:: administrator rights are needed to read the tunnel on this machine; the window stays open.

cd /d "%~dp0"

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo requesting administrator rights ...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

:: Bypass applies only to this PowerShell process; no saved policy is changed.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0status.ps1"

echo.
pause
