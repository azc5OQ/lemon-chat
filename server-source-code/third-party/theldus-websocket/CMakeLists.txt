# Copyright (C) 2016-2022  Davidson Francis <davidsondfgl@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>

cmake_minimum_required(VERSION 3.5.2)
set(CMAKE_VERBOSE_MAKEFILE ON)

project(ws C)

get_filename_component(PROJECT_NAME ${CMAKE_CURRENT_SOURCE_DIR} NAME)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	message(STATUS "Configuring ${PROJECT_NAME} Linux")
	# Additional configurations for Linux
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	message(STATUS "Configuring ${PROJECT_NAME} for Windows")
	# Additional configurations or Windows
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	message(STATUS "Configuring ${PROJECT_NAME} for macOS")
	# Additional configurations for macOS
else()
	message(WARNING "Unsupported operating system: ${CMAKE_SYSTEM_NAME}")
endif()


set(CMAKE_C_STANDARD 99)

include_directories(include)

add_library(ws
    src/ws.c
    src/base64.c
    src/sha1.c
    src/handshake.c
    src/utf8.c
)

set(CMAKE_VERBOSE_MAKEFILE ON)

#set(CMAKE_STATIC_LINKER_FLAGS "${CMAKE_STATIC_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")

if(WIN32)
	target_link_libraries(ws pthread ws2_32 wsock32 -static)
else()
	target_link_libraries(ws pthread)
endif(WIN32)


option(VALIDATE_UTF8 "Enable UTF-8 validation (default ON)" ON)
if(VALIDATE_UTF8)
	target_compile_definitions(ws PRIVATE VALIDATE_UTF8)
endif(VALIDATE_UTF8)
