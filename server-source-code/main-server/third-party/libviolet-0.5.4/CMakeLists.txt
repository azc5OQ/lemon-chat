cmake_minimum_required(VERSION 3.10)
project(violet
	VERSION 0.5.4
	LANGUAGES C)

set(PROJECT_DESCRIPTION "Lightweight STUN/TURN server")

# Options
option(USE_SYSTEM_JUICE "Use system libjuice" OFF)
option(WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

set(CMAKE_C_STANDARD 11)
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/Modules)



include_directories(violet
        PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/include
    	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/include/juice
)

if(WIN32)
	add_definitions(-DWIN32_LEAN_AND_MEAN)
	if(MSVC)
		add_definitions(-DNOMINMAX)
		add_definitions(-D_CRT_SECURE_NO_WARNINGS)
		add_definitions(-D_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
	endif()
endif()

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
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/crc32.h
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/const_time.h
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/conn.h
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/conn_poll.h
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/conn_thread.h
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/conn_mux.h
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/base64.h
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/hash.h
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/hmac.h
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/ice.h
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/log.h
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/random.h
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/server.h
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/stun.h
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/timestamp.h
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/turn.h
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/udp.h
)

set(LIBJUICE_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/addr.c
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/agent.c
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/crc32.c
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/const_time.c
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/conn.c
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/conn_poll.c
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/conn_thread.c
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/conn_mux.c
	${CMAKE_CURRENT_SOURCE_DIR}/deps/libjuice/src/base64.c
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