@echo off
cls
setlocal EnableDelayedExpansion

:: Delete any existing 'files_to_process.txt' if it exists
del files_to_process.txt

:: Generate a list of all .h, .c, .cpp, .hpp files in the current directory and subdirectories
:: and save the list to 'files_to_process.txt'
dir /s /b *.h *.c *.cpp *.hpp > files_to_process.txt

:: Count the total number of files to be processed by searching for the lines in the text file
set "cmd=findstr /R /N "^^" files_to_process.txt | find /C ":""
for /f %%a in ('!cmd!') do set total_files=%%a

:: Initialize the processed files counter, starting at 1
set processed_files=1

:: Loop through each file listed in 'files_to_process.txt'
for /f "delims=" %%f in (files_to_process.txt) do (
    :: Increment the processed files counter
    set /a processed_files=processed_files+1
    :: Display progress: Processed file / Total files
    echo !processed_files! / %total_files% : %%f
    
    :: Run clang-format in the background (-B) on the current file
    start /B clang-format -style=file -i "%%f"
)

:while_loop_beginning

:: Capture the number of running 'clang-format' processes
::for /f %%a in ('tasklist /FI "IMAGENAME eq clang-format.exe" 2>NUL ^| find /I /C "clang-format.exe"') do set count_clangformat_still_running=%%a
for /f "tokens=1" %%a in ('tasklist /FI "IMAGENAME eq clang-format.exe" 2^>NUL ^| find /I /C "clang-format.exe"') do set count_clangformat_still_running=%%a

:: Print how many 'clang-format' processes are still running
echo "still running !count_clangformat_still_running!"

:: If there are still 'clang-format' processes running, loop again
if !count_clangformat_still_running! NEQ 0 (
    goto :while_loop_beginning
)

:: Wait for user input before exiting the script (e.g., to see final status)
pause

:: Delete the 'files_to_process.txt' file after all processes are complete
del /q /s files_to_process.txt
