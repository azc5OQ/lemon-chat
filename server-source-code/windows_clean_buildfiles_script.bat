@echo off


::list of libraries used in project

::mbedtls (.a)
::libtommath (.a)
::libtomcrypt (.a)
::libdatachannel (.dll)
::libviolet (.a)
::theldus-websocket (.a)
::opus (.a)
::lemonchat-server (.exe)

::there are a
::libraries cJSON , console logging library (log), kokke-tiny-aes and ITH-SHA are built into .exe

::note, when using w64devkit toolchain, what would be a .lib file has an ".a" extension like on linux, not .lib .. .dll stays .dll


::sets current working directory
set "ROOT_DIRECTORY=%~dp0"

set "THIRD_PARTY_DIRECTORY=%ROOT_DIRECTORY%third-party"

echo %THIRD_PARTY_DIRECTORY%

set /p choice= "clean files from previous build if any y/n: "

echo %choice%

IF /i "%choice%"=="Y" IF /i "%choice%"=="y" (
  
  ::delete any files that might be leftovers from previous build
  ::checking if they exist is not needed, would only waste space in .bat file

  rd "%THIRD_PARTY_DIRECTORY%\mbedtls-3.5.1\CMakeFiles"  /S /Q
  rd "%THIRD_PARTY_DIRECTORY%\mbedtls-3.5.1\library\CMakeFiles"  /S /Q
  del "%THIRD_PARTY_DIRECTORY%\mbedtls-3.5.1\CMakeCache.txt"

  del "%ROOT_DIRECTORY%\*.o" /S /Q
  del "%ROOT_DIRECTORY%\*.a" /S /Q
  del "%ROOT_DIRECTORY%\*.ninja_deps" /S /Q
  del "%ROOT_DIRECTORY%\*.ninja_log" /S /Q

  rd "%THIRD_PARTY_DIRECTORY%\libdatachannel\CMakeFiles"  /S /Q
  del "%THIRD_PARTY_DIRECTORY%\libdatachannel\CMakeCache.txt"
  del "%THIRD_PARTY_DIRECTORY%\libdatachannel\libdatachannel-static.a"
  del "%THIRD_PARTY_DIRECTORY%\libdatachannel\libdatachannel.dll"
  del "%THIRD_PARTY_DIRECTORY%\libdatachannel\libdatachannel.dll.a"

  del "%THIRD_PARTY_DIRECTORY%\libdatachannel\deps\mbedtls\lib\libmbedcrypto.a"
  del "%THIRD_PARTY_DIRECTORY%\libdatachannel\deps\mbedtls\lib\libmbedtls.a"
  del "%THIRD_PARTY_DIRECTORY%\libdatachannel\deps\mbedtls\lib\libmbedx509.a"

  rd "%THIRD_PARTY_DIRECTORY%\libtom\libtommath\cmake-build-release"   /S /Q
  rd "%THIRD_PARTY_DIRECTORY%\libtom\libtommath\CMakeFiles"   /S /Q
  del "%THIRD_PARTY_DIRECTORY%\libtom\libtommath\CMakeCache.txt"
  del "%THIRD_PARTY_DIRECTORY%\libtom\libtommath\libtommath.a"

  del "%THIRD_PARTY_DIRECTORY%\libtom\libtomcrypt\libtomcrypt.a"

  rd "%THIRD_PARTY_DIRECTORY%\theldus-websocket\CMakeFiles"  /S /Q
  del "%THIRD_PARTY_DIRECTORY%\theldus-websocket\CMakeCache.txt"

  rd "%THIRD_PARTY_DIRECTORY%\libviolet\CMakeFiles"  /S /Q
  del "%THIRD_PARTY_DIRECTORY%\libviolet\CMakeCache.txt"
  del "%THIRD_PARTY_DIRECTORY%\libviolet\libviolet.a"

  del "%THIRD_PARTY_DIRECTORY%\libmaxminddb-1.12.2\CMakeCache.txt"
  del "%THIRD_PARTY_DIRECTORY%\libmaxminddb-1.12.2\libmaxminddb.a"

  del "%THIRD_PARTY_DIRECTORY%\libopus-1.5.2\CMakeCache.txt"
  rd "%THIRD_PARTY_DIRECTORY%\libopus-1.5.2\CMakeFiles"  /S /Q
  del "%THIRD_PARTY_DIRECTORY%\libopus-1.5.2\libopus.a"

  del "%ROOT_DIRECTORY%\main\linkage-files\windows\*" /S /Q
  del "%ROOT_DIRECTORY%\main\linkage-files\linux\*" /S /Q

  rd "%ROOT_DIRECTORY%\main\CMakeFiles"  /S /Q
  del "%ROOT_DIRECTORY%\main\CMakeCache.txt"
  del "%ROOT_DIRECTORY%\main\cmake_install.cmake"
  del "%ROOT_DIRECTORY%\main\build.ninja"
  del "%ROOT_DIRECTORY%\main\.ninja_deps"
  del "%ROOT_DIRECTORY%\main\.ninja_log"
  del "%ROOT_DIRECTORY%\main\liblemon-chat-server.dll.a"
  del "%ROOT_DIRECTORY%\main\chat-server.exe"
  del "%ROOT_DIRECTORY%\main\chat-server"
  del "%ROOT_DIRECTORY%\main\cmake-build-debug"  /S /Q
  del "%ROOT_DIRECTORY%\main\.cmake"  /S /Q
  del "%ROOT_DIRECTORY%\main\.idea"  /S /Q
  del "%ROOT_DIRECTORY%\main\libchat-server.dll.a"
  del "%ROOT_DIRECTORY%\..\buildresult\*" /S /Q
	
  rd "%ROOT_DIRECTORY%\main\.cmake" /S /Q
  rd "%ROOT_DIRECTORY%\main\.idea" /S /Q

  rd "%ROOT_DIRECTORY%\main\cmake-build-debug" /S /Q
)
del all_files.txt
del no_extension_files.txt
del gitignore_file_count.txt

pause