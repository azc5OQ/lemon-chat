#ifndef PLATFORM_H
#define PLATFORM_H

#include "types.h"
#include "gui_api.h"
#include "gui.h"

// platform.h - uniform platform interface.

// Window + renderer init parameters. Shared across platforms;
// previously lived in the now-deleted platform_win32.h /
// platform_android.h. All fields are required.
// title:        Window title. wchar_t* for native Win32 Unicode;
// ignored on Android (no OS-level title bar there).
// width/height: Initial client-area dimensions in pixels.
// clear_color:  Back-buffer clear color, applied by the renderer at
// the start of every frame.
// hidden:       TRUE creates the window without ever showing it. The
// renderer still draws into its back buffer and
// renderer__request_capture reads it back, so the visual
// test runner can run without a window appearing on the
// desktop. Win32: no WS_VISIBLE, never raised. macOS: never
// ordered front. X11: never mapped (GL may drop the pixels of
// an unmapped window; capture on an off-screen server such as
// Xvfb). Wayland: not supported, a warning is logged and the
// window is shown. Android and DRM have no windows to hide.
typedef struct gui_app_config_t
{
    const wchar_t* title;         // 0x0
    int64          width;         // 0x8
    int64          height;        // 0x10
    gui_color_t    clear_color;   // 0x18
    boole          hidden;        // 0x28
} gui_app_config_t;

// Every platform backend (Windows, Android, eventually Linux + macOS)
// implements the same four functions:
// platform__init      stand up window + renderer + input.
// platform__tick      one frame of event pump + render + present.
// platform__shutdown  tear everything down.
// Host apps include this header and use the unified names; the right
// backend is linked in by the build system, so main.c reads exactly
// the same on every target platform.
// ENTRY POINT
// -----------
// Windows uses `int main(int argc, char** argv)`. Android uses
// `void android_main(struct android_app*)` called by
// android_native_app_glue. These can't be unified at the symbol
// level, so platform.h renames the host's entry to `app_main`:
// #define main app_main
// The host writes `int main(int argc, char** argv)` as usual; the
// preprocessor rewrites it to `int app_main(...)`. The actual `int
// main()` (Windows) and `void android_main()` (Android) are
// provided by the platform backend and forward to `app_main`. On
// Android the backend also stashes the `struct android_app*` before
// dispatching, so `platform__init` can pick it up without the host
// having to touch the NDK glue struct.
// HOW TO USE
// ----------
// #include "platform.h"
// int main(int argc, char** argv)
// {
// gui_app_config_t cfg = { ... };
// if (!platform__init(&cfg)) return 1;
// // build scene...
// while (platform__tick()) { ... optional per-frame work ... }
// platform__shutdown();
// return 0;
// }
// Works unchanged on Windows and Android. The compiler / linker pick
// the right backend.

GUI_API boole platform__init(const gui_app_config_t* cfg);

GUI_API boole platform__tick(void);

GUI_API void  platform__shutdown(void);

GUI_API void  platform__set_topmost(void);

GUI_API boole platform__capture_bmp(const char* path);

#ifdef _WIN32
// Opt-in counterpart of the Wayland platform's render gate. Hosts that
// mutate scene state outside OS input call request_render after a change.
GUI_API void platform_win32__set_render_gating(boole enabled);
GUI_API void platform_win32__request_render(void);
#endif

// Rename the host's `main` to `app_main` so the platform backend's
// own `main` / `android_main` entry points can forward to it. The
// backend's entry-point .c file deliberately does NOT include this
// header, so the real `int main()` it defines isn't macro-replaced.
// Guarded by _PLATFORM_INTERNAL so the backend can include this
// header (to pick up the API decls) without tripping the rename.
// Also skipped when the host defines MEEK_UI_HOST_OWNS_MAIN: an
// embedded host keeps its own main and never calls platform__init.
#if !defined(_PLATFORM_INTERNAL) && !defined(MEEK_UI_HOST_OWNS_MAIN)
#define main app_main
#endif

#endif
