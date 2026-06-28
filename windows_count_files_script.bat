@echo off
cls
setlocal EnableDelayedExpansion

call :count c
call :count h
call :count cpp
call :count hpp
call :count o
call :count obj
call :count dts
call :count dtsi
call :count S
call :count rst
call :count txt
call :count yaml
call :count rs
call :count sh
call :count csv
call :count py
call :count conf
call :count config
call :count gitignore
call :count asn1
call :count js
call :count html
call :count css

echo.

set "no_ext=0"
for /r %%f in (*) do if "%%~xf"=="" set /a no_ext+=1
echo files without extension count !no_ext!

set "all=0"
for /f %%a in ('dir /s /b /a-d 2^>nul ^| find /c /v ""') do set "all=%%a"
echo all files count !all!

pause
goto :eof

:count
set "n=0"
for /f %%a in ('dir /s /b *.%~1 2^>nul ^| find /c /v ""') do set "n=%%a"
echo .%~1 file count !n!
goto :eof