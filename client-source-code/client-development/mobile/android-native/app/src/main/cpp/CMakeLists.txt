cmake_minimum_required(VERSION 3.22.1)
project(lemonnative LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
get_filename_component(LEMON_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../../.." ABSOLUTE)

if(NOT EXISTS "${LEMON_ROOT}/client-source-code/client-development/desktop/main/crypto/crypto_session.c" OR NOT EXISTS "${LEMON_ROOT}/server-source-code/main-server/third-party/libopus-1.6.1/CMakeLists.txt")
    message(FATAL_ERROR "Open android-native inside the complete lemon-chat checkout. Shared crypto and Opus sources are required and are built automatically.")
endif()

add_library(lemonnative SHARED)
include(crypto.cmake)
include(audio.cmake)
include(transport.cmake)
target_compile_options(lemonnative PRIVATE -Os -ffunction-sections -fdata-sections)
target_link_options(lemonnative PRIVATE -Wl,--gc-sections)
target_link_libraries(lemonnative PRIVATE android log)
