# cmake/meek_ui.cmake
#
# The ONE place that knows which files make up meek-ui. Every build
# (the Windows shared library, the Linux single-binary examples, the
# test runner, and any host that consumes the library through
# add_subdirectory) asks these functions instead of listing sources
# by hand. Before this file existed there were 16 hand-written
# source lists across the tree and they drifted (atlas.c was missing
# from the Windows list, for example).
#
# Layout the functions assume:
#
#   src/                    backend-neutral scene, parser, animation,
#                           font, scroll, widgets/, clib/, third_party/
#   backends/renderers/     shared graphics API implementations and shaders
#   backends/platform/      OS window/input/filesystem adapters and entry points
#   backends/common/        shared CPU helpers, contracts and startup trampoline
#   backends/embedded/      host-owned device adapters and input translation
#   fonts/                  .ttf files shipped for the examples and tests
#
# Usage from a host CMakeLists.txt:
#
#   include(<path-to-meek-ui>/cmake/meek_ui.cmake)
#   meek_ui_core_sources(CORE)
#   meek_ui_backend_sources(BACKEND d3d11 win32)
#   add_executable(myapp main.c ${CORE} ${BACKEND})
#   target_include_directories(myapp PRIVATE ${MEEK_UI_INCLUDE_DIRS})
#   target_compile_definitions(myapp PRIVATE MEEK_UI_STATIC)
#   meek_ui_renderer_link_libs(LIBS d3d11)
#   target_link_libraries(myapp PRIVATE user32 gdi32 ${LIBS})

include_guard(GLOBAL)

