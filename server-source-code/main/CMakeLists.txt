#set(CMAKE_VERBOSE_MAKEFILE ON)

cmake_minimum_required(VERSION 3.23)
project(untitled C)
set(CMAKE_C_STANDARD 99)


get_filename_component(PROJECT_NAME ${CMAKE_CURRENT_SOURCE_DIR} NAME)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
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
else()
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
endif()

include_directories(untitled
        PUBLIC
        ${PROJECT_SOURCE_DIR}/../third-party/libviolet-0.5.4/deps/libjuice/include
        ${PROJECT_SOURCE_DIR}/../third-party/libviolet-0.5.4/deps/libjuice/include/juice
        ${PROJECT_SOURCE_DIR}/../third-party/mbedtls-3.6.6/include)

add_executable(chat-server
        main.c
        first_time_setup.c
        settings.c
        base.c
        audio_channel.c
        ../third-party/rxi-log/log.c
        ../third-party/zhicheng/base64.c
        ../third-party/kokke-tiny-aes-c/aes.c
        ../third-party/ITH-sha/sha256.c
        ../third-party/dave-g-json/cJSON.c
        clib/clib_memory.c
        clib/clib_string.c
        clib/clib_rwlock.c
        client_message.c
        server_message.c
        server_logs.c
        memory_manager.c
        ip_tools.c
        musicbot.c
        util.c
        http/http_server.c
        )


#linux uses forward slash for path, windows two backward slashes

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(chat-server PRIVATE ${PROJECT_SOURCE_DIR}\\linkage-files\\windows\\libws.a)
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
    ${PROJECT_SOURCE_DIR}/linkage-files/linux/libws.a
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
    ${PROJECT_SOURCE_DIR}/linkage-files/macos/libws.a
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
