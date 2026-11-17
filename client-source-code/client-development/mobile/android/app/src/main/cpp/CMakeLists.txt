# starts the verified embedded runtime through node::Start

cmake_minimum_required(VERSION 3.22.1)

project("lemonchatnode")

if(NOT DEFINED NODEJS_MOBILE_ROOT)
    message(FATAL_ERROR "Build through Gradle so the pinned Node runtime is prepared first")
endif()

set(NODEJS_MOBILE_LIBRARY "${NODEJS_MOBILE_ROOT}/jniLibs/${ANDROID_ABI}/libnode.so")
set(NODEJS_MOBILE_HEADERS "${NODEJS_MOBILE_ROOT}/include/node")
if(NOT EXISTS "${NODEJS_MOBILE_ROOT}/runtime.json" OR NOT EXISTS "${NODEJS_MOBILE_LIBRARY}" OR NOT EXISTS "${NODEJS_MOBILE_HEADERS}/node.h")
    message(FATAL_ERROR "The verified Node runtime is incomplete. Run prepareNodeRuntime through Gradle")
endif()

add_library(node SHARED IMPORTED)
set_target_properties(node PROPERTIES
    IMPORTED_LOCATION "${NODEJS_MOBILE_LIBRARY}")

add_library(lemonchatnode SHARED native-lib.cpp)

target_compile_features(lemonchatnode PRIVATE cxx_std_20)
target_include_directories(lemonchatnode PRIVATE "${NODEJS_MOBILE_HEADERS}")
target_link_options(lemonchatnode PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")

# log is needed for the stdout/stderr pump into logcat
target_link_libraries(lemonchatnode
    node
    log)