get_filename_component(MEEK_UI_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
# Keep public header names usable by existing standalone and embedded hosts.
set(MEEK_UI_INCLUDE_DIRS
    "${MEEK_UI_ROOT}/src"
    "${MEEK_UI_ROOT}/backends/common"
    "${MEEK_UI_ROOT}/backends/embedded")
foreach(_api IN ITEMS opengl2 opengl3 gles3 d3d9 d3d11 d3d12 vulkan)
    list(APPEND MEEK_UI_INCLUDE_DIRS "${MEEK_UI_ROOT}/backends/renderers/${_api}")
endforeach()
foreach(_os IN ITEMS win32 linux android macos)
    list(APPEND MEEK_UI_INCLUDE_DIRS "${MEEK_UI_ROOT}/backends/platform/${_os}")
endforeach()
set(MEEK_UI_FONTS_DIR "${MEEK_UI_ROOT}/fonts")
set(MEEK_UI_EXAMPLE_ASSETS_DIR "${MEEK_UI_ROOT}/examples/assets")

set(MEEK_UI_RENDERERS opengl2 opengl3 gles3 d3d11 d3d9 d3d12 vulkan)
set(MEEK_UI_PLATFORMS win32 linux_x11 linux_drm linux_wayland android macos none)

# Core sources: everything under src/ that is not a backend. A glob
# is deliberate here -- src/ contains only core files after the
# reorganization, so "every .c in src/" IS the canonical list and
# cannot drift. CONFIGURE_DEPENDS makes CMake re-glob when files are
# added or removed.
function(meek_ui_core_sources out_var)
    file(GLOB _core CONFIGURE_DEPENDS
        "${MEEK_UI_ROOT}/src/*.c"
        "${MEEK_UI_ROOT}/src/widgets/*.c"
        "${MEEK_UI_ROOT}/src/clib/*.c"
        "${MEEK_UI_ROOT}/src/third_party/*.c")
    # Unused reference implementation from another clib layout. Its
    # definitions.h and third-party/rxi-log dependencies are not shipped.
    list(REMOVE_ITEM _core "${MEEK_UI_ROOT}/src/clib/clib_rwlock.c")
    if(WIN32)
        # Explicit opt-in compute engine, shared by standalone and embedded hosts.
        # DLLs are loaded lazily from System32; CPU-only hosts need no extra imports.
        list(APPEND _core "${MEEK_UI_ROOT}/backends/renderers/d3d11/effects_d3d11.c")
    endif()
    # The portable compositor contains scalar pixel loops. Keep those loops
    # optimized in debug hosts as well; symbols and assertions remain enabled.
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        set_source_files_properties("${MEEK_UI_ROOT}/src/compositor.c" "${MEEK_UI_ROOT}/src/gradient.c" "${MEEK_UI_ROOT}/src/motion.c" "${MEEK_UI_ROOT}/src/clib/stdlib.c" PROPERTIES COMPILE_OPTIONS "-O2")
        # Allow vectorization of contiguous pixel spans without relaxing floating
        # point semantics or requiring a particular CPU instruction extension.
        set_source_files_properties("${MEEK_UI_ROOT}/src/compositor.c" "${MEEK_UI_ROOT}/src/gradient.c" PROPERTIES COMPILE_OPTIONS "-O3")
    endif()
    set(${out_var} ${_core} PARENT_SCOPE)
endfunction()

# Backend sources for one renderer + one platform.
#
#   renderer: opengl3 | gles3 | d3d11 | d3d9 | d3d12 | vulkan
#   platform: win32 | linux_x11 | linux_drm | linux_wayland | android | macos | none
#
# "none" is the embedded / overlay case: the host owns the window,
# the device and the main loop, feeds input to scene__on_mouse_*
# itself and calls renderer__init_adopt. It still needs the fs_*
# file for its OS so font and image loading work.
function(meek_ui_backend_sources out_var renderer platform)
    set(_b "${MEEK_UI_ROOT}/backends")
    if(NOT renderer IN_LIST MEEK_UI_RENDERERS)
        message(FATAL_ERROR "meek-ui: unknown renderer '${renderer}' (valid: ${MEEK_UI_RENDERERS})")
    endif()
    if(NOT platform IN_LIST MEEK_UI_PLATFORMS)
        message(FATAL_ERROR "meek-ui: unknown platform '${platform}' (valid: ${MEEK_UI_PLATFORMS})")
    endif()

    set(_src "${_b}/renderers/${renderer}/${renderer}_renderer.c")

    # The texture atlas talks to GL directly, so only the GLES backend
    # gets the real one. Every other renderer links the no-op atlas:
    # atlas__alloc always fails and widget_image falls back to one
    # texture per image, which every backend supports.
    if(renderer STREQUAL "gles3")
        list(APPEND _src "${_b}/renderers/gles3/atlas_gles3.c")
    else()
        list(APPEND _src "${_b}/common/atlas_null.c")
    endif()

    if(platform STREQUAL "win32")
        list(APPEND _src "${_b}/platform/win32/platform_win32.c" "${_b}/platform/win32/fs_win32.c")
    elseif(platform STREQUAL "linux_x11")
        list(APPEND _src "${_b}/platform/linux/platform_linux_x11.c" "${_b}/platform/linux/fs_linux.c")
    elseif(platform STREQUAL "linux_drm")
        list(APPEND _src "${_b}/platform/linux/platform_linux_drm.c" "${_b}/platform/linux/fs_linux.c")
    elseif(platform STREQUAL "linux_wayland")
        list(APPEND _src "${_b}/platform/linux/platform_linux_wayland_client.c" "${_b}/platform/linux/fs_linux.c")
    elseif(platform STREQUAL "android")
        list(APPEND _src "${_b}/platform/android/platform_android.c" "${_b}/platform/android/fs_android.c")
    elseif(platform STREQUAL "macos")
        list(APPEND _src "${_b}/platform/macos/platform_macos.m" "${_b}/platform/macos/fs_macos.c")
    elseif(platform STREQUAL "none")
        if(WIN32)
            list(APPEND _src "${_b}/platform/win32/fs_win32.c")
        elseif(APPLE)
            list(APPEND _src "${_b}/platform/macos/fs_macos.c")
        else()
            list(APPEND _src "${_b}/platform/linux/fs_linux.c")
        endif()
    endif()
    set(${out_var} ${_src} PARENT_SCOPE)
endfunction()

# Embedded backend: the shared renderer in adopt mode plus the state
# backup / restore layer in backends/embedded/ and the Win32 input
# translation. No platform file; the host owns the window and the loop.
#
#   renderer: d3d9 | d3d11 | d3d12 | opengl2 | opengl3 | gles3 | vulkan
function(meek_ui_embedded_backend_sources out_var renderer)
    set(_b "${MEEK_UI_ROOT}/backends")
    set(_e "${MEEK_UI_ROOT}/backends/embedded")
    if(renderer STREQUAL "d3d11" OR renderer STREQUAL "d3d9" OR renderer STREQUAL "d3d12" OR renderer STREQUAL "gles3" OR renderer STREQUAL "opengl2" OR renderer STREQUAL "opengl3" OR renderer STREQUAL "vulkan")
        set(_src "${_b}/renderers/${renderer}/${renderer}_renderer.c" "${_b}/common/atlas_null.c" "${_e}/embedded_${renderer}.c")
        if(WIN32)
            list(APPEND _src "${_b}/platform/win32/fs_win32.c" "${_e}/embedded_win32_input.c")
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND (renderer STREQUAL "gles3" OR renderer STREQUAL "opengl2" OR renderer STREQUAL "opengl3" OR renderer STREQUAL "vulkan"))
            list(APPEND _src "${_b}/platform/linux/fs_linux.c")
        else()
            message(FATAL_ERROR "meek-ui: embedded ${renderer} is unsupported on ${CMAKE_SYSTEM_NAME}")
        endif()
    else()
        message(FATAL_ERROR "meek-ui: no embedded backend for renderer '${renderer}' (available: d3d9, d3d11, d3d12, opengl2, opengl3, gles3, vulkan)")
    endif()
    set(${out_var} ${_src} PARENT_SCOPE)
