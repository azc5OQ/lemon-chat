# JNI glue that starts the embedded node runtime. The node itself is the prebuilt libnode.so from
# nodejs-mobile (jniLibs/<abi>/libnode.so); nothing here compiles node, it only calls node::Start.
#
# The headers next to this file come from the same nodejs-mobile release as the .so and must be
# kept in step with it - node.h's Start() signature is the whole contract.

cmake_minimum_required(VERSION 3.22.1)

project("lemonchatnode")

# the prebuilt node, picked per abi by gradle's abiFilters
add_library(node SHARED IMPORTED)
set_target_properties(node PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libnode.so)

add_library(lemonchatnode SHARED native-lib.cpp)

target_include_directories(lemonchatnode PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/nodejs-mobile-include/node)

# log is needed for the stdout/stderr pump into logcat
target_link_libraries(lemonchatnode
    node
    log)
