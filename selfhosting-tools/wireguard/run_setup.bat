@echo off
setlocal
:: lemon-chat selfhosting-tools\wireguard\run_setup.bat - double-click this to run the WireGuard setup (setup.ps1).
:: it asks Windows for administrator rights (the tunnel service needs them), runs the setup in a
:: PowerShell window that stays open, and never needs the execution policy changed.

cd /d "%~dp0"

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo requesting administrator rights for the setup ...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

:: Bypass applies only to this PowerShell process; no saved policy is changed.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1"

echo.
echo (setup finished, this window can be closed)
pause
