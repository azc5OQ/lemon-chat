# meek-ui

A C UI (user interface) toolkit that behaves like a web page inside your own renderer.
You describe a screen in an HTML (Hypertext Markup Language) style `.ui` file and use
CSS (Cascading Style Sheets) syntax in a `.style` file (or build the same tree in C), and meek-ui
lays it out, animates it, routes input, and draws it through the
graphics API (application programming interface) you already use: OpenGL 2, OpenGL 3.3, OpenGL ES (Embedded Systems) 3, Direct3D 9, 11,
12, or Vulkan. No scripting language, no code generation, no runtime
reflection.

It is meant to be dropped into an existing application the way you
would drop in Nuklear or Dear ImGui: a core library, one
renderer implementation and the appropriate platform or embedded adapter. It can also draw as an overlay inside
a frame that another program renders, see section 4.

Besides the HTML-shaped elements (labels, buttons, inputs, textarea,
checkbox, radio, select, images, scrolling containers, collections) it
has widgets HTML does not: a slider, a color picker, a date picker, a
memory view for debugging, a file picker, a canvas, an on-screen
keyboard, a page-stack navigation and modal popups, all usable from
`.ui` / `.style` files and from C. Elements fade in and out as they
scroll into and out of view when their style says so (`:appear` /
`:disappear`).

The layout follows CSS closely enough that a web browser rendering the
same scene is the reference the test suite compares against:
`docs/css-conformance.md` is the contract and documents the browser comparisons.
Layout comparisons require reference markers; some complex references, including
Motion Lab, do not have them. Renderer capture tests still exercise every scene,
including its scripted interaction states. Current platform coverage and measured
pixel agreement are recorded in `tests/visual/renderer-ports-performance.md`.

## Layout

| Directory | What is in it |
|---|---|
| `src/` | The core. Backend-neutral: scene tree, layout (`scene_layout.c`), style resolution, parsers, animator, fonts, scrolling, `widgets/`, the `clib/` libc wrappers, vendored `third_party/` headers. `meek_ui.h` is the single include, `meek_ui_config.h` documents the knobs. |
| `backends/renderers/<api>/` | Shared graphics API implementations, their atlas/effect code and `shaders/` sources with checked-in generated headers. |
| `backends/platform/<os>/` | OS window/event-loop and filesystem adapters; Windows application entry point. |
| `backends/common/` | Shared CPU helpers, rendering contract, null atlas and filesystem/startup helpers. |
| `backends/tools/` | Shader embedding and freshness checks. |
| `backends/embedded/` | Host-owned device/context adapters for Direct3D 9/11/12, OpenGL 2/3, OpenGL ES 3 and Vulkan. Each preserves host state or uses private command buffers. See its `README.md` for per-API requirements. |
| `examples/` | Standalone programs per platform, embedded overlay hosts, their shared `embedded_common/` scene, and demo assets in `examples/assets/`. |
| `fonts/` | TTF files the examples and tests register at startup. |
| `tests/visual/` | Visual regression harness: scenes, the test runner, `run_all.bat` / `run_all.sh`, the HTML twins of the scenes (`html/`), the browser layout check (`layout_check.py`), the report. `tests/README.md` is the map. |
| `docs/` | Contributor guides, the style property reference, the widget property matrix, the CSS conformance contract, design notes. |
| `cmake/meek_ui.cmake` | The single source list. Every build asks it which files to compile. |

## Integrating into your application

### 1. CMake

```cmake
add_subdirectory(path/to/meek-ui)          # builds the meek_ui target
target_link_libraries(myapp PRIVATE meek_ui)
```

Choose the renderer with `-DMEEK_UI_RENDERER=d3d11` (default `opengl3`
on Windows, `gles3` on Linux). `-DMEEK_UI_SHARED=OFF` produces a static
library instead of `meek_ui.dll`. Or skip the target and use the
helper functions directly:

```cmake
include(path/to/meek-ui/cmake/meek_ui.cmake)
meek_ui_core_sources(CORE)
meek_ui_backend_sources(BACKEND d3d11 win32)
meek_ui_renderer_link_libs(LIBS d3d11)
add_executable(myapp main.c ${CORE} ${BACKEND})
target_include_directories(myapp PRIVATE ${MEEK_UI_INCLUDE_DIRS})
target_compile_definitions(myapp PRIVATE MEEK_UI_STATIC UNICODE _UNICODE)
target_link_libraries(myapp PRIVATE user32 gdi32 ${LIBS})
```

### 2. Copy the files into any build system