endfunction()

# System libraries a renderer needs on Windows. Linux renderers get
# their libraries from pkg-config in the example CMake files, since
# the names differ per distro.
function(meek_ui_renderer_link_libs out_var renderer)
    if(renderer STREQUAL "opengl2" OR renderer STREQUAL "opengl3")
        set(_libs opengl32)
    elseif(renderer STREQUAL "gles3")
        # Embedded GLES on Windows can use a caller-provided ANGLE SDK.
        find_library(MEEK_UI_GLES_LIBRARY NAMES GLESv2 libGLESv2 REQUIRED)
        set(_libs "${MEEK_UI_GLES_LIBRARY}")
    elseif(renderer STREQUAL "d3d11")
        # d3d11: core API. dxgi: swapchain. d3dcompiler: runtime HLSL
        # compile. dxguid: IID_* constants for QueryInterface calls.
        set(_libs d3d11 dxgi d3dcompiler dxguid)
    elseif(renderer STREQUAL "d3d9")
        set(_libs d3d9 d3dcompiler)
    elseif(renderer STREQUAL "d3d12")
        set(_libs d3d12 dxgi d3dcompiler dxguid)
    elseif(renderer STREQUAL "vulkan")
        find_package(Vulkan REQUIRED)
        set(_libs Vulkan::Vulkan)
    else()
        set(_libs "")
    endif()
    set(${out_var} ${_libs} PARENT_SCOPE)
endfunction()

# Extra include directories for optional graphics SDKs.
function(meek_ui_renderer_include_dirs out_var renderer)
    set(_incs "")
    if(renderer STREQUAL "vulkan")
        find_package(Vulkan REQUIRED)
        set(_incs "${Vulkan_INCLUDE_DIRS}")
    elseif(renderer STREQUAL "gles3" AND WIN32)
        find_path(MEEK_UI_GLES_INCLUDE_DIR GLES3/gl3.h REQUIRED)
        set(_incs "${MEEK_UI_GLES_INCLUDE_DIR}")
    endif()
    set(${out_var} ${_incs} PARENT_SCOPE)
endfunction()

# Warning flags shared by every target in this tree. MSVC is TRUE for
# both cl.exe and clang-cl, which is the right discriminator for flag
# syntax; plain Clang and GCC take the Unix-style driver flags.
function(meek_ui_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /wd4100 /wd4505)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-unused-function)
    endif()
endfunction()
