#set(CMAKE_VERBOSE_MAKEFILE ON)

cmake_minimum_required(VERSION 3.22)
project(lemon_chat C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Check the atomic types and operations used by the server and WebSocket library.
# Some CPUs need a separate library for operations on 64-bit values.
include(CheckCSourceCompiles)
include(CMakePushCheckState)
set(LEMON_ATOMIC_CHECK_SOURCE [=[
    #include <stdint.h>
    #include <stdatomic.h>

    typedef unsigned long long uint64;
    static _Atomic(uint64) packet_counter;
    static atomic_uint_fast64_t byte_counter;

    int main(void)
    {
        uint64 previous;

        atomic_init(&packet_counter, 1);
        previous = atomic_fetch_add_explicit(&packet_counter, 1, memory_order_relaxed);
        atomic_store_explicit(&packet_counter, previous, memory_order_relaxed);
        atomic_fetch_add_explicit(&byte_counter, previous, memory_order_relaxed);
        return atomic_load_explicit(&packet_counter, memory_order_relaxed) !=
            atomic_load_explicit(&byte_counter, memory_order_relaxed);
    }
]=])
cmake_push_check_state(RESET)
check_c_source_compiles("${LEMON_ATOMIC_CHECK_SOURCE}" LEMON_HAS_C11_ATOMICS)
set(LEMON_ATOMIC_LIBRARY "")
if(NOT LEMON_HAS_C11_ATOMICS)
    set(CMAKE_REQUIRED_LIBRARIES atomic)
    check_c_source_compiles("${LEMON_ATOMIC_CHECK_SOURCE}" LEMON_HAS_C11_ATOMICS_WITH_LIBRARY)
    if(NOT LEMON_HAS_C11_ATOMICS_WITH_LIBRARY)
        message(FATAL_ERROR
            "The server requires C11 atomics, including 64-bit counters. "
            "Use a compiler with <stdatomic.h> support and its atomic runtime library.")
    endif()
    set(LEMON_ATOMIC_LIBRARY atomic)
endif()
cmake_pop_check_state()
unset(LEMON_ATOMIC_CHECK_SOURCE)

# A direct CMake build should use the same optimized configuration as the scripts.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Server build configuration" FORCE)
endif()

option(LEMON_ENABLE_AESNI "Use CPU AES instructions when supported, with portable tiny-AES as fallback" ON)
option(LEMON_ENABLE_LTO "Optimize across source files in Release builds when the compiler supports it" ON)
option(LEMON_TRACK_ALLOCATIONS "Keep a list of live allocations for memory debugging in Release builds" OFF)
set_source_files_properties(../third-party/kokke-tiny-aes-c/aes.c
    PROPERTIES COMPILE_DEFINITIONS "TINY_AES_ENABLE_AESNI=$<BOOL:${LEMON_ENABLE_AESNI}>")

if(ANDROID)
    message(STATUS "Configuring ${PROJECT_NAME} for Android")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "Configuring ${PROJECT_NAME} for Linux")
    # Additional configurations for Linux
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(STATUS "Configuring ${PROJECT_NAME} for Windows")
    # Additional configurations for Windows
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(STATUS "Configuring ${PROJECT_NAME} for macOS")
    # Additional configurations for macOS
else()
    message(WARNING "Unsupported operating system: ${CMAKE_SYSTEM_NAME}")
endif()




