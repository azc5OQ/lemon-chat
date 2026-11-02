cmake_minimum_required(VERSION 3.21)

# meek-ui/CMakeLists.txt
#
# Top-level build for the library, its examples and its tests.
#
#   Windows:  builds meek_ui as a shared library (meek_ui.dll) or a
#             static library (-DMEEK_UI_SHARED=OFF), then the Win32
#             examples and the visual test runner against it.
#   Linux:    each example and the test runner is a single binary
#             that compiles the library sources directly, the same
#             shape as the build.sh scripts next to them.
#   Android / macOS are not driven from here; see the build.sh in
#             examples/android and examples/macos.
#
# Which files are compiled is decided in cmake/meek_ui.cmake. This
# file only chooses the renderer / platform and wires targets.
#
# Options:
#   MEEK_UI_RENDERER           opengl3 (Windows default) | gles3 (Linux
#                              default) | d3d11 | d3d9 | d3d12 | vulkan
#   MEEK_UI_SHARED             ON: meek_ui.dll. OFF: static library.
#   MEEK_UI_TRACK_ALLOCATIONS  ON: memory_manager leak + type reports.
#   MEEK_UI_BUILD_EXAMPLES     ON
#   MEEK_UI_BUILD_TESTS        ON
#   MEEK_UI_LINUX_BACKEND      x11 | drm | wayland | all (Linux only)

project(meek_ui C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(cmake/meek_ui.cmake)

if(WIN32)
    set(_default_renderer opengl3)
else()
    set(_default_renderer gles3)
endif()
set(MEEK_UI_RENDERER "${_default_renderer}" CACHE STRING "Renderer backend: ${MEEK_UI_RENDERERS}")
set_property(CACHE MEEK_UI_RENDERER PROPERTY STRINGS ${MEEK_UI_RENDERERS})
# GUI_RENDERER was the pre-reorganization name of this option.
if(DEFINED GUI_RENDERER AND NOT GUI_RENDERER STREQUAL "")
    set(MEEK_UI_RENDERER "${GUI_RENDERER}" CACHE STRING "Renderer backend" FORCE)
endif()

option(MEEK_UI_SHARED            "Build meek_ui as a shared library on Windows" ON)
option(MEEK_UI_TRACK_ALLOCATIONS "Track all heap allocations (leak + type reporting)" ON)
option(MEEK_UI_BUILD_EXAMPLES    "Build the example hosts" ON)
option(MEEK_UI_BUILD_TESTS       "Build the visual regression test runner" ON)

message(STATUS "meek-ui: renderer = ${MEEK_UI_RENDERER}")

# Every artifact lands in the build directory root. On Windows that
# puts meek_ui.dll next to the executables that load it.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}")
endforeach()

if(WIN32)
    # UNICODE selects the W variants of the Win32 API; platform_win32.c
    # calls them explicitly, so a build without this define fails on
    # LoadCursorW.
    add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

    meek_ui_core_sources(_core)
    meek_ui_backend_sources(_backend ${MEEK_UI_RENDERER} win32)
    meek_ui_renderer_link_libs(_libs ${MEEK_UI_RENDERER})
    meek_ui_renderer_include_dirs(_incs ${MEEK_UI_RENDERER})

    if(MEEK_UI_SHARED)
        # src/meek_ui.def exports the memory manager, whose header is
        # self-contained and carries no GUI_API
        add_library(meek_ui SHARED ${_core} ${_backend} ${CMAKE_CURRENT_SOURCE_DIR}/src/meek_ui.def)
        # GUI_BUILDING_DLL flips GUI_API to dllexport while compiling
        # the library; consumers see dllimport.
        target_compile_definitions(meek_ui PRIVATE GUI_BUILDING_DLL)
        # Same file name on every toolchain (MinGW would otherwise prepend lib).
        set_target_properties(meek_ui PROPERTIES PREFIX "")
    else()
        add_library(meek_ui STATIC ${_core} ${_backend})
        target_compile_definitions(meek_ui PUBLIC MEEK_UI_STATIC)
    endif()

    target_include_directories(meek_ui PUBLIC ${MEEK_UI_INCLUDE_DIRS})
    if(_incs)
        target_include_directories(meek_ui PRIVATE ${_incs})
    endif()
    if(MEEK_UI_TRACK_ALLOCATIONS)
        target_compile_definitions(meek_ui PUBLIC GUI_TRACK_ALLOCATIONS)
    endif()
    target_link_libraries(meek_ui PRIVATE user32 gdi32 ${_libs})
    meek_ui_apply_warnings(meek_ui)

    if(MEEK_UI_BUILD_EXAMPLES)
        add_subdirectory(examples/win32)
        add_subdirectory(examples/win32_code)
        add_subdirectory(examples/embedded_d3d11)
        add_subdirectory(examples/embedded_d3d9)
        add_subdirectory(examples/embedded_d3d9_min)
        add_subdirectory(examples/embedded_opengl)
        add_subdirectory(examples/embedded_opengl2)
        add_subdirectory(examples/embedded_d3d12)
        add_subdirectory(examples/embedded_vulkan)
    endif()
    if(MEEK_UI_BUILD_TESTS)
        add_subdirectory(tests/visual)
    endif()

elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(MEEK_UI_LINUX_BACKEND "all" CACHE STRING "Linux window backend: x11, drm, wayland, or all")
    set_property(CACHE MEEK_UI_LINUX_BACKEND PROPERTY STRINGS x11 drm wayland all)

    if(MEEK_UI_BUILD_EXAMPLES)
        add_subdirectory(examples/embedded_gles3)
        add_subdirectory(examples/embedded_opengl)
        add_subdirectory(examples/embedded_opengl2)
        add_subdirectory(examples/embedded_vulkan)
        if(MEEK_UI_LINUX_BACKEND STREQUAL "x11" OR MEEK_UI_LINUX_BACKEND STREQUAL "all")
            add_subdirectory(examples/linux_x11)
        endif()
        if(MEEK_UI_LINUX_BACKEND STREQUAL "drm" OR MEEK_UI_LINUX_BACKEND STREQUAL "all")
            add_subdirectory(examples/linux_drm)
        endif()
        if(MEEK_UI_LINUX_BACKEND STREQUAL "wayland" OR MEEK_UI_LINUX_BACKEND STREQUAL "all")
            add_subdirectory(examples/linux_wayland)
        endif()
    endif()
    if(MEEK_UI_BUILD_TESTS AND (MEEK_UI_LINUX_BACKEND STREQUAL "x11" OR MEEK_UI_LINUX_BACKEND STREQUAL "all"))
        add_subdirectory(tests/visual)
    endif()

else()
    message(FATAL_ERROR "This CMake supports Windows and Linux. Android builds go through examples/android/build.sh; macOS through examples/macos/build.sh. Detected: ${CMAKE_SYSTEM_NAME}")
endif()

# Shared test/demo actions remain outside the library and public renderer API.
foreach(_host IN ITEMS
    example_embedded_d3d9
    example_embedded_d3d11
    example_embedded_d3d12
    example_embedded_opengl
    example_embedded_opengl2
    example_embedded_opengl_linux
    example_embedded_gles3
    example_embedded_vulkan
)
    if(TARGET ${_host})
        target_sources(${_host} PRIVATE
            examples/embedded_common/navigation_demo.c
            examples/embedded_common/embedded_replay.c
        )
    endif()
endforeach()
