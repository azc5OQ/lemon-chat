@echo off
rem builds bundle.js from the client sources; run after changing anything under ..\browser\src
cd /d "%~dp0"
python build-node.py
if errorlevel 1 pause
