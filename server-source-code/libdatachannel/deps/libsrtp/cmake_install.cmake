# Install script for directory: /home/user/Desktop/lemonchat-jun-2025/server-source-code/libdatachannel/deps/libsrtp

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "0")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/sbin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/home/user/Desktop/lemonchat-jun-2025/server-source-code/libdatachannel/deps/libsrtp/libsrtp2.a")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  include("/home/user/Desktop/lemonchat-jun-2025/server-source-code/libdatachannel/deps/libsrtp/CMakeFiles/srtp2.dir/install-cxx-module-bmi-Release.cmake" OPTIONAL)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/srtp2" TYPE FILE FILES
    "/home/user/Desktop/lemonchat-jun-2025/server-source-code/libdatachannel/deps/libsrtp/include/srtp.h"
    "/home/user/Desktop/lemonchat-jun-2025/server-source-code/libdatachannel/deps/libsrtp/crypto/include/auth.h"
    "/home/user/Desktop/lemonchat-jun-2025/server-source-code/libdatachannel/deps/libsrtp/crypto/include/cipher.h"
    "/home/user/Desktop/lemonchat-jun-2025/server-source-code/libdatachannel/deps/libsrtp/crypto/include/crypto_types.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/libSRTP/libSRTPTargets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/libSRTP/libSRTPTargets.cmake"
         "/home/user/Desktop/lemonchat-jun-2025/server-source-code/libdatachannel/deps/libsrtp/CMakeFiles/Export/4057af6c16ac9b4df3236859d5c9bdc7/libSRTPTargets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/libSRTP/libSRTPTargets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/libSRTP/libSRTPTargets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/libSRTP" TYPE FILE FILES "/home/user/Desktop/lemonchat-jun-2025/server-source-code/libdatachannel/deps/libsrtp/CMakeFiles/Export/4057af6c16ac9b4df3236859d5c9bdc7/libSRTPTargets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/libSRTP" TYPE FILE FILES "/home/user/Desktop/lemonchat-jun-2025/server-source-code/libdatachannel/deps/libsrtp/CMakeFiles/Export/4057af6c16ac9b4df3236859d5c9bdc7/libSRTPTargets-release.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/libSRTP" TYPE FILE FILES
    "/home/user/Desktop/lemonchat-jun-2025/server-source-code/libdatachannel/deps/libsrtp/libSRTPConfig.cmake"
    "/home/user/Desktop/lemonchat-jun-2025/server-source-code/libdatachannel/deps/libsrtp/libSRTPConfigVersion.cmake"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/home/user/Desktop/lemonchat-jun-2025/server-source-code/libdatachannel/deps/libsrtp/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
