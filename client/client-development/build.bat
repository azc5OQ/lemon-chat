@echo off
rem Glue the split sources in src\ back into src\client-build.html
python "%~dp0build.py" %*
echo.
pause
