@echo off
rem builds bundle.js, the chat client for Node.js on Android
rem run this after changing anything under ..\..\browser\main
cd /d "%~dp0"
python build-node.py
if errorlevel 1 pause