Add these to your project. Include `src/`, `backends/common/`, the chosen
`backends/renderers/<api>/` and `backends/platform/<os>/`; embedded hosts also
include `backends/embedded/`. CMake supplies these through `MEEK_UI_INCLUDE_DIRS`:

- `src/*.c`, `src/widgets/*.c`, `src/clib/*.c`, `src/third_party/*.c`, excluding the unused `src/clib/clib_rwlock.c` reference (as CMake does)
- one renderer: `backends/renderers/<api>/<api>_renderer.c`
- the matching atlas: `backends/renderers/gles3/atlas_gles3.c` for `gles3`, `backends/common/atlas_null.c` for everything else
- one platform: `backends/platform/<os>/platform_<name>.c` plus `backends/platform/<os>/fs_<os>.c`
- on Windows, `backends/renderers/d3d11/effects_d3d11.c` supplies the optional shared effect engine
- on Windows, when the library owns the window: `backends/platform/win32/main_entry_win32.c` compiled into the executable
- for an overlay host: the matching `backends/embedded/embedded_<api>.c` and `backends/embedded/embedded_win32_input.c` instead of a platform file

Define `MEEK_UI_STATIC` (no DLL import thunks) and, on Windows,
`UNICODE` and `_UNICODE`. Windows link libraries per renderer:
`opengl32` / `d3d11 dxgi d3dcompiler dxguid` / `d3d9 d3dcompiler` /
`d3d12 dxgi d3dcompiler dxguid`; plus `user32 gdi32` always.

The library normally uses the `stdlib__` wrappers in `src/clib/stdlib.h`.
A constant-size `memcpy` in the CPU vector pixel loop is deliberately visible
to the compiler so it becomes an alias-safe unaligned load/store. They are not exported from the DLL, so a host
that links `meek_ui.dll` uses its own libc.

### 3. Host code when meek-ui owns the window

```c
#include "meek_ui.h"

int main(int argc, char** argv)
{
    gui_app_config_t cfg;
    cfg.title       = L"my app";
    cfg.width       = 800;
    cfg.height      = 600;
    cfg.clear_color = scene__hex(0x101010);
    cfg.hidden      = FALSE;                 /* TRUE: never show the window (test runs) */

    platform__init(&cfg);
    font__register_dir("path/to/fonts");     /* families are the file stems */
    hot_reload__watch_style("app.style");
    hot_reload__watch_ui("app.ui");
    while (platform__tick())
    {
        hot_reload__tick();
    }
    hot_reload__shutdown();
    platform__shutdown();
    return 0;
}
```

`platform.h` renames your `main` to `app_main`; the platform backend
provides the real entry point and forwards to it. Event handlers named
in `.ui` files (`on_click="save"`) are resolved by name from the host
executable, or registered explicitly with `scene__register_handler`.

### 4. Drawing inside a window, device and loop you already own

This is the overlay case: meek-ui inside a game's Direct3D or OpenGL
frame, a plugin in a host application, a managed host that hands over
its swap chain. Define `MEEK_UI_HOST_OWNS_MAIN` (keeps your `main`), do
not compile a `platform_*.c` file (`meek_ui_backend_sources(...
<renderer> none)`), compile the matching file from
`backends/embedded/`, and drive the pipeline yourself:

1. Attach the matching adapter with objects you own and keep. For example, `embedded_d3d11__attach(device, context, swapchain)`, `embedded_opengl2__attach()` with a current desktop compatibility context, or `embedded_vulkan__attach(&config)` with a borrowed graphics queue.
2. `widget_registry__bootstrap_builtins`, `font__init`, `font__register_dir`, then register handlers by name with `scene__register_handler`.
3. Load a scene with `hot_reload__watch_ui` / `parser_xml__load_ui` or build it with `scene__node_new`.
4. Per frame, after host drawing and before presentation, advance scene time and enter the adapter's frame boundary. Perform hot reload, style resolution, animation, layout and drawing inside that boundary. OpenGL resource uploads also belong inside it. Vulkan takes a frame descriptor with target image/view and synchronization details; see the adapter header. End the overlay frame, then let the host present.
5. In your window procedure call `embedded_win32__on_window_event(hwnd, msg, wparam, lparam)` first; it returns TRUE when the UI consumed the message.
6. On a swap chain resize call `embedded_d3d11__release_render_target` / `embedded_d3d12__release_render_targets` before `ResizeBuffers`; on d3d9 wrap the host's `Reset` with `on_device_lost` / `on_device_reset`.

`backends/embedded/README.md` has the full host sequence, and
`examples/embedded_*/` are complete hosts that render their own content
and composite the overlay from `examples/embedded_common/overlay.ui`.
Injecting the overlay into a program that was not built for it (DLL
injection, hooking `Present`) is not part of the toolkit.

