cmake_minimum_required(VERSION 3.23.2)
set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_C_STANDARD 99)

project(test0s_solution C)


add_subdirectory("${PROJECT_SOURCE_DIR}/libtommath" "${PROJECT_SOURCE_DIR}/libtommath/build-result")
add_subdirectory("${PROJECT_SOURCE_DIR}/libtomcrypt" "${PROJECT_SOURCE_DIR}/libtomcrypt/build-result")

