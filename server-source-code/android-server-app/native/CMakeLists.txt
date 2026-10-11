cmake_minimum_required(VERSION 3.22)
project(lemon_android_server LANGUAGES C CXX)
if(NOT ANDROID)
    message(FATAL_ERROR "Use the Android NDK toolchain (or build through Gradle).")
endif()

get_filename_component(SERVER "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(THIRD "${SERVER}/third-party")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_DEPS_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_MEDIA ON CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory("${THIRD}/mbedtls-3.6.6" mbedtls EXCLUDE_FROM_ALL)
add_library(MbedTLS::MbedTLS INTERFACE IMPORTED GLOBAL)
set_target_properties(MbedTLS::MbedTLS PROPERTIES INTERFACE_LINK_LIBRARIES "mbedtls;mbedx509;mbedcrypto")
add_subdirectory("${THIRD}/libdatachannel-0.24.2" datachannel EXCLUDE_FROM_ALL)
add_subdirectory("${THIRD}/libtom/libtommath" tommath EXCLUDE_FROM_ALL)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
add_subdirectory("${THIRD}/libopus-1.5.2" opus EXCLUDE_FROM_ALL)

# Violet only contributes option parsing. Share datachannel's libjuice instead
# of linking two copies of its ICE/STUN implementation into one executable.
add_library(android_violet STATIC
    "${THIRD}/libviolet-0.5.4/src/options.c"
    "${THIRD}/libviolet-0.5.4/src/utils.c")
target_link_libraries(android_violet PUBLIC LibJuice::LibJuiceStatic)
target_include_directories(android_violet PRIVATE "${THIRD}/libdatachannel-0.24.2/deps/libjuice/include")

# The vendored MaxMind library uses autotools on desktop. Its two portable
# sources need no configure-generated host config on Android (API 26+).
add_library(android_maxminddb STATIC
    "${THIRD}/libmaxminddb-1.12.2/src/maxminddb.c"
    "${THIRD}/libmaxminddb-1.12.2/src/data-pool.c")
target_include_directories(android_maxminddb PUBLIC "${THIRD}/libmaxminddb-1.12.2/include")
target_compile_definitions(android_maxminddb PUBLIC MMDB_UINT128_IS_BYTE_ARRAY=1)
target_compile_definitions(android_maxminddb PRIVATE PACKAGE_VERSION="1.12.2")

add_subdirectory("${SERVER}/main" server)
target_sources(chat-server PRIVATE "${CMAKE_CURRENT_LIST_DIR}/android_main.c")
set_target_properties(chat-server PROPERTIES
    OUTPUT_NAME "libchat-server" SUFFIX ".so" LINKER_LANGUAGE CXX)
if(SERVER_OUTPUT_DIR)
    set_target_properties(chat-server PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${SERVER_OUTPUT_DIR}")
endif()
target_link_options(chat-server PRIVATE "-Wl,-z,max-page-size=16384")
