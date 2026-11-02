#ifndef PLATFORM_LINUX_WAYLAND_CLIENT_H
#define PLATFORM_LINUX_WAYLAND_CLIENT_H

// platform_linux_wayland_client.h - Wayland-client-specific hooks.
// Most platform interaction goes through the uniform `platform.h`
// API (platform__init / tick / shutdown). This header is for code
// that needs the one thing the uniform API can't express: access
// to the live `wl_display*` so additional protocol extensions can
// be bound on the same connection.
// The canonical consumer today is meek-shell, which binds our
// private `meek_shell_v1` extension alongside the standard ones
// meek-ui's backend already handles. meek-shell doesn't need its
// own wl_display_connect -- sharing the existing one keeps us to
// a single protocol connection per process.
// ONLY the Wayland-client backend defines these. Code that links
// the X11 or DRM backends will see `platform_wayland__get_display`
// as an unresolved symbol at link time -- intentional; it's the
// signal that the Wayland-specific path isn't available on this
// build.

#include "gui_api.h"
#include "types.h"

struct wl_display;

GUI_API struct wl_display* platform_wayland__get_display(void);

GUI_API void*              platform_wayland__get_egl_display(void);
GUI_API void*              platform_wayland__get_egl_context(void);

GUI_API boole              platform_wayland__create_shared_egl_context(void** out_ctx);

GUI_API void               platform_wayland__request_render(void);

GUI_API boole              platform_wayland__get_output_pixel_size(int* out_w, int* out_h);

GUI_API boole              platform_wayland__get_output_mm_size(int* out_w, int* out_h);

GUI_API boole              platform_wayland__get_output_refresh_mhz(int* out_mhz);

GUI_API uint64             platform_wayland__frames_rendered(void);

#endif
