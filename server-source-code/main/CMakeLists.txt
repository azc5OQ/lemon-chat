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


#pozor, moznost -static dodrbe, dynamicke kniznice

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")

include_directories(untitled
        PUBLIC
        ${PROJECT_SOURCE_DIR}/../third-party/mbedtls-3.5.1/include)

add_executable(chat-server
        main.c
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
        memory_manager.c
        ip_tools.c
        musicbot.c
        util.c
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
