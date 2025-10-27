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

include_directories(untitled PUBLIC ${PROJECT_SOURCE_DIR}/../mbedtls-3.5.1/include)

add_executable(chat-server
        main.c
        audio_channel.c
        log/log.c
        zhicheng/base64.c
        kokke-tiny-aes-c/aes.c
        ITH-sha/sha256.c
        dave-g-json/cJSON.c
        clib/clib_memory.c
        clib/clib_string.c
        client_message.c
        server_message.c
        memory_manager.c
        vec/vec.c
        ip_tools.c
        )



#in the command 'target_link_libraries' the keywords PUBLIC,PRIVATE,INTERFACE have certain meaning, which has something to do with how header files of libs are used within CMakeList's target
#PUBLIC -> both source and header files within this project contain #include to header files of the linked library
#PRIVATE -> only source files within this project contain #include to header files of this linked library
#INTERFACE -> only header files within this project contain #include to header files of this linked library




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
    target_link_libraries(chat-server PRIVATE ws2_32 wsock32 bcrypt) #system libraries
endif()


if(CMAKE_SYSTEM_NAME STREQUAL "Linux")

    target_link_libraries(chat-server PRIVATE ${PROJECT_SOURCE_DIR}/linkage-files/linux/libws.a)
    target_link_libraries(chat-server PRIVATE ${PROJECT_SOURCE_DIR}/linkage-files/linux/libtommath.a)
    target_link_libraries(chat-server PRIVATE ${PROJECT_SOURCE_DIR}/linkage-files/linux/libtomcrypt.a)
    target_link_libraries(chat-server PRIVATE ${PROJECT_SOURCE_DIR}/linkage-files/linux/libviolet.a)
    target_link_libraries(chat-server PRIVATE ${PROJECT_SOURCE_DIR}/linkage-files/linux/libmbedtls.a)
    target_link_libraries(chat-server PRIVATE ${PROJECT_SOURCE_DIR}/linkage-files/linux/libmbedx509.a)
    target_link_libraries(chat-server PRIVATE ${PROJECT_SOURCE_DIR}/linkage-files/linux/libmbedcrypto.a)
    target_link_libraries(chat-server PRIVATE ${PROJECT_SOURCE_DIR}/linkage-files/linux/libmaxminddb.a)



#link .so file to executable, in this case libdatachannel.so located in linkage-files/linux
#GLOBAL keyword allows finding .so file even outside of root path of CMakeLists.txt. wont harm
#SHARED tells cmake that .so file is being imported
#IMPORTED keyword tells cmake that library that is being imported, is not built directly by this CMakeLists.txt and comes from elsewhere
#PRIVATE keyword already explained, it had to be used even if it doesnt make sense
# gcc toolchain  -static flag enforces only static libs linking, and if used, will make this code unusable

    add_library(datachannel SHARED IMPORTED GLOBAL)
    set_target_properties(datachannel PROPERTIES IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/linkage-files/linux/libdatachannel.so)
    target_link_libraries(chat-server PRIVATE datachannel)

endif()




#besides these static libs, executable also needs to be glued with pthread.so and libdatachannel.so. That is done in linux_build_script.sh file
#there is no .a / .lib file associated with .so like with .dll on windows, but, .so file still needs to be specifice
#I have not found a way to set project to use .so file here in CMakeLists.txt so its specified in C_FLAGS

#   libdatachannel.so cannot be linked as static library, or this will occur
#/usr/lib/gcc/x86_64-pc-linux-gnu/13/../../../../x86_64-pc-linux-gnu/bin/ld: attempted static link of dynamic object `linkage-files/linux/libdatachannel.so'
#collect2: error: ld returned 1 exit status
#ninja: build stopped: subcommand failed.
