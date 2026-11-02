# Compile meek-ui directly into your executable or DLL

You can add meek-ui's C source files to your own program and compile them together. There is no separate meek-ui `.lib`, `.a` or DLL (dynamic-link library). This works for an executable or for a plugin DLL: meek-ui becomes part of that binary. The existing [Direct3D 11 example](../../examples/embedded_d3d11/CMakeLists.txt) already builds this way.

This guide uses an embedded backend: your program owns the window, graphics device and drawing loop, and meek-ui draws into them. UI means user interface. OpenGL means Open Graphics Library; GLES means OpenGL for Embedded Systems. Include headers in your application code and add the selected `.c` files to the build as separate compilation units. Each compilation unit is compiled independently before the linker combines them. There is no supported single-header implementation or combined file which you enable with an implementation macro; including every `.c` inside `main.c` can cause duplicate private names.

## CMake: add the sources to your existing target

The following is a complete build-file example for Windows and Direct3D 11. `main.c` is your own host implementation. Change the checkout path to match your project. The helper file selects sources and include directories; including it does not build a library or add the standalone application integration.

```cmake
cmake_minimum_required(VERSION 3.21)
project(my_overlay C)

# Your executable owns its entry point and graphics device.
# In an existing project, keep its add_executable and use that target name below.
add_executable(my_overlay main.c)

# Keep the meek-ui directory tree intact, including generated shader headers.
include("path/to/meek-ui/cmake/meek_ui.cmake")
set(overlay_renderer d3d11)

# Core includes layout, widgets, styles, animation, fonts, images and drawing.
# Backend includes the selected renderer, embedded adapter and file/input helpers.
meek_ui_core_sources(overlay_core)
meek_ui_embedded_backend_sources(overlay_backend ${overlay_renderer})
meek_ui_renderer_include_dirs(overlay_sdk_includes ${overlay_renderer})
meek_ui_renderer_link_libs(overlay_system_libraries ${overlay_renderer})

# Compile every selected .c directly into YOUR target. No meek-ui archive target.
target_sources(my_overlay PRIVATE ${overlay_core} ${overlay_backend})
target_compile_features(my_overlay PRIVATE c_std_11)
target_include_directories(my_overlay PRIVATE
    ${MEEK_UI_INCLUDE_DIRS}
    ${overlay_sdk_includes}
)

# These definitions apply to both your files and meek-ui files in this target.
target_compile_definitions(my_overlay PRIVATE
    MEEK_UI_STATIC          # Direct calls inside this binary; no DLL imports.
    MEEK_UI_HOST_OWNS_MAIN  # Keep your own entry point and drawing loop.
    GUI_TRACK_ALLOCATIONS  # Account for meek-ui allocations with its memory manager.
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)

# These are Windows/graphics dependencies, not a separately built meek-ui library.
target_link_libraries(my_overlay PRIVATE
    user32 gdi32 ${overlay_system_libraries}
)
```

Use ordinary Clang with your existing Windows target/toolchain environment:

```sh
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release
cmake --build build --target my_overlay
```

Keep any target, runtime and toolchain options your project already needs. Plain `clang` can target different Windows environments; its headers, system libraries and linker must match that target. These commands do not select `clang-cl`. Release optimization matters for rendering speed; the helper also enables optimization for several expensive core files in debug builds.

## Put it inside your own DLL instead

Replace the executable declaration with your plugin's target:

```cmake
# plugin.c implements the entry points which your host application calls.
add_library(my_overlay SHARED plugin.c)
```

Keep the remaining `target_sources`, include directories, definitions and link dependencies exactly as above. In particular, keep `MEEK_UI_STATIC`: meek-ui's implementation and its callers are inside this same DLL. Do not define `GUI_BUILDING_DLL` for this arrangement. Export your own plugin entry points as required by your host, for example with `__declspec(dllexport)` on Windows; meek-ui functions need not be exported.

Your Windows linker may produce an import `.lib` alongside your plugin DLL. That is a description of your plugin's exports, not a separate static build of meek-ui. Whether another program needs it depends on how that program loads your plugin.

Run initialization and drawing on the host's graphics thread after its graphics objects exist. Shut down before those objects or your plugin are unloaded. For a Windows plugin, perform graphics initialization through a host callback after loading, outside `DllMain`.

## Selecting files yourself, without the CMake helpers

These paths are relative to the meek-ui checkout. Add each selected `.c` exactly once to your executable or DLL's source list. Compile them as C, using C11 or newer.

| Group | Sources to compile |
|---|---|
| Shared core | All `.c` files directly in `src/`, `src/widgets/`, `src/clib/` and `src/third_party/`, **except** `src/clib/clib_rwlock.c`. That unused reference file depends on headers absent from this checkout. |
| Windows core addition | `backends/renderers/d3d11/effects_d3d11.c`, even when selecting another graphics backend. Shared composition references this optional effect implementation. Compiling it does not enable GPU (graphics processing unit) effects; activation remains explicit. |
| One renderer | `backends/renderers/<renderer>/<renderer>_renderer.c` |
| Matching adapter | `backends/embedded/embedded_<renderer>.c` |
| Embedded image atlas fallback | `backends/common/atlas_null.c`, including for the current embedded GLES adapter. An atlas is a texture containing multiple smaller images; this fallback uses separate image textures. |
| Windows file and input support | `backends/platform/win32/fs_win32.c` and `backends/embedded/embedded_win32_input.c` |
| Linux file support | `backends/platform/linux/fs_linux.c`; forward host input through the scene functions. |

