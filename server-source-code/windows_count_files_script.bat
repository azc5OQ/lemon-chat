@echo off
cls
setlocal EnableDelayedExpansion


dir /s /b *.c > c_file_count.txt
set "cmd=findstr /R /N "^^" c_file_count.txt | find /C ":""
for /f %%a in ('!cmd!') do set c_file_count=%%a
echo .c file count %c_file_count%
del c_file_count.txt

dir /s /b *.h > h_file_count.txt
set "cmd=findstr /R /N "^^" h_file_count.txt | find /C ":""
for /f %%a in ('!cmd!') do set h_file_count=%%a
echo .h file count %h_file_count%
del h_file_count.txt

dir /s /b *.cpp > cpp_file_count.txt
set "cmd=findstr /R /N "^^" cpp_file_count.txt | find /C ":""
for /f %%a in ('!cmd!') do set cpp_file_count=%%a
echo .cpp file count %cpp_file_count%
del cpp_file_count.txt

dir /s /b *.hpp > hpp_file_count.txt
set "cmd=findstr /R /N "^^" hpp_file_count.txt | find /C ":""
for /f %%a in ('!cmd!') do set hpp_file_count=%%a
echo .hpp file count %hpp_file_count%
del hpp_file_count.txt

dir /s /b *.o > hpp_file_count.txt
set "cmd=findstr /R /N "^^" hpp_file_count.txt | find /C ":""
for /f %%a in ('!cmd!') do set hpp_file_count=%%a
echo .o file count %hpp_file_count%
del hpp_file_count.txt

dir /s /b *.obj > hpp_file_count.txt
set "cmd=findstr /R /N "^^" hpp_file_count.txt | find /C ":""
for /f %%a in ('!cmd!') do set hpp_file_count=%%a
echo .obj file count %hpp_file_count%
del hpp_file_count.txt

dir /s /b *.dts > dts_file_counts.txt
set "cmd=findstr /R /N "^^" dts_file_counts.txt | find /C ":""
for /f %%a in ('!cmd!') do set dts_file_counts=%%a
echo .dts file count %dts_file_counts%
del dts_file_counts.txt

dir /s /b *.dtsi > dtsi_file_counts.txt
set "cmd=findstr /R /N "^^" dtsi_file_counts.txt | find /C ":""
for /f %%a in ('!cmd!') do set dtsi_file_counts=%%a
echo .dtsi file count %dtsi_file_counts%
del dtsi_file_counts.txt

dir /s /b *.S > assembly_file_count.txt
set "cmd=findstr /R /N "^^" assembly_file_count.txt | find /C ":""
for /f %%a in ('!cmd!') do set assembly_file_count=%%a
echo .S file count %assembly_file_count%
del assembly_file_count.txt

dir /s /b *.rst > rst_file_count.txt
set "cmd=findstr /R /N "^^" rst_file_count.txt | find /C ":""
for /f %%a in ('!cmd!') do set rst_file_count=%%a
echo .rst file count %rst_file_count%
del rst_file_count.txt

dir /s /b *.txt > txt_file_count.txt
set "cmd=findstr /R /N "^^" txt_file_count.txt | find /C ":""
for /f %%a in ('!cmd!') do set txt_file_count=%%a
echo .txt file count %txt_file_count%
del txt_file_count.txt

dir /s /b *.yaml > yaml_file_count.txt
set "cmd=findstr /R /N "^^" yaml_file_count.txt | find /C ":""
for /f %%a in ('!cmd!') do set yaml_file_count=%%a
echo .yaml file count %yaml_file_count%
del yaml_file_count.txt



dir /s /b *.rs > rs_file_count.txt
set "cmd=findstr /R /N "^^" rs_file_count.txt | find /C ":""
for /f %%a in ('!cmd!') do set rs_file_count=%%a
echo .rs file count %rs_file_count%
del rs_file_count.txt


dir /s /b *.sh > sh_file_count.txt
set "cmd=findstr /R /N "^^" sh_file_count.txt | find /C ":""
for /f %%a in ('!cmd!') do set sh_file_count=%%a
echo .sh file count %sh_file_count%
del sh_file_count.txt


dir /s /b *.csv > csv_file_count.txt
set "cmd=findstr /R /N "^^" csv_file_count.txt | find /C ":""
for /f %%a in ('!cmd!') do set csv_file_count=%%a
echo .csv file count %csv_file_count%
del csv_file_count.txt


dir /s /b *.py > py_file_count.txt
set "cmd=findstr /R /N "^^" py_file_count.txt | find /C ":""
for /f %%a in ('!cmd!') do set py_file_count=%%a
echo .py file count %py_file_count%
del py_file_count.txt

dir /s /b *.conf > conf_file_count.txt
set "cmd=findstr /R /N "^^" conf_file_count.txt | find /C ":""
for /f %%a in ('!cmd!') do set conf_file_count=%%a
echo .conf file count %conf_file_count%
del conf_file_count.txt


dir /s /b *.config > config_file_count.txt
set "cmd=findstr /R /N "^^" config_file_count.txt | find /C ":""
for /f %%a in ('!cmd!') do set config_file_count=%%a
echo .config file count %config_file_count%
del config_file_count.txt

dir /s /b *.gitignore > gitignore_file_count.txt
set "cmd=findstr /R /N "^^" gitignore_file_count.txt | find /C ":""
for /f %%a in ('!cmd!') do set gitignore_file_count=%%a
echo .gitignore file count %gitignore_file_count%
::del gitignore_file_count.txt


dir /s /b *.asn1 > asn1_file_count.txt
set "cmd=findstr /R /N "^^" asn1_file_count.txt | find /C ":""
for /f %%a in ('!cmd!') do set asn1_file_count=%%a
echo .asn1 file count %asn1_file_count%
del asn1_file_count.txt


for /r %%f in (*) do (
    if "%%~xf"=="" (
        echo %%f >> no_extension_files.txt
    )
)
for /f "usebackq" %%b in (`type no_extension_files.txt ^| find "" /v /c`) do (
    echo files without extension count %%b
)
del no_extension_files.txt




dir /s /b * | findstr /R /E /C:".*" > all_files.txt
set "cmd=findstr /R /N "^^" all_files.txt | find /C ":""
for /f %%a in ('!cmd!') do set all_files_count=%%a
echo all files count %all_files_count%
del all_files.txt

pause