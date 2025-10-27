cmake_minimum_required(VERSION 3.23.2)
set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_C_STANDARD 99)

project(violet C)

set(LIBVIOLET_HEADERS
	${CMAKE_CURRENT_SOURCE_DIR}/src/options.h
	${CMAKE_CURRENT_SOURCE_DIR}/src/utils.h
)

set(LIBVIOLET_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/src/options.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/utils.c
)


set(LIBJUICE_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/include/juice/juice.h
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/addr.h
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/agent.h
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/base64.h
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/const_time.h
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/crc32.h
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/hash.h
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/hmac.h
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/ice.h
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/log.h
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/picohash.h
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/random.h
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/server.h
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/socket.h
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/stun.h
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/thread.h
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/timestamp.h
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/turn.h
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/udp.h
)

set(LIBJUICE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/addr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/agent.c
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/base64.c
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/const_time.c
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/crc32.c
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/hash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/hmac.c
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/ice.c
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/juice.c
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/log.c
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/random.c
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/server.c
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/stun.c
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/timestamp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/turn.c
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/udp.c
)


add_library(violet STATIC
    ${LIBVIOLET_HEADERS}
    ${LIBVIOLET_SOURCES}
    ${LIBJUICE_HEADERS}
    ${LIBJUICE_SOURCES}
)