Do not recursively add every source in `backends/`: renderer implementations share function names. Choose one renderer per binary containing meek-ui. The embedded recipe also excludes `platform_win32.c`, Linux window-loop implementations, standalone startup code, example/test entry points and `atlas_gles3.c`. Your host supplies the window and loop. Header-only third-party implementations are already instantiated by the core; no extra implementation macro is needed in your program.

The Windows Direct3D 11 selection, in addition to the shared core, is therefore:

```text
backends/renderers/d3d11/effects_d3d11.c
backends/renderers/d3d11/d3d11_renderer.c
backends/embedded/embedded_d3d11.c
backends/common/atlas_null.c
backends/platform/win32/fs_win32.c
backends/embedded/embedded_win32_input.c
```

Add `src`, `backends/common`, `backends/embedded`, the selected `backends/renderers/<renderer>` and your operating system's `backends/platform/<system>` to the header search paths. Preserve the subdirectories beneath them. Also retain the Windows effect source and its shader headers when selecting another Windows renderer. The helper's `MEEK_UI_INCLUDE_DIRS` supplies all current public backend include paths if you prefer to reuse it.

Apply the definitions from the CMake example to **every** meek-ui compilation unit and every host compilation unit using its headers, rather than defining them only at the top of `main.c`. With Clang, these are ordinary `-D` options. Add graphics development headers for the selected backend as needed.

## Other embedded backends

API means application programming interface. SDK means software development kit.

On Windows, change `overlay_renderer` in the example and include its matching adapter header. Keep `user32` and `gdi32`; the helper selects these additional graphics libraries:

| Selection | Header for your code | Additional Windows link dependencies |
|---|---|---|
| `d3d9` | `embedded_d3d9.h` | `d3d9`, `d3dcompiler` |
| `d3d11` | `embedded_d3d11.h` | `d3d11`, `dxgi`, `d3dcompiler`, `dxguid` |
| `d3d12` | `embedded_d3d12.h` | `d3d12`, `dxgi`, `d3dcompiler`, `dxguid` |
| `opengl2` | `embedded_opengl2.h` | `opengl32` |
| `opengl3` | `embedded_opengl3.h` | `opengl32` |
| `gles3` | `embedded_gles3.h` | Host-provided GLES implementation and headers; the helper finds `GLESv2` or `libGLESv2`. Windows host integration is not device-tested. |
| `vulkan` | `embedded_vulkan.h` | Vulkan headers and loader; CMake supplies `Vulkan::Vulkan`. |

For Linux, retain the core/embedded source helpers, include directories and the three meek-ui definitions. Replace the Windows-specific definitions and library selection with the dependencies of your graphics host. The current Linux examples use `gl`, `egl`, `x11`, `m` and `dl` for desktop OpenGL; `glesv2`, `egl`, `x11`, `m` and `dl` for GLES; and `Vulkan::Vulkan`, `X11::X11`, `m` and `dl` for Vulkan. X11 is the X Window System, version 11. EGL is Khronos's native graphics-context interface. These examples include window/context creation; an existing host can reuse its own window-system dependencies. See the [OpenGL](../../examples/embedded_opengl/CMakeLists.txt), [GLES](../../examples/embedded_gles3/CMakeLists.txt) and [Vulkan](../../examples/embedded_vulkan/CMakeLists.txt) build files. The Windows graphics-library helper is not a Linux dependency resolver.

Keep the selected renderer's `shaders/generated` files in their original location. Renderer sources include these headers directly; there are no shader `.c` files to add or external shader files to ship for the current generated-header paths. The Windows effect implementation has its own generated headers too. Regenerate them when editing their shader sources, using the renderer's documented shader workflow.

## Calling meek-ui and supplying assets

For a Direct3D 11 host, start with these headers:

```c
#include "embedded_d3d11.h"       /* Attach/detach and frame boundaries. */
#include "embedded_win32_input.h" /* Forward Windows input messages. */
#include "scene.h"               /* Create/find nodes, layout and events. */
#include "renderer.h"            /* Direct drawing of shapes and images. */
#include "font.h"                /* Register fonts. */
#include "widget_registry.h"     /* Register built-in widgets once. */
#include "clib/memory_manager.h"  /* Initialize tracked allocation accounting. */
```

Add `parser_xml.h` and `parser_style.h` for explicit loading, or `hot_reload.h` for file watching. CSS means Cascading Style Sheets; `.style` files use meek-ui's CSS-like language. Compiling C sources into your binary does not automatically package `.ui`, `.style`, image or font files. Supply those at runtime, use the parsing/font APIs with memory data, or use the supported [inline stylesheet resources](../../docs/inline-resources.md).

Initialize memory accounting and built-in widgets once. Attach the selected adapter to your existing graphics objects, load the interface and fonts, then update/layout/draw between its frame calls. Forward input, report current dimensions and handle resize/device replacement. Release scene/font/file-watching resources and detach before shutting down memory accounting. The [host sequence](README.md#host-sequence) and [per-API hook recipes](README.md#hook-integration-which-function-and-what-to-call-inside-it) show the actual calls and their ordering. Source integration changes how the program is built; these runtime contracts remain the same.
