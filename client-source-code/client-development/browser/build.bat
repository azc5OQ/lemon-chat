@echo off
rem Glue the split sources in src\ back into the shipped ..\..\client.html (client-source-code\client.html)
python "%~dp0build.py" %*
echo.
pause