## Building the examples and tests

Presets exist for Clang and for the w64devkit GCC toolchain, one build
directory per renderer:

```bash
cmake --preset windows-gcc-debug-d3d11
cmake --build --preset windows-gcc-debug-d3d11
build\gcc-debug-d3d11\example_win32.exe
```

Then:

- `tests\visual\run_all.bat` renders every scene in `tests/visual/scenes/` through every renderer you built, with hidden windows (`HIDDEN=1` near its top; 0 shows them), and writes `tests/visual/report.html` with a browser rendering of each scene next to the captures.
- `python tests\visual\layout_check.py` compares every node's rectangle with the one Microsoft Edge computes for the scene's HTML twin and exits 0 when all scenes agree within 1 px.

On Linux configure with plain CMake from this directory
(`-DMEEK_UI_LINUX_BACKEND=x11`) and run `tests/visual/run_all.sh`; each
example directory also has a standalone `build.sh`.

## Configuration

All knobs are macros, documented in `src/meek_ui_config.h`:

| Macro | Effect |
|---|---|
| `MEEK_UI_STATIC` | No dllimport / dllexport; use for static or copied-in builds. |
| `GUI_BUILDING_DLL` | Set by the library's own DLL build only. |
| `MEEK_UI_HOST_OWNS_MAIN` | Keep your own `main`; for embedded hosts. |
| `GUI_TRACK_ALLOCATIONS` | Leak and per-type allocation reports on shutdown. |
| `GUI_FONTS_SOURCE_DIR` | Optional directory `font__init` scans; prefer `font__register_dir`. |
| `MEEK_UI_OPENGL_SRGB_FRAMEBUFFER` | Opt-in sRGB framebuffer for opengl3. Off by default so opengl3 blends in sRGB space like the Direct3D backends and browsers. |
| `MEEK_DBG_*` | Gated debug logging, see `src/debug_definitions.h`. |

## Backend status

| Renderer | Rects, borders, shadows | Text | Images | Backdrop blur | Frame capture | Overlay backend | Atlas batching |
|---|---|---|---|---|---|---|---|
| opengl2 | CPU | CPU | CPU | CPU | yes | `embedded_opengl2` | no |
| opengl3 | yes | yes | yes | yes | yes | `embedded_opengl3` | no (`atlas_null`) |
| gles3 | yes | yes | yes | yes | yes | `embedded_gles3` | standalone only |
| d3d11 | yes | yes | yes | yes | yes | `embedded_d3d11` | no |
| d3d9 | yes | yes | yes | yes | yes | `embedded_d3d9` | no |
| d3d12 | yes | yes | yes | yes | yes | `embedded_d3d12` | no |
| vulkan | CPU | CPU | CPU | CPU | yes | `embedded_vulkan` | no |

CPU means central processing unit. OpenGL 2, Vulkan and embedded GLES3 use the
portable painter for the complete public drawing contract, then upload the
finished image. This does not claim native GPU (graphics processing unit) blur
or effect pipelines for those APIs. Optional Direct3D 11 effect acceleration is
shared on Windows and remains explicitly selected by the application.

The yellow FPS (frames per second) counter is available through `--fps` in
standard-entry programs, `renderer__parse_launch_flags` in custom hosts, or
`renderer__set_debug_flags(GUI_RENDERER_DEBUG__FPS)`. See
[renderer diagnostics](docs/renderer-diagnostics.md).

The four Windows renderers produce identical pixels for every scene in
the sweep (the maximum channel difference between any two of them is
1 on a handful of blur pixels). Frame capture is the renderer reading
its own back buffer (`renderer__request_capture`), which is what the
test harness saves; it works with a hidden window.

Platforms: `win32`, `linux_x11`, `linux_drm` (kiosk, no window
system), `linux_wayland` (regular Wayland client), `android`, `macos`,
and `none` for embedded hosts.

## Where to read next

- `docs/css-conformance.md` for what the layout and text engine promise relative to CSS, the deliberate differences, and how the browser comparison is run.
- [Architecture walkthrough](docs/architecture.md) follows a pixel from UI/style loading to presentation, with terms explained as they appear.
- `docs/style-properties.md` and `docs/widget-property-matrix.md` for the markup and style language.
- `docs/code-style.md` for how the C is written (measured from lemon-chat, the style reference).
- `docs/contributing/` for adding widgets, style rules, and running tests; `tests/README.md` for the test map.
- `backends/embedded/README.md` for the overlay backends.
