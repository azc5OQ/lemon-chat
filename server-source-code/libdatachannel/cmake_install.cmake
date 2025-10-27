# Install script for directory: C:/Users/retur/Desktop/final/server-source-code/libdatachannel

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/Program Files (x86)/libdatachannel")
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

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "C:/software/w64devkit/bin/objdump.exe")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY OPTIONAL FILES "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/libdatachannel.dll.a")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE SHARED_LIBRARY FILES "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/libdatachannel.dll")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libdatachannel.dll" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libdatachannel.dll")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "C:/software/w64devkit/bin/strip.exe" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libdatachannel.dll")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/rtc" TYPE FILE FILES
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/candidate.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/channel.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/configuration.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/datachannel.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/dependencydescriptor.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/description.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/iceudpmuxlistener.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/mediahandler.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/rtcpreceivingsession.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/common.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/global.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/message.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/frameinfo.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/peerconnection.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/reliability.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/rtc.h"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/rtc.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/rtp.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/track.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/websocket.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/websocketserver.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/rtppacketizationconfig.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/rtcpsrreporter.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/rtppacketizer.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/rtpdepacketizer.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/h264rtppacketizer.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/h264rtpdepacketizer.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/nalunit.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/h265rtppacketizer.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/h265rtpdepacketizer.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/h265nalunit.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/av1rtppacketizer.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/rtcpnackresponder.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/utils.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/plihandler.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/pacinghandler.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/rembhandler.hpp"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/include/rtc/version.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/LibDataChannel/LibDataChannelTargets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/LibDataChannel/LibDataChannelTargets.cmake"
         "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/CMakeFiles/Export/32c821eb1e7b36c3a3818aec162f7fd2/LibDataChannelTargets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/LibDataChannel/LibDataChannelTargets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/LibDataChannel/LibDataChannelTargets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/LibDataChannel" TYPE FILE FILES "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/CMakeFiles/Export/32c821eb1e7b36c3a3818aec162f7fd2/LibDataChannelTargets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/LibDataChannel" TYPE FILE FILES "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/CMakeFiles/Export/32c821eb1e7b36c3a3818aec162f7fd2/LibDataChannelTargets-release.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/LibDataChannel" TYPE FILE FILES
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/LibDataChannelConfig.cmake"
    "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/LibDataChannelConfigVersion.cmake"
    )
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.

endif()

if(CMAKE_INSTALL_COMPONENT)
  set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
file(WRITE "C:/Users/retur/Desktop/final/server-source-code/libdatachannel/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