if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # Apple clang / ld64 do not accept -static-libgcc/-static-libstdc++ (those are GCC-only).
    # instead give the executable an rpath so it finds libdatachannel.dylib sitting next to it.
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath,@loader_path")
elseif(NOT ANDROID AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
endif()

add_executable(chat-server
        main.c
        first_time_setup.c
        core/server_settings.c
        base.c
        core/channel.c
        chat_websocket.c
        core/client_login.c
        core/client_session.c
        core/attachments.c
        core/identity_store.c
        core/administration.c
        core/emojis.c
        core/permissions.c
        stunnel.c
        tailscale.c
        core/audio_channel.c
        audio_webrtc.c
        audio_websocket.c
        metadata_crypto.c
        media_auth.c
        core/video_channel.c
        video_websocket.c
        video_webrtc.c
        ../third-party/rxi-log/log.c
        ../third-party/zhicheng/base64.c
        ../third-party/kokke-tiny-aes-c/aes.c
        ../third-party/ITH-sha/sha256.c
        ../third-party/dave-g-json/cJSON.c
        clib/clib_memory.c
        clib/clib_string.c
        clib/clib_rwlock.c
        msg/client/client_msg_connection.c
        msg/client/client_msg_channel.c
        msg/client/client_msg_chat.c
        msg/client/client_msg_attachments.c
        msg/client/client_msg_profile.c
        msg/client/client_msg_server_settings.c
        msg/client/client_msg_administration.c
        msg/client/client_msg_emojis.c
        msg/client/client_msg_audio.c
        msg/client/client_msg_video.c
        msg/client/client_msg_musicbot.c
        core/login_admission.c
        msg/server/server_msg_delivery.c
        msg/server/server_msg_connection.c
        msg/server/server_msg_channel.c
        msg/server/server_msg_chat.c
        msg/server/server_msg_attachments.c
        msg/server/server_msg_profile.c
        msg/server/server_msg_server_settings.c
        msg/server/server_msg_administration.c
        msg/server/server_msg_emojis.c
        msg/server/server_msg_audio.c
        msg/server/server_msg_video.c
        msg/server/server_msg_musicbot.c
        core/server_logs.c
        memory_manager.c
        ip_tools.c
        core/musicbot.c
        util.c
        httpserver/http_server.c
        httpclient/httpclient.c
        httpclient/url_preview.c
        httpclient/url_resolver.c
        )

target_include_directories(chat-server PRIVATE
    ${PROJECT_SOURCE_DIR}/../third-party/libviolet-0.5.4/deps/libjuice/include
    ${PROJECT_SOURCE_DIR}/../third-party/libviolet-0.5.4/deps/libjuice/include/juice
    ${PROJECT_SOURCE_DIR}/../third-party/mbedtls-3.6.6/include)

# Allocation tracking takes a shared lock on every allocation and free.
# Keep it in Debug builds; Release builds can enable it when investigating leaks.
target_compile_definitions(chat-server PRIVATE
    LEMON_TRACK_ALLOCATIONS=$<IF:$<CONFIG:Debug>,1,$<BOOL:${LEMON_TRACK_ALLOCATIONS}>>)


# Build the parser from source so security fixes cannot be bypassed by a stale
# prebuilt libws.a in linkage-files.
find_package(Threads REQUIRED)
add_library(server-websocket STATIC
    ../third-party/theldus-websocket/src/ws.c
    ../third-party/theldus-websocket/src/base64.c
    ../third-party/theldus-websocket/src/sha1.c
    ../third-party/theldus-websocket/src/handshake.c
    ../third-party/theldus-websocket/src/utf8.c)
target_compile_definitions(server-websocket PRIVATE VALIDATE_UTF8)
target_include_directories(server-websocket PRIVATE ../third-party/theldus-websocket/include)
target_link_libraries(server-websocket PUBLIC Threads::Threads ${LEMON_ATOMIC_LIBRARY})
target_link_libraries(chat-server PRIVATE server-websocket)

# Keep Debug builds easy to inspect. CMake checks both the compiler and linker
# before enabling optimization across the server's C files in Release builds.
if(LEMON_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT LEMON_LTO_SUPPORTED OUTPUT LEMON_LTO_ERROR LANGUAGES C)
    if(LEMON_LTO_SUPPORTED)
        set_target_properties(chat-server server-websocket PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
            INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    else()
        message(STATUS "Link-time optimization unavailable; using normal Release optimization")
    endif()
endif()

if(WIN32)
    target_sources(chat-server PRIVATE httpclient/httpclient_windows.c)
    set_source_files_properties(httpclient/httpclient.c httpclient/httpclient_windows.c
        PROPERTIES COMPILE_DEFINITIONS "_WIN32_WINNT=0x0A00")
    target_link_libraries(chat-server PRIVATE secur32 psapi iphlpapi)
else()
    target_sources(chat-server PRIVATE httpclient/httpclient_posix.c)
    if(APPLE)
        target_link_libraries(chat-server PRIVATE "-framework Security" "-framework CoreFoundation")
    endif()
endif()

if(ANDROID)
    # These source-built targets are provided by android-server-app/native.
    target_link_libraries(chat-server PRIVATE datachannel-static libtommath
        android_violet MbedTLS::MbedTLS android_maxminddb opus dl m)
endif()

#linux uses forward slash for path, windows two backward slashes

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(chat-server PRIVATE ${PROJECT_SOURCE_DIR}\\linkage-files\\windows\\libdatachannel.dll.a) #dll needs complementary .lib (.a) file in this case
    target_link_libraries(chat-server PRIVATE ${PROJECT_SOURCE_DIR}\\linkage-files\\windows\\libtommath.a)
    target_link_libraries(chat-server PRIVATE ${PROJECT_SOURCE_DIR}\\linkage-files\\windows\\libtomcrypt.a)
    target_link_libraries(chat-server PRIVATE ${PROJECT_SOURCE_DIR}\\linkage-files\\windows\\libviolet.a)
    target_link_libraries(chat-server PRIVATE ${PROJECT_SOURCE_DIR}\\linkage-files\\windows\\libmbedtls.a)
    target_link_libraries(chat-server PRIVATE ${PROJECT_SOURCE_DIR}\\linkage-files\\windows\\libmbedx509.a)
    target_link_libraries(chat-server PRIVATE ${PROJECT_SOURCE_DIR}\\linkage-files\\windows\\libmbedcrypto.a)
    target_link_libraries(chat-server PRIVATE ${PROJECT_SOURCE_DIR}\\linkage-files\\windows\\libmaxminddb.a)
    target_link_libraries(chat-server PRIVATE ${PROJECT_SOURCE_DIR}\\linkage-files\\windows\\libopus.a)
    target_link_libraries(chat-server PRIVATE ws2_32 wsock32 bcrypt) #system libraries
endif()


if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(chat-server
    PRIVATE
    ${PROJECT_SOURCE_DIR}/linkage-files/linux/libtommath.a
    ${PROJECT_SOURCE_DIR}/linkage-files/linux/libtomcrypt.a
    ${PROJECT_SOURCE_DIR}/linkage-files/linux/libviolet.a
    ${PROJECT_SOURCE_DIR}/linkage-files/linux/libmbedtls.a
    ${PROJECT_SOURCE_DIR}/linkage-files/linux/libmbedx509.a
    ${PROJECT_SOURCE_DIR}/linkage-files/linux/libmbedcrypto.a
    ${PROJECT_SOURCE_DIR}/linkage-files/linux/libmaxminddb.a
    ${PROJECT_SOURCE_DIR}/linkage-files/linux/libopus.a
    dl
    m
    )

    add_library(datachannel SHARED IMPORTED GLOBAL)
    set_target_properties(datachannel PROPERTIES IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/linkage-files/linux/libdatachannel.so)
    target_link_libraries(chat-server PRIVATE datachannel)

endif()


if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_link_libraries(chat-server
    PRIVATE
    ${PROJECT_SOURCE_DIR}/linkage-files/macos/libtommath.a
    ${PROJECT_SOURCE_DIR}/linkage-files/macos/libtomcrypt.a
    ${PROJECT_SOURCE_DIR}/linkage-files/macos/libviolet.a
    ${PROJECT_SOURCE_DIR}/linkage-files/macos/libmbedtls.a
    ${PROJECT_SOURCE_DIR}/linkage-files/macos/libmbedx509.a
    ${PROJECT_SOURCE_DIR}/linkage-files/macos/libmbedcrypto.a
    ${PROJECT_SOURCE_DIR}/linkage-files/macos/libmaxminddb.a
    ${PROJECT_SOURCE_DIR}/linkage-files/macos/libopus.a
    m
    )
    # dlopen lives in libSystem on macOS, so there is no separate -ldl. if the datachannel
    # link ever reports undefined framework symbols, add e.g. "-framework Security" here.

    # datachannel is the one shared library; it must sit next to chat-server.bin at runtime.
    # the build script normalises its install_name to @rpath/libdatachannel.dylib, and the
    # executable gets an @loader_path rpath (above), so it resolves from its own directory.
    add_library(datachannel SHARED IMPORTED GLOBAL)
    set_target_properties(datachannel PROPERTIES IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/linkage-files/macos/libdatachannel.dylib)
    target_link_libraries(chat-server PRIVATE datachannel)
endif()

option(SERVER_REGRESSION_TESTS "Enable server and browser regression checks" OFF)
if(SERVER_REGRESSION_TESTS)
    enable_testing()
    find_program(NODE_EXECUTABLE NAMES node nodejs REQUIRED)
    if(WIN32)
        add_custom_command(TARGET chat-server POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PROJECT_SOURCE_DIR}/linkage-files/windows/libdatachannel.dll"
                "$<TARGET_FILE_DIR:chat-server>")
    endif()
    add_test(NAME channel_maintainer
        COMMAND ${NODE_EXECUTABLE} "${PROJECT_SOURCE_DIR}/../../tests/server/channel-maintainer-test.cjs"
            $<TARGET_FILE:chat-server>)
    add_test(NAME channel_keys
        COMMAND ${NODE_EXECUTABLE} "${PROJECT_SOURCE_DIR}/../../tests/browser/channel-keys.test.cjs")
    set_tests_properties(channel_maintainer channel_keys PROPERTIES TIMEOUT 180)

    # Run the actual setup without opening ports or starting external programs.
    get_target_property(SETUP_TEST_SOURCES chat-server SOURCES)
    list(REMOVE_ITEM SETUP_TEST_SOURCES main.c)
    add_executable(first-time-setup-driver ${SETUP_TEST_SOURCES}
        "${PROJECT_SOURCE_DIR}/../../tests/server/first-time-setup-driver.c")
    target_include_directories(first-time-setup-driver PRIVATE
        $<TARGET_PROPERTY:chat-server,INCLUDE_DIRECTORIES>)
    target_compile_definitions(first-time-setup-driver PRIVATE
        $<TARGET_PROPERTY:chat-server,COMPILE_DEFINITIONS>)
    target_link_libraries(first-time-setup-driver PRIVATE
        $<TARGET_PROPERTY:chat-server,LINK_LIBRARIES>)
    if(WIN32)
        add_custom_command(TARGET first-time-setup-driver POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PROJECT_SOURCE_DIR}/linkage-files/windows/libdatachannel.dll"
                "$<TARGET_FILE_DIR:first-time-setup-driver>")
    endif()
    add_test(NAME first_time_setup
        COMMAND ${NODE_EXECUTABLE} "${PROJECT_SOURCE_DIR}/../../tests/server/first-time-setup-test.cjs"
            $<TARGET_FILE:first-time-setup-driver>)
    set_tests_properties(first_time_setup PROPERTIES TIMEOUT 60)
endif()
