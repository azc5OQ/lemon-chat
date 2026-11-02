// platforms/linux/platform_linux_wayland_client.c - Wayland-client backend.
// Third Linux backend alongside platform_linux_x11.c (nested in an
// X session) and platform_linux_drm.c (kiosk / no display manager).
// This one runs the gui toolkit as a regular Wayland CLIENT: it
// connects to whatever compositor is already up (any standard
// Wayland compositor, including meek-compositor), asks for a window
// via xdg-shell, and renders into an EGL window surface.
// WHY THIS EXISTS:
// Most modern Linux desktops run Wayland compositors natively.
// Going through XWayland's X11 translation (which is what
// platform_linux_x11.c does under Wayland) means an extra bounce
// + loses features the compositor only exposes natively (fractional
// scaling, proper fullscreen, xdg_toplevel state events). It also
// unblocks meek-shell: meek-shell uses meek-ui as a library and
// needs to present through Wayland to talk to meek-compositor.
// WHAT IS xdg-shell?
// "xdg" = cross-desktop group (freedesktop.org). The base Wayland
// protocol has no notion of a window with a title, min/maximize,
// fullscreen, etc. xdg-shell is the standard extension that every
// compositor implements for application-level window management.
// Without binding xdg_wm_base we'd have a surface that the
// compositor doesn't know what to do with.
// RENDERER CHOICE:
// Same gles3_renderer.c every other Linux / Android backend uses.
// Context-neutral so we just hand it the current EGL context.
// WHAT'S IN THIS PASS (B1 MVP):
// - Connect, bind wl_compositor + xdg_wm_base + wl_output.
// - Create a wl_surface, xdg_surface, xdg_toplevel.
// - Initial empty commit -> wait for configure -> ack.
// - wl_egl_window + eglCreateWindowSurface on it (Mesa handles
// buffer allocation + attach+commit internally via
// eglSwapBuffers, which is why we don't hand-roll dmabuf
// submission here).
// - Main loop: dispatch + render + swap.
// - xdg_toplevel.close -> should_close. Ping/pong for liveness.
// DEFERRED (B1.2+):
// - Input: wl_seat / wl_pointer / wl_keyboard / wl_touch.
// - Keyboard layout via libxkbcommon.
// - Fractional scaling via wp_fractional_scale_v1.
// - Explicit dmabuf submission + linux-dmabuf v1 bind. Not needed
// as long as Mesa's wl_egl_window path works (it does on every
// compositor we care about).

// clock_gettime is a POSIX interface, hidden by glibc in strict C11 mode.
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <poll.h> // non-blocking wl_display_read_events drain in tick loop.
#include <dlfcn.h> // dlsym(RTLD_DEFAULT, ...) for UI_HANDLER symbol resolution.
#include <sys/mman.h> // mmap the keymap fd the compositor sends us.
#include <sys/stat.h> // stat() for one-shot screenshot trigger.

#include <xkbcommon/xkbcommon.h> // translate wl_keyboard.key events to codepoints.

#include <wayland-client.h>
#include <wayland-egl.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

// scanner-generated client-side bindings. CMakeLists.txt runs
// wayland-scanner on /usr/share/wayland-protocols/stable/xdg-shell/
// xdg-shell.xml with `client-header` + `private-code`, depositing
// these under the demo's build dir. The demo's include path
// exposes them.
#include "xdg-shell-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"

#include "types.h"
#include "gui.h"
#include "scene.h"
#include "animator.h"
#include "renderer.h"
#include "widget_registry.h"
#include "widgets/widget_image_cache.h"
#include "font.h"
#include "fs.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "third_party/log.h"

#define _PLATFORM_INTERNAL
#include "platform.h"
#undef _PLATFORM_INTERNAL

#include "platform_linux_wayland_client.h"

// Emit the real int main() that forwards to the host's (renamed)
// app_main. Without this the linker errors with "undefined reference
// to main" when musl's Scrt1.o tries to call it. Matches the pattern
// in platform_linux_drm.c / platform_linux_x11.c.
#include "_main_trampoline.h"
#include "clib/stdlib.h"
GUI_DEFINE_MAIN_TRAMPOLINE()

// ============================================================================
// state
// ============================================================================

typedef struct _platform_linux_wayland_client_internal__state_t
{
    // Wayland-level handles. wl_display owns the socket; the rest
    // are per-global references obtained during the registry burst.
    struct wl_display*    display;                              // 0x0
    struct wl_registry*   registry;                             // 0x8
    struct wl_compositor* compositor;                           // 0x10
    struct xdg_wm_base*   wm_base;                              // 0x18
    // optional; advertised size / scale.
    struct wl_output*     output;                               // 0x20

    // Captured wl_output.mode pixel dimensions. Phones / single-output
    // setups latch the active mode here. Both 0 until the compositor
    // sends the first wl_output.mode event with the WL_OUTPUT_MODE_CURRENT
    // bit set; consumers (e.g. meek-shell's gesture_recognizer__init,
    // which needs panel-native pixels for edge-zone classification)
    // should fall back to cfg width/height when these are still 0.
    int output_pixel_w;                                         // 0x28
    int output_pixel_h;                                         // 0x2C
    int output_mm_w;                                            // 0x30
    int output_mm_h;                                            // 0x34
    // Refresh rate in mHz from the wl_output.mode event (Wayland
    // advertises rates as mHz to capture 59.940 / 60.000 / 120.000
    // distinctions without floats). 0 means "never received a
    // current-mode event"; callers should treat that as unknown.
    int output_refresh_mhz;                                     // 0x38
    // Monotonic count of REAL rendered frames -- increments after
    // every eglSwapBuffers call, NOT on every platform__tick.
    // platform__tick gets called for many wayland events that don't
    // result in a render (touch, configure, frame callbacks), so a
    // FPS counter that ticks on platform__tick reads "events/sec"
    // rather than "frames/sec." Consumers diff the value over a
    // window to get true rendered FPS.
    uint64 frames_rendered;                                     // 0x40

    // zwp_idle_inhibit_manager_v1: tells the compositor "don't
    // blank the screen while my surface is visible". Optional --
    // if the compositor doesn't advertise it we fall through
    // silently (the user's session will just auto-blank). The
    // manager is bound at registry time; the actual inhibitor
    // object is created per-surface once the xdg_surface is up.
    struct zwp_idle_inhibit_manager_v1* idle_inhibit_manager;   // 0x48
    struct zwp_idle_inhibitor_v1*       idle_inhibitor;         // 0x50

    // text-input-unstable-v3 (per-seat IME-aware focus signal).
    // We bind the manager at registry time; once the seat is up,
    // we ask the manager for our own zwp_text_input_v3 object.
    // scene_input's text-input focus hook then calls enable() on
    // the object when an <input>/<textarea> takes focus, and the
    // compositor turns that into a meek_shell_v1.ime_request_on
    // event so the SHELL shows its on-screen keyboard.
    // Without this, focusing an input in a meek-ui app silently
    // does nothing -- the in-tree widget_keyboard wouldn't show
    // either, since scene_input's text_input hook is now the
    // gate that decides between "platform handles the keyboard"
    // and "the app draws its own keyboard."
    struct zwp_text_input_manager_v3* text_input_manager;       // 0x58
    struct zwp_text_input_v3*         text_input;               // 0x60
    int                                text_input_active;       // 0x68
    // 1 after enter event from compositor
    int                                text_input_entered;      // 0x6C

    // Edge-swipe gesture state. We track per-touch start position so
    // wl_touch.up can classify the gesture as a left-edge → right
    // swipe and dispatch the named handler "on_swipe_back" via
    // scene__dispatch_gesture_by_name. Any meek-ui app can bind to
    // that gesture (the settings app pops its <navigation> stack on
    // it). One-finger only; multi-touch swipes deferred.
    int64 swipe_start_x;                                        // 0x70
    int64 swipe_start_y;                                        // 0x78

    // Input: wl_seat wraps pointer + keyboard + touch. Each
    // capability is optional; we grab each one when the seat
    // advertises it. On a typical phone only touch + keyboard
    // are present (phone has no physical mouse); on dev desktop
    // pointer + keyboard. Tracking the last pointer/touch
    // position here so we can synthesise release events at the
    // correct spot when wl_touch.up fires (which carries no
    // coordinates of its own).
    struct wl_seat*     seat;                                   // 0x80
    struct wl_pointer*  pointer;                                // 0x88
    struct wl_touch*    touch;                                  // 0x90
    struct wl_keyboard* keyboard;                               // 0x98
    int64               last_x;                                 // 0xA0
    int64               last_y;                                 // 0xA8
    // -1 when no finger down
    int32_t             touch_active_id;                        // 0xB0
    int64               touch_x;                                // 0xB8
    int64               touch_y;                                // 0xC0

    // xkbcommon state. Compositor sends a keymap via
    // wl_keyboard.keymap(XKB_V1, fd, size); we mmap the fd, parse
    // with xkb_keymap_new_from_string, and use the resulting
    // keymap + state to translate incoming wl_keyboard.key
    // keycodes into unicode codepoints + VK_ values.
    // Lifecycle: xkb_context is created once at platform init, kept
    // until shutdown. xkb_keymap + xkb_state are re-created on
    // every wl_keyboard.keymap event (the compositor may swap
    // layouts at runtime). NULL when no keymap has arrived yet.
    struct xkb_context* xkb_ctx;                                // 0xC8
    struct xkb_keymap*  xkb_keymap;                             // 0xD0
    struct xkb_state*   xkb_state;                              // 0xD8

    // Our window. xdg-shell is a "role" layered on top of
    // wl_surface: you create a surface first, then wrap it as an
    // xdg_surface, then pick a role (toplevel here). Closing
    // happens via xdg_toplevel.close arriving; destroying the
    // toplevel also destroys its children down the tree.
    struct wl_surface*    wl_surface;                           // 0xE0
    struct xdg_surface*   xdg_surface;                          // 0xE8
    struct xdg_toplevel*  xdg_toplevel;                         // 0xF0

    // EGL backing. wl_egl_window wraps our wl_surface so EGL can
    // hand Mesa a buffer to draw into; Mesa then attach+commits
    // to the compositor inside eglSwapBuffers. We never manually
    // call wl_surface_attach -- that would duplicate what EGL
    // already did.
    struct wl_egl_window* egl_window;                           // 0xF8
    EGLDisplay            egl_display;                          // 0x100
    EGLContext            egl_context;                          // 0x108
    EGLSurface            egl_surface;                          // 0x110
    EGLConfig             egl_config;                           // 0x118

    // Lifecycle flags. `configured` flips TRUE on the first
    // xdg_surface.configure -- until then we don't render. Before
    // configure, rendering would attach a buffer to a surface
    // whose role hasn't been confirmed yet, which is a protocol
    // error on strict compositors.
    int  viewport_w;                                            // 0x120
    int  viewport_h;                                            // 0x124
    boole should_close;                                         // 0x128
    boole configured;                                           // 0x129

    gui_color_t clear_color;                                      // 0x12C

    // Render-gating state (see comment block above platform__tick
    // for the design). force_render is 1 when something has
    // happened that obliges us to render the next tick (input
    // event, configure resize, hot reload, meek_shell_v1 event
    // arriving via platform_wayland__request_render). The gate
    // also keeps rendering while animator__has_active() returns 1
    // (a fade / transition is in flight) -- that's checked at
    // the gate inline rather than via this struct.
    // last_activity_ms is the CLOCK_MONOTONIC ms timestamp of the
    // most recent activity (input event, animator-driven render,
    // foreign-app buffer arrival, hot reload). The poll timeout
    // uses this for hysteresis: until ACTIVITY_TIMEOUT_MS has
    // passed with no activity, we treat the system as ACTIVE and
    // poll non-blockingly so animations + rapid input bursts run
    // at full vblank rate AND the render block runs on every tick
    // (because some animator types -- transitions, auto-repeat --
    // aren't caught by animator__has_active and need an
    // unconditional tick to advance). After the activity window
    // ages out we drop into IDLE mode and may skip both the render
    // block and block in poll for IDLE_TIMEOUT_MS so the loop
    // doesn't busy-spin while waiting for input.
    // gating_disabled mirrors the MEEK_RENDER_GATING env var; if
    // "off", we render every tick like before Phase 2 (kill switch).
    int force_render;                                           // 0x13C
    int gating_disabled;                                        // 0x140
    int64 last_activity_ms;                                     // 0x148
} _platform_linux_wayland_client_internal__state_t;

static _platform_linux_wayland_client_internal__state_t _platform_linux_wayland_client_internal__g;

// ============================================================================
// forward decls
// ============================================================================

static boole _platform_linux_wayland_client_internal__open_display(void);
static void  _platform_linux_wayland_client_internal__close_display(void);
static boole _platform_linux_wayland_client_internal__bind_globals(void);
static boole _platform_linux_wayland_client_internal__make_toplevel(const gui_app_config_t* cfg);
static boole _platform_linux_wayland_client_internal__init_egl(void);
static void  _platform_linux_wayland_client_internal__term_egl(void);
static gui_handler_fn _platform_linux_wayland_client_internal__resolve_host_symbol(char* name);

// -- render gating (Phase 2): force_render setter
static void _platform_linux_wayland_client_internal__force_render_now(void);

// -- wl_output listener (current-mode pixel dimensions capture).
static void _platform_linux_wayland_client_internal__on_output_geometry( void* data, struct wl_output* o, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char* make, const char* model, int32_t transform);
static void _platform_linux_wayland_client_internal__on_output_mode( void* data, struct wl_output* o, uint32_t flags, int32_t width, int32_t height, int32_t refresh);
static void _platform_linux_wayland_client_internal__on_output_done( void* data, struct wl_output* o);
static void _platform_linux_wayland_client_internal__on_output_scale( void* data, struct wl_output* o, int32_t factor);

// Listener callbacks. All Wayland protocol events are dispatched
// through registered listener structs of function pointers --
// similar in shape to wl_resource vtables on the compositor side.
static void _platform_linux_wayland_client_internal__on_registry_global( void* data, struct wl_registry* r, uint32_t name, const char* interface, uint32_t version);
static void _platform_linux_wayland_client_internal__on_registry_global_remove( void* data, struct wl_registry* r, uint32_t name);

static void _platform_linux_wayland_client_internal__on_wm_base_ping( void* data, struct xdg_wm_base* wm, uint32_t serial);

static void _platform_linux_wayland_client_internal__on_xdg_surface_configure( void* data, struct xdg_surface* s, uint32_t serial);

static void _platform_linux_wayland_client_internal__on_toplevel_configure( void* data, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* states);
static void _platform_linux_wayland_client_internal__on_toplevel_close( void* data, struct xdg_toplevel* t);
static void _platform_linux_wayland_client_internal__on_toplevel_configure_bounds( void* data, struct xdg_toplevel* t, int32_t w, int32_t h);
static void _platform_linux_wayland_client_internal__on_toplevel_wm_capabilities( void* data, struct xdg_toplevel* t, struct wl_array* caps);

// -- seat/pointer/touch
static void _platform_linux_wayland_client_internal__on_seat_capabilities(void* data, struct wl_seat* s, uint32_t caps);
static void _platform_linux_wayland_client_internal__on_seat_name(void* data, struct wl_seat* s, const char* name);

static void _platform_linux_wayland_client_internal__on_pointer_enter(void* data, struct wl_pointer* p, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y);
static void _platform_linux_wayland_client_internal__on_pointer_leave(void* data, struct wl_pointer* p, uint32_t serial, struct wl_surface* s);
static void _platform_linux_wayland_client_internal__on_pointer_motion(void* data, struct wl_pointer* p, uint32_t time, wl_fixed_t x, wl_fixed_t y);
static void _platform_linux_wayland_client_internal__on_pointer_button(void* data, struct wl_pointer* p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state);
static void _platform_linux_wayland_client_internal__on_pointer_axis(void* data, struct wl_pointer* p, uint32_t time, uint32_t axis, wl_fixed_t value);
static void _platform_linux_wayland_client_internal__on_pointer_frame(void* data, struct wl_pointer* p);
static void _platform_linux_wayland_client_internal__on_pointer_axis_source(void* data, struct wl_pointer* p, uint32_t source);
static void _platform_linux_wayland_client_internal__on_pointer_axis_stop(void* data, struct wl_pointer* p, uint32_t time, uint32_t axis);
static void _platform_linux_wayland_client_internal__on_pointer_axis_discrete(void* data, struct wl_pointer* p, uint32_t axis, int32_t discrete);

static void _platform_linux_wayland_client_internal__on_touch_down(void* data, struct wl_touch* t, uint32_t serial, uint32_t time, struct wl_surface* s, int32_t id, wl_fixed_t x, wl_fixed_t y);
static void _platform_linux_wayland_client_internal__on_touch_up(void* data, struct wl_touch* t, uint32_t serial, uint32_t time, int32_t id);
static void _platform_linux_wayland_client_internal__on_touch_motion(void* data, struct wl_touch* t, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y);
static void _platform_linux_wayland_client_internal__on_touch_frame(void* data, struct wl_touch* t);
static void _platform_linux_wayland_client_internal__on_touch_cancel(void* data, struct wl_touch* t);
static void _platform_linux_wayland_client_internal__on_touch_shape(void* data, struct wl_touch* t, int32_t id, wl_fixed_t major, wl_fixed_t minor);
static void _platform_linux_wayland_client_internal__on_touch_orientation(void* data, struct wl_touch* t, int32_t id, wl_fixed_t orientation);

static void _platform_linux_wayland_client_internal__on_keyboard_keymap(void* data, struct wl_keyboard* k, uint32_t format, int32_t fd, uint32_t size);
static void _platform_linux_wayland_client_internal__on_keyboard_enter(void* data, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s, struct wl_array* keys);
static void _platform_linux_wayland_client_internal__on_keyboard_leave(void* data, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s);
static void _platform_linux_wayland_client_internal__on_keyboard_key(void* data, struct wl_keyboard* k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
static void _platform_linux_wayland_client_internal__on_keyboard_modifiers(void* data, struct wl_keyboard* k, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
static void _platform_linux_wayland_client_internal__on_keyboard_repeat_info(void* data, struct wl_keyboard* k, int32_t rate, int32_t delay);

// ============================================================================
// listener tables
// ============================================================================

static const struct wl_registry_listener _platform_linux_wayland_client_internal__registry_listener = {
    .global        = _platform_linux_wayland_client_internal__on_registry_global,
    .global_remove = _platform_linux_wayland_client_internal__on_registry_global_remove,
};

static const struct xdg_wm_base_listener _platform_linux_wayland_client_internal__wm_base_listener = {
    .ping = _platform_linux_wayland_client_internal__on_wm_base_ping,
};

static const struct xdg_surface_listener _platform_linux_wayland_client_internal__xdg_surface_listener = {
    .configure = _platform_linux_wayland_client_internal__on_xdg_surface_configure,
};

static const struct xdg_toplevel_listener _platform_linux_wayland_client_internal__toplevel_listener = {
    .configure         = _platform_linux_wayland_client_internal__on_toplevel_configure,
    .close             = _platform_linux_wayland_client_internal__on_toplevel_close,
    .configure_bounds  = _platform_linux_wayland_client_internal__on_toplevel_configure_bounds,
    .wm_capabilities   = _platform_linux_wayland_client_internal__on_toplevel_wm_capabilities,
};

static const struct wl_seat_listener _platform_linux_wayland_client_internal__seat_listener = {
    .capabilities = _platform_linux_wayland_client_internal__on_seat_capabilities,
    .name         = _platform_linux_wayland_client_internal__on_seat_name,
};

static const struct wl_pointer_listener _platform_linux_wayland_client_internal__pointer_listener = {
    .enter         = _platform_linux_wayland_client_internal__on_pointer_enter,
    .leave         = _platform_linux_wayland_client_internal__on_pointer_leave,
    .motion        = _platform_linux_wayland_client_internal__on_pointer_motion,
    .button        = _platform_linux_wayland_client_internal__on_pointer_button,
    .axis          = _platform_linux_wayland_client_internal__on_pointer_axis,
    .frame         = _platform_linux_wayland_client_internal__on_pointer_frame,
    .axis_source   = _platform_linux_wayland_client_internal__on_pointer_axis_source,
    .axis_stop     = _platform_linux_wayland_client_internal__on_pointer_axis_stop,
    .axis_discrete = _platform_linux_wayland_client_internal__on_pointer_axis_discrete,
};

static const struct wl_touch_listener _platform_linux_wayland_client_internal__touch_listener = {
    .down        = _platform_linux_wayland_client_internal__on_touch_down,
    .up          = _platform_linux_wayland_client_internal__on_touch_up,
    .motion      = _platform_linux_wayland_client_internal__on_touch_motion,
    .frame       = _platform_linux_wayland_client_internal__on_touch_frame,
    .cancel      = _platform_linux_wayland_client_internal__on_touch_cancel,
    .shape       = _platform_linux_wayland_client_internal__on_touch_shape,
    .orientation = _platform_linux_wayland_client_internal__on_touch_orientation,
};

static const struct wl_output_listener _platform_linux_wayland_client_internal__output_listener = {
    .geometry = _platform_linux_wayland_client_internal__on_output_geometry,
    .mode     = _platform_linux_wayland_client_internal__on_output_mode,
    .done     = _platform_linux_wayland_client_internal__on_output_done,
    .scale    = _platform_linux_wayland_client_internal__on_output_scale,
};

static const struct wl_keyboard_listener _platform_linux_wayland_client_internal__keyboard_listener = {
    .keymap      = _platform_linux_wayland_client_internal__on_keyboard_keymap,
    .enter       = _platform_linux_wayland_client_internal__on_keyboard_enter,
    .leave       = _platform_linux_wayland_client_internal__on_keyboard_leave,
    .key         = _platform_linux_wayland_client_internal__on_keyboard_key,
    .modifiers   = _platform_linux_wayland_client_internal__on_keyboard_modifiers,
    .repeat_info = _platform_linux_wayland_client_internal__on_keyboard_repeat_info,
};

// ============================================================================
// registry handlers
// ============================================================================

// Fires once per global the compositor advertises. We look for
// the names we care about and bind them. Everything else is
// ignored -- compositors advertise dozens of extensions we don't
// use.
static void _platform_linux_wayland_client_internal__on_registry_global( void* data, struct wl_registry* r, uint32_t name, const char* interface, uint32_t version)
{
    (void)data;
    if (stdlib__strcmp(interface, wl_compositor_interface.name) == 0)
    {
        // Cap at 4: versions 5+ add wl_surface.offset + damage_buffer
        // behavior we don't depend on yet.
        uint32_t v = version < 4 ? version : 4;
        _platform_linux_wayland_client_internal__g.compositor =
            wl_registry_bind(r, name, &wl_compositor_interface, v);
    }
    else if (stdlib__strcmp(interface, xdg_wm_base_interface.name) == 0)
    {
        // Cap at 5 to match meek-compositor; most compositors in
        // the wild are on 5 or 6.
        uint32_t v = version < 5 ? version : 5;
        _platform_linux_wayland_client_internal__g.wm_base =
            wl_registry_bind(r, name, &xdg_wm_base_interface, v);
        xdg_wm_base_add_listener(_platform_linux_wayland_client_internal__g.wm_base, &_platform_linux_wayland_client_internal__wm_base_listener, NULL_POINTER);
    }
    else if (stdlib__strcmp(interface, wl_output_interface.name) == 0)
    {
        // Cap at 2 (first version with scale events; newer versions
        // add name/description we don't consume). Listener latches
        // the current mode's pixel dimensions for downstream
        // consumers via platform_wayland__get_output_pixel_size.
        uint32_t v = version < 2 ? version : 2;
        _platform_linux_wayland_client_internal__g.output =
            wl_registry_bind(r, name, &wl_output_interface, v);
        wl_output_add_listener(_platform_linux_wayland_client_internal__g.output, &_platform_linux_wayland_client_internal__output_listener, NULL_POINTER);
    }
    else if (stdlib__strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name) == 0)
    {
        // Idle-inhibit v1 has only ever been v1. Capping anyway in
        // case the compositor advertises a future revision -- we
        // implement v1's surface, so bind v1.
        uint32_t v = version < 1 ? version : 1;
        _platform_linux_wayland_client_internal__g.idle_inhibit_manager =
            wl_registry_bind(r, name, &zwp_idle_inhibit_manager_v1_interface, v);
        log_info("%s", "platform_linux_wayland_client: zwp_idle_inhibit_manager_v1 available (screen-blank inhibit supported)");
    }
    else if (stdlib__strcmp(interface, wl_seat_interface.name) == 0)
    {
        // wl_seat v7 adds the modern axis_value120 event for
        // smooth scrolling. We only need v5 (axis_discrete) but
        // bind up to v7 if offered; compositor sends us what it
        // supports, client handlers ignore events we didn't
        // register for.
        uint32_t v = version < 7 ? version : 7;
        _platform_linux_wayland_client_internal__g.seat =
            wl_registry_bind(r, name, &wl_seat_interface, v);
        wl_seat_add_listener(_platform_linux_wayland_client_internal__g.seat, &_platform_linux_wayland_client_internal__seat_listener, NULL_POINTER);
        _platform_linux_wayland_client_internal__g.touch_active_id = -1;
        log_info("%s %u %s", "platform_linux_wayland_client: wl_seat v", v, "bound");
    }
    else if (stdlib__strcmp(interface, zwp_text_input_manager_v3_interface.name) == 0)
    {
        // text-input-unstable-v3 has only ever been v1 in the wild;
        // cap to 1 defensively.
        uint32_t v = version < 1 ? version : 1;
        _platform_linux_wayland_client_internal__g.text_input_manager =
            wl_registry_bind(r, name, &zwp_text_input_manager_v3_interface, v);
        log_info("%s %u %s", "platform_linux_wayland_client: zwp_text_input_manager_v3 v", v, "bound");
    }
}

static void _platform_linux_wayland_client_internal__on_registry_global_remove( void* data, struct wl_registry* r, uint32_t name)
{
    (void)data; (void)r; (void)name;
    // Compositor tore down a global. If it's wl_output that means
    // a monitor was unplugged; we'd want to re-query size. Not
    // handled in this MVP.
}

// ============================================================================
// ping / pong liveness
// ============================================================================

// Compositor pings every few seconds to check we haven't frozen.
// Reply immediately; if we don't, the compositor may kill us.
static void _platform_linux_wayland_client_internal__on_wm_base_ping( void* data, struct xdg_wm_base* wm, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm, serial);
}

// ============================================================================
// xdg_surface configure (always ack immediately)
// ============================================================================

// Compositor sends xdg_surface.configure(serial) after we commit
// the xdg_surface, to say "here's the state you should be in;
// acknowledge then commit an actual buffer." We always ack right
// away; toplevel-specific state (size) was already processed via
// xdg_toplevel.configure which fires right before this one.
static void _platform_linux_wayland_client_internal__on_xdg_surface_configure( void* data, struct xdg_surface* s, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(s, serial);
    _platform_linux_wayland_client_internal__g.configured = TRUE;
}

// ============================================================================
// xdg_toplevel events
// ============================================================================

// Size hint from the compositor. w == 0 || h == 0 means "pick
// your own" (typical on first configure before we've told it
// anything). Resize our EGL backing buffer if the size changed.
static void _platform_linux_wayland_client_internal__on_toplevel_configure( void* data, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* states)
{
    (void)data; (void)t; (void)states;

    if (w > 0 && h > 0 && (w != _platform_linux_wayland_client_internal__g.viewport_w || h != _platform_linux_wayland_client_internal__g.viewport_h))
    {
        _platform_linux_wayland_client_internal__g.viewport_w = w;
        _platform_linux_wayland_client_internal__g.viewport_h = h;
        if (_platform_linux_wayland_client_internal__g.egl_window != NULL_POINTER)
        {
            // wl_egl_window_resize nudges Mesa to allocate new
            // backing buffers at the next eglSwapBuffers. Offsets
            // are for sub-surface positioning which we don't use.
            wl_egl_window_resize(_platform_linux_wayland_client_internal__g.egl_window, w, h, /*dx*/ 0, /*dy*/ 0);
        }
        // Wake the render loop -- a resize means layout will change
        // and we owe the compositor a frame at the new dimensions
        // even if nothing else marked us dirty.
        _platform_linux_wayland_client_internal__g.force_render = 1;
    }
}

static void _platform_linux_wayland_client_internal__on_toplevel_close( void* data, struct xdg_toplevel* t)
{
    (void)data; (void)t;
    log_info("%s", "platform_linux_wayland_client: compositor requested close");
    _platform_linux_wayland_client_internal__g.should_close = TRUE;
}

// configure_bounds was added in v4 -- the compositor's hint about
// the maximum size we could sanely request (output size minus
// decorations / panels). We ignore it for now.
static void _platform_linux_wayland_client_internal__on_toplevel_configure_bounds( void* data, struct xdg_toplevel* t, int32_t w, int32_t h)
{
    (void)data; (void)t; (void)w; (void)h;
}

// wm_capabilities (v5+) tells us which title-bar-menu actions the
// compositor supports (maximize / minimize / fullscreen / menu).
// We don't render our own decorations yet; ignore.
static void _platform_linux_wayland_client_internal__on_toplevel_wm_capabilities( void* data, struct xdg_toplevel* t, struct wl_array* caps)
{
    (void)data; (void)t; (void)caps;
}

// ============================================================================
// connection + registry
// ============================================================================

static boole _platform_linux_wayland_client_internal__open_display(void)
{
    // NULL passes $WAYLAND_DISPLAY / $XDG_RUNTIME_DIR to libwayland.
    _platform_linux_wayland_client_internal__g.display = wl_display_connect(NULL_POINTER);
    if (_platform_linux_wayland_client_internal__g.display == NULL_POINTER)
    {
        log_error("%s %s %s", "wl_display_connect failed (is WAYLAND_DISPLAY set? XDG_RUNTIME_DIR=", getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "(unset)", ")");
        return FALSE;
    }

    _platform_linux_wayland_client_internal__g.registry =
        wl_display_get_registry(_platform_linux_wayland_client_internal__g.display);
    wl_registry_add_listener(_platform_linux_wayland_client_internal__g.registry, &_platform_linux_wayland_client_internal__registry_listener, NULL_POINTER);

    // Two round-trips: first to fetch the list of globals, second
    // to process any events that binding them triggered (e.g.
    // wl_output geometry events firing right after we bound the
    // output). Without the second roundtrip our global list is
    // complete but no state has settled.
    if (wl_display_roundtrip(_platform_linux_wayland_client_internal__g.display) < 0 || wl_display_roundtrip(_platform_linux_wayland_client_internal__g.display) < 0)
    {
        log_error("%s", "wl_display_roundtrip failed during registry sweep");
        return FALSE;
    }

    return TRUE;
}

static boole _platform_linux_wayland_client_internal__bind_globals(void)
{
    if (_platform_linux_wayland_client_internal__g.compositor == NULL_POINTER)
    {
        log_error("%s", "compositor did not advertise wl_compositor");
        return FALSE;
    }
    if (_platform_linux_wayland_client_internal__g.wm_base == NULL_POINTER)
    {
        log_error("%s", "compositor did not advertise xdg_wm_base (not an xdg-shell-capable compositor)");
        return FALSE;
    }
    // wl_output is advisory -- we can run without knowing the
    // output geometry, the compositor will size us via toplevel
    // configure.
    return TRUE;
}

// ============================================================================
// zwp_text_input_v3 plumbing.
// The compositor sends `enter` to our text_input when our wl_surface
// has keyboard focus, `leave` when it loses focus. Per spec we must
// only call enable() when entered. We also bump a serial on every
// commit() (required by the protocol's done() handshake -- though
// we don't process server-side commits we still increment).
// Other events (preedit_string, commit_string, delete_surrounding_text,
// done) we currently ignore: meek-ui's input widgets receive their
// characters via wl_keyboard, not via the text_input commit_string
// event. The IME is purely a "show me your keyboard now" signal.
// Future work: wire commit_string into scene__on_char so non-ASCII
// CJK input works.
// ============================================================================

static void _platform_linux_wayland_client_internal__text_input_on_enter( void* data, struct zwp_text_input_v3* ti, struct wl_surface* surface)
{
    (void)data; (void)ti; (void)surface;
    _platform_linux_wayland_client_internal__g.text_input_entered = 1;
    // If a text widget already had focus when enter arrived (race:
    // the focus hook fired before the compositor told us we have
    // text-input focus), enable now. The hook will short-circuit on
    // a subsequent fire.
    if (_platform_linux_wayland_client_internal__g.text_input_active && _platform_linux_wayland_client_internal__g.text_input != NULL_POINTER)
    {
        zwp_text_input_v3_enable(_platform_linux_wayland_client_internal__g.text_input);
        zwp_text_input_v3_commit(_platform_linux_wayland_client_internal__g.text_input);
        log_info("%s", "platform_linux_wayland_client: text_input.enable on late enter");
    }
}

static void _platform_linux_wayland_client_internal__text_input_on_leave( void* data, struct zwp_text_input_v3* ti, struct wl_surface* surface)
{
    (void)data; (void)ti; (void)surface;
    _platform_linux_wayland_client_internal__g.text_input_entered = 0;
}

static void _platform_linux_wayland_client_internal__text_input_on_preedit( void* data, struct zwp_text_input_v3* ti, const char* text, int32_t cursor_begin, int32_t cursor_end)
{
    (void)data; (void)ti; (void)text; (void)cursor_begin; (void)cursor_end;
}

static void _platform_linux_wayland_client_internal__text_input_on_commit_string( void* data, struct zwp_text_input_v3* ti, const char* text)
{
    (void)data; (void)ti; (void)text;
}

static void _platform_linux_wayland_client_internal__text_input_on_delete_surrounding( void* data, struct zwp_text_input_v3* ti, uint32_t before, uint32_t after)
{
    (void)data; (void)ti; (void)before; (void)after;
}

static void _platform_linux_wayland_client_internal__text_input_on_done( void* data, struct zwp_text_input_v3* ti, uint32_t serial)
{
    (void)data; (void)ti; (void)serial;
}

static const struct zwp_text_input_v3_listener
    _platform_linux_wayland_client_internal__text_input_listener = {
        .enter                  = _platform_linux_wayland_client_internal__text_input_on_enter,
        .leave                  = _platform_linux_wayland_client_internal__text_input_on_leave,
        .preedit_string         = _platform_linux_wayland_client_internal__text_input_on_preedit,
        .commit_string          = _platform_linux_wayland_client_internal__text_input_on_commit_string,
        .delete_surrounding_text= _platform_linux_wayland_client_internal__text_input_on_delete_surrounding,
        .done                   = _platform_linux_wayland_client_internal__text_input_on_done,
};

// scene_input fires this on text-widget focus change. We translate
// to the corresponding zwp_text_input_v3 request so the compositor
// can ask its shell to show / hide the on-screen keyboard.
// Spec gate: enable() is only valid AFTER an enter event. If we
// haven't been entered yet (e.g. the seat just gave keyboard focus
// to a different surface, or the compositor hasn't routed it to us
// yet), we record the desired state and let the on_enter handler
// do the enable when the event arrives.
static void _platform_linux_wayland_client_internal__on_text_input_focus(boole active)
{
    _platform_linux_wayland_client_internal__g.text_input_active = active ? 1 : 0;

    if (_platform_linux_wayland_client_internal__g.text_input == NULL_POINTER)
    {
        return;
    }
    if (active)
    {
        if (_platform_linux_wayland_client_internal__g.text_input_entered == 0)
        {
            // Compositor hasn't told us we have text-input focus
            // yet. The on_enter handler will fire enable() when it
            // does. Most compositors send enter immediately on
            // text_input creation if the surface already has
            // keyboard focus, so this is a rare race.
            log_info("%s", "platform_linux_wayland_client: text_input focus active, deferring enable() until enter");
            return;
        }
        zwp_text_input_v3_enable(_platform_linux_wayland_client_internal__g.text_input);
        zwp_text_input_v3_commit(_platform_linux_wayland_client_internal__g.text_input);
        log_info("%s", "platform_linux_wayland_client: text_input.enable + commit (input focused)");
    }
    else
    {
        zwp_text_input_v3_disable(_platform_linux_wayland_client_internal__g.text_input);
        zwp_text_input_v3_commit(_platform_linux_wayland_client_internal__g.text_input);
        log_info("%s", "platform_linux_wayland_client: text_input.disable + commit (input blurred)");
    }
}

static boole _platform_linux_wayland_client_internal__make_toplevel(const gui_app_config_t* cfg)
{
    _platform_linux_wayland_client_internal__g.wl_surface =
        wl_compositor_create_surface(_platform_linux_wayland_client_internal__g.compositor);
    if (_platform_linux_wayland_client_internal__g.wl_surface == NULL_POINTER)
    {
        log_error("%s", "wl_compositor_create_surface failed");
        return FALSE;
    }

    _platform_linux_wayland_client_internal__g.xdg_surface =
        xdg_wm_base_get_xdg_surface(_platform_linux_wayland_client_internal__g.wm_base, _platform_linux_wayland_client_internal__g.wl_surface);
    if (_platform_linux_wayland_client_internal__g.xdg_surface == NULL_POINTER)
    {
        log_error("%s", "xdg_wm_base_get_xdg_surface failed");
        return FALSE;
    }
    xdg_surface_add_listener(_platform_linux_wayland_client_internal__g.xdg_surface, &_platform_linux_wayland_client_internal__xdg_surface_listener, NULL_POINTER);

    _platform_linux_wayland_client_internal__g.xdg_toplevel =
        xdg_surface_get_toplevel(_platform_linux_wayland_client_internal__g.xdg_surface);
    if (_platform_linux_wayland_client_internal__g.xdg_toplevel == NULL_POINTER)
    {
        log_error("%s", "xdg_surface_get_toplevel failed");
        return FALSE;
    }
    xdg_toplevel_add_listener(_platform_linux_wayland_client_internal__g.xdg_toplevel, &_platform_linux_wayland_client_internal__toplevel_listener, NULL_POINTER);

    // Title + app_id. app_id is how compositors group windows
    // (taskbars, alt-tab lists) -- a stable reverse-DNS-looking
    // string is conventional. Cast cfg->title from wchar_t* to a
    // UTF-8 buffer when set; for now we only have a placeholder.
    // TODO: wchar_t -> UTF-8 conversion for xdg_toplevel_set_title.
    // gui_app_config_t.hidden is not honored here: a Wayland surface
    // without a mapped role is never composited, and rendering into an
    // unmapped surface is not defined, so the window is shown
    if (cfg->hidden == TRUE)
    {
        log_warn("%s", "platform_linux_wayland_client: gui_app_config_t.hidden is not supported on Wayland; the window is shown");
    }
    xdg_toplevel_set_title(_platform_linux_wayland_client_internal__g.xdg_toplevel, "meek-ui");
    xdg_toplevel_set_app_id(_platform_linux_wayland_client_internal__g.xdg_toplevel, "com.meek.ui");

    // Request fullscreen unless MEEK_NO_FULLSCREEN=1 is in the env.
    // Passing NULL for the output lets the compositor pick. Shells
    // that act as a phone's primary UI want all the screen they can
    // get; tiling compositors honour this and give us the whole
    // output. The escape hatch is there because nested-dev on a
    // desktop sometimes wants a tile-sized window, not a fullscreen
    // takeover of the dev machine's display.
    char* no_fs = getenv("MEEK_NO_FULLSCREEN");
    if (no_fs == NULL_POINTER || no_fs[0] == 0 || no_fs[0] == '0')
    {
        xdg_toplevel_set_fullscreen(_platform_linux_wayland_client_internal__g.xdg_toplevel, NULL_POINTER);
        log_info("%s", "platform_linux_wayland_client: requested fullscreen (set MEEK_NO_FULLSCREEN=1 to skip)");
    }

    // Idle-inhibit: if the compositor advertised the manager in the
    // registry burst, create a per-surface inhibitor now. The
    // inhibitor is automatically active as long as the surface is
    // visible; closing the surface (or destroying the inhibitor)
    // releases the inhibit. Envvar MEEK_NO_IDLE_INHIBIT=1 opts out
    // -- useful when running on a dev desktop where you DO want
    // your screensaver to fire during testing.
    char* no_inhibit = getenv("MEEK_NO_IDLE_INHIBIT");
    if (_platform_linux_wayland_client_internal__g.idle_inhibit_manager != NULL_POINTER && (no_inhibit == NULL_POINTER || no_inhibit[0] == 0 || no_inhibit[0] == '0'))
    {
        _platform_linux_wayland_client_internal__g.idle_inhibitor =
            zwp_idle_inhibit_manager_v1_create_inhibitor( _platform_linux_wayland_client_internal__g.idle_inhibit_manager, _platform_linux_wayland_client_internal__g.wl_surface);
        if (_platform_linux_wayland_client_internal__g.idle_inhibitor != NULL_POINTER)
        {
            log_info("%s", "platform_linux_wayland_client: idle-inhibit active (screen stays on while surface visible; set MEEK_NO_IDLE_INHIBIT=1 to skip)");
        }
        else
        {
            log_warn("%s", "platform_linux_wayland_client: idle-inhibit manager advertised but create_inhibitor failed");
        }
    }
    else if (_platform_linux_wayland_client_internal__g.idle_inhibit_manager == NULL_POINTER)
    {
        log_info("%s", "platform_linux_wayland_client: compositor does not advertise idle-inhibit; screen may auto-blank");
    }

    // Set up zwp_text_input_v3 if both manager and seat are
    // available. We need the seat to call get_text_input. Then
    // register the focus hook so scene_input fires our enable /
    // disable on input/textarea focus changes.
    if (_platform_linux_wayland_client_internal__g.text_input_manager != NULL_POINTER && _platform_linux_wayland_client_internal__g.seat != NULL_POINTER)
    {
        _platform_linux_wayland_client_internal__g.text_input =
            zwp_text_input_manager_v3_get_text_input( _platform_linux_wayland_client_internal__g.text_input_manager, _platform_linux_wayland_client_internal__g.seat);
        if (_platform_linux_wayland_client_internal__g.text_input != NULL_POINTER)
        {
            zwp_text_input_v3_add_listener( _platform_linux_wayland_client_internal__g.text_input, &_platform_linux_wayland_client_internal__text_input_listener, NULL_POINTER);
            scene__set_text_input_focus_hook( _platform_linux_wayland_client_internal__on_text_input_focus);
            log_info("%s", "platform_linux_wayland_client: zwp_text_input_v3 ready (focus hook installed)");
        }
        else
        {
            log_warn("%s", "platform_linux_wayland_client: zwp_text_input_v3.get_text_input returned NULL");
        }
    }
    else
    {
        log_info("%s", "platform_linux_wayland_client: text_input_manager or seat unavailable; on-input focus the system keyboard will not auto-show");
    }

    // Initial default size before the compositor configures.
    // Needed so wl_egl_window_create has concrete dimensions.
    _platform_linux_wayland_client_internal__g.viewport_w = (int)cfg->width;
    _platform_linux_wayland_client_internal__g.viewport_h = (int)cfg->height;

    // Empty initial commit: "please configure me." The xdg-shell
    // protocol requires this before we attach a real buffer.
    wl_surface_commit(_platform_linux_wayland_client_internal__g.wl_surface);

    // Wait for the compositor's configure + our ack to complete.
    // After this roundtrip, `configured` is TRUE and we can begin
    // rendering.
    while (_platform_linux_wayland_client_internal__g.configured == FALSE)
    {
        if (wl_display_dispatch(_platform_linux_wayland_client_internal__g.display) < 0)
        {
            log_error("%s", "wl_display_dispatch failed waiting for configure");
            return FALSE;
        }
    }

    return TRUE;
}

// ============================================================================
// EGL
// ============================================================================

static boole _platform_linux_wayland_client_internal__init_egl(void)
{
    _platform_linux_wayland_client_internal__g.egl_display =
        eglGetDisplay((EGLNativeDisplayType)_platform_linux_wayland_client_internal__g.display);
    if (_platform_linux_wayland_client_internal__g.egl_display == EGL_NO_DISPLAY)
    {
        log_error("%s", "eglGetDisplay returned EGL_NO_DISPLAY");
        return FALSE;
    }

    EGLint egl_major = 0, egl_minor = 0;
    if (eglInitialize(_platform_linux_wayland_client_internal__g.egl_display, &egl_major, &egl_minor) == FALSE)
    {
        log_error("%s %x %s", "eglInitialize failed (0x", eglGetError(), ")");
        return FALSE;
    }
    log_info("%s %d %s %d", "platform_linux_wayland_client: EGL", egl_major, ".", egl_minor);

    if (eglBindAPI(EGL_OPENGL_ES_API) == FALSE)
    {
        log_error("%s", "eglBindAPI(GLES) failed");
        return FALSE;
    }

    // Ask for a standard RGBA8 window-surface config with depth
    // (the renderer may not use depth, but requesting it doesn't
    // cost anything and saves us if gles3_renderer evolves to
    // need it).
    EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,   8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_NONE,
    };
    EGLint n = 0;
    if (eglChooseConfig(_platform_linux_wayland_client_internal__g.egl_display, cfg_attrs, &_platform_linux_wayland_client_internal__g.egl_config, 1, &n) == FALSE || n == 0)
    {
        log_error("%s", "eglChooseConfig found no matching configs");
        return FALSE;
    }

    EGLint ctx_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE,
    };
    _platform_linux_wayland_client_internal__g.egl_context = eglCreateContext( _platform_linux_wayland_client_internal__g.egl_display, _platform_linux_wayland_client_internal__g.egl_config, EGL_NO_CONTEXT, ctx_attrs);
    if (_platform_linux_wayland_client_internal__g.egl_context == EGL_NO_CONTEXT)
    {
        log_error("%s %x %s", "eglCreateContext failed (0x", eglGetError(), ")");
        return FALSE;
    }

    // wl_egl_window adapts our wl_surface into something EGL's
    // platform layer can make a window surface against. Mesa's
    // wayland-egl backend handles buffer allocation + dmabuf
    // attach on eglSwapBuffers.
    _platform_linux_wayland_client_internal__g.egl_window = wl_egl_window_create( _platform_linux_wayland_client_internal__g.wl_surface, _platform_linux_wayland_client_internal__g.viewport_w, _platform_linux_wayland_client_internal__g.viewport_h);
    if (_platform_linux_wayland_client_internal__g.egl_window == NULL_POINTER)
    {
        log_error("%s", "wl_egl_window_create failed");
        return FALSE;
    }

    _platform_linux_wayland_client_internal__g.egl_surface = eglCreateWindowSurface( _platform_linux_wayland_client_internal__g.egl_display, _platform_linux_wayland_client_internal__g.egl_config, (EGLNativeWindowType)_platform_linux_wayland_client_internal__g.egl_window, NULL_POINTER);
    if (_platform_linux_wayland_client_internal__g.egl_surface == EGL_NO_SURFACE)
    {
        log_error("%s %x %s", "eglCreateWindowSurface failed (0x", eglGetError(), ")");
        return FALSE;
    }

    if (eglMakeCurrent(_platform_linux_wayland_client_internal__g.egl_display, _platform_linux_wayland_client_internal__g.egl_surface, _platform_linux_wayland_client_internal__g.egl_surface, _platform_linux_wayland_client_internal__g.egl_context) == FALSE)
    {
        log_error("%s %x %s", "eglMakeCurrent failed (0x", eglGetError(), ")");
        return FALSE;
    }

    return TRUE;
}

static void _platform_linux_wayland_client_internal__term_egl(void)
{
    if (_platform_linux_wayland_client_internal__g.egl_display != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(_platform_linux_wayland_client_internal__g.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (_platform_linux_wayland_client_internal__g.egl_surface != EGL_NO_SURFACE)
    {
        eglDestroySurface(_platform_linux_wayland_client_internal__g.egl_display, _platform_linux_wayland_client_internal__g.egl_surface);
        _platform_linux_wayland_client_internal__g.egl_surface = EGL_NO_SURFACE;
    }
    if (_platform_linux_wayland_client_internal__g.egl_context != EGL_NO_CONTEXT)
    {
        eglDestroyContext(_platform_linux_wayland_client_internal__g.egl_display, _platform_linux_wayland_client_internal__g.egl_context);
        _platform_linux_wayland_client_internal__g.egl_context = EGL_NO_CONTEXT;
    }
    if (_platform_linux_wayland_client_internal__g.egl_display != EGL_NO_DISPLAY)
    {
        eglTerminate(_platform_linux_wayland_client_internal__g.egl_display);
        _platform_linux_wayland_client_internal__g.egl_display = EGL_NO_DISPLAY;
    }
    if (_platform_linux_wayland_client_internal__g.egl_window != NULL_POINTER)
    {
        wl_egl_window_destroy(_platform_linux_wayland_client_internal__g.egl_window);
        _platform_linux_wayland_client_internal__g.egl_window = NULL_POINTER;
    }
}

// ============================================================================
// connection teardown
// ============================================================================

static void _platform_linux_wayland_client_internal__close_display(void)
{
    // Release idle-inhibit first so the compositor is allowed to
    // blank the screen again after we go away. Destroying the
    // inhibitor object is the "release" operation; the manager
    // itself is just a factory and gets torn down at the end.
    if (_platform_linux_wayland_client_internal__g.idle_inhibitor != NULL_POINTER)
    {
        zwp_idle_inhibitor_v1_destroy(_platform_linux_wayland_client_internal__g.idle_inhibitor);
        _platform_linux_wayland_client_internal__g.idle_inhibitor = NULL_POINTER;
    }
    if (_platform_linux_wayland_client_internal__g.idle_inhibit_manager != NULL_POINTER)
    {
        zwp_idle_inhibit_manager_v1_destroy(_platform_linux_wayland_client_internal__g.idle_inhibit_manager);
        _platform_linux_wayland_client_internal__g.idle_inhibit_manager = NULL_POINTER;
    }

    // Seat input objects. wl_pointer_release + wl_touch_release
    // are the "graceful" destructors (added in seat v3+); they
    // tell the compositor we won't receive any more events before
    // the seat object itself dies.
    if (_platform_linux_wayland_client_internal__g.pointer != NULL_POINTER)
    {
        wl_pointer_release(_platform_linux_wayland_client_internal__g.pointer);
        _platform_linux_wayland_client_internal__g.pointer = NULL_POINTER;
    }
    if (_platform_linux_wayland_client_internal__g.touch != NULL_POINTER)
    {
        wl_touch_release(_platform_linux_wayland_client_internal__g.touch);
        _platform_linux_wayland_client_internal__g.touch = NULL_POINTER;
    }
    if (_platform_linux_wayland_client_internal__g.seat != NULL_POINTER)
    {
        wl_seat_release(_platform_linux_wayland_client_internal__g.seat);
        _platform_linux_wayland_client_internal__g.seat = NULL_POINTER;
    }

    if (_platform_linux_wayland_client_internal__g.xdg_toplevel != NULL_POINTER)
    {
        xdg_toplevel_destroy(_platform_linux_wayland_client_internal__g.xdg_toplevel);
        _platform_linux_wayland_client_internal__g.xdg_toplevel = NULL_POINTER;
    }
    if (_platform_linux_wayland_client_internal__g.xdg_surface != NULL_POINTER)
    {
        xdg_surface_destroy(_platform_linux_wayland_client_internal__g.xdg_surface);
        _platform_linux_wayland_client_internal__g.xdg_surface = NULL_POINTER;
    }
    if (_platform_linux_wayland_client_internal__g.wl_surface != NULL_POINTER)
    {
        wl_surface_destroy(_platform_linux_wayland_client_internal__g.wl_surface);
        _platform_linux_wayland_client_internal__g.wl_surface = NULL_POINTER;
    }
    if (_platform_linux_wayland_client_internal__g.output != NULL_POINTER)
    {
        wl_output_destroy(_platform_linux_wayland_client_internal__g.output);
        _platform_linux_wayland_client_internal__g.output = NULL_POINTER;
    }
    if (_platform_linux_wayland_client_internal__g.wm_base != NULL_POINTER)
    {
        xdg_wm_base_destroy(_platform_linux_wayland_client_internal__g.wm_base);
        _platform_linux_wayland_client_internal__g.wm_base = NULL_POINTER;
    }
    if (_platform_linux_wayland_client_internal__g.compositor != NULL_POINTER)
    {
        wl_compositor_destroy(_platform_linux_wayland_client_internal__g.compositor);
        _platform_linux_wayland_client_internal__g.compositor = NULL_POINTER;
    }
    if (_platform_linux_wayland_client_internal__g.registry != NULL_POINTER)
    {
        wl_registry_destroy(_platform_linux_wayland_client_internal__g.registry);
        _platform_linux_wayland_client_internal__g.registry = NULL_POINTER;
    }
    if (_platform_linux_wayland_client_internal__g.display != NULL_POINTER)
    {
        wl_display_disconnect(_platform_linux_wayland_client_internal__g.display);
        _platform_linux_wayland_client_internal__g.display = NULL_POINTER;
    }
}

// ============================================================================
// host symbol resolver (UI_HANDLER bindings)
// ============================================================================

static gui_handler_fn _platform_linux_wayland_client_internal__resolve_host_symbol(char* name)
{
    // Same dlsym(RTLD_DEFAULT, name) pattern the X11 + DRM
    // backends use. Handlers are UI_HANDLER-marked in the host's
    // main.c, which keeps them exported even under
    // -fvisibility=hidden.
    void* sym = NULL_POINTER;
#if defined(__linux__)
    sym = dlsym(RTLD_DEFAULT, name);
#endif
    return (gui_handler_fn)sym;
}

// ============================================================================
// public API
// ============================================================================

/**
 * @brief initialize the platform and renderer. Called once at startup.
 *
 * @param const gui_app_config_t* cfg -> Window title / size / clear color.
 *
 * @return boole -> TRUE on success; FALSE if window creation or renderer init failed.
 */
boole platform__init(const gui_app_config_t* cfg)
{
    memory_manager__init();

    stdlib__memset(&_platform_linux_wayland_client_internal__g, 0, sizeof(_platform_linux_wayland_client_internal__g));
    _platform_linux_wayland_client_internal__g.egl_display = EGL_NO_DISPLAY;
    _platform_linux_wayland_client_internal__g.egl_context = EGL_NO_CONTEXT;
    _platform_linux_wayland_client_internal__g.egl_surface = EGL_NO_SURFACE;

    // Render-gating defaults: force_render=1 so the very first
    // platform__tick after init produces a frame regardless of
    // anything else. The gating_disabled flag mirrors the
    // MEEK_RENDER_GATING env var as a hard kill switch -- set
    // "off" / "0" / "no" to fall back to pre-Phase-2 always-render
    // behaviour without rebuilding.
    _platform_linux_wayland_client_internal__g.force_render = 1;
    {
        const char* gating = getenv("MEEK_RENDER_GATING");
        if (gating != NULL_POINTER && ( stdlib__strcmp(gating, "off") == 0 || stdlib__strcmp(gating, "0")   == 0 || stdlib__strcmp(gating, "no")  == 0))
        {
            _platform_linux_wayland_client_internal__g.gating_disabled = 1;
            log_info("%s %s", "platform_linux_wayland_client: render gating DISABLED via MEEK_RENDER_GATING=", gating);
        }
    }

    if (cfg == NULL_POINTER)
    {
        log_error("%s", "platform__init: cfg is NULL");
        memory_manager__shutdown();
        return FALSE;
    }
    _platform_linux_wayland_client_internal__g.clear_color = cfg->clear_color;

    if (_platform_linux_wayland_client_internal__open_display() == FALSE)
    {
        goto fail;
    }
    if (_platform_linux_wayland_client_internal__bind_globals() == FALSE)
    {
        goto fail;
    }
    if (_platform_linux_wayland_client_internal__make_toplevel(cfg) == FALSE)
    {
        goto fail;
    }
    if (_platform_linux_wayland_client_internal__init_egl() == FALSE)
    {
        goto fail;
    }

    if (renderer__init(NULL_POINTER) == FALSE)
    {
        log_error("%s", "platform__init: renderer__init failed");
        goto fail;
    }

    widget_registry__bootstrap_builtins();
    if (font__init() == FALSE)
    {
        log_error("%s", "platform__init: font__init failed");
        renderer__shutdown();
        goto fail;
    }

    scene__set_symbol_resolver(_platform_linux_wayland_client_internal__resolve_host_symbol);

    log_info("%s %d %s %d %s", "platform_linux_wayland_client: up (", _platform_linux_wayland_client_internal__g.viewport_w, "x", _platform_linux_wayland_client_internal__g.viewport_h, ")");
    return TRUE;

fail:
    _platform_linux_wayland_client_internal__term_egl();
    _platform_linux_wayland_client_internal__close_display();
    memory_manager__shutdown();
    return FALSE;
}

// Internal force-render setter. Called from this file's wl_seat
// listeners (after every scene__on_*) and from the configure
// resize path. The PUBLIC equivalent for cross-module callers
// (meek-shell's meek_shell_v1_client.c, hot reload, etc.) is
// platform_wayland__request_render below.
static void _platform_linux_wayland_client_internal__force_render_now(void)
{
    _platform_linux_wayland_client_internal__g.force_render = 1;
}

// Public wake hook. See header docstring. Used by code outside
// this file to mark scene state as dirty when the change isn't
// observable through the standard scene__on_* path.
/**
 * @brief mark the platform's render loop as obliged to run a render block on its next tick, regardless of the gate's normal idle decision. Called by code OUTSIDE the platform file when scene-relevant state has changed but isn't observable through scene__on_* (which already wakes the loop). Examples: - meek-shell's meek_shell_v1_client.c receiving a foreign-app buffer event, toplevel add/remove, IME request, etc. - hot reload reloading a .ui or .style file mid-loop. Without this hook, those state changes wouldn't be reflected on screen until some unrelated input event woke the gate. Idempotent + side-effect-free except for the internal flag write. Safe no-op before platform__init or after platform__shutdown.
 *
 * @return void
 */
void platform_wayland__request_render(void)
{
    _platform_linux_wayland_client_internal__g.force_render = 1;
}

/**
 * @brief read the current wl_output mode's pixel dimensions. Both arguments are populated with the latched values from the most recent `wl_output.mode` event carrying the WL_OUTPUT_MODE_CURRENT flag -- i.e. the panel's native pixel size. Returns FALSE (and writes 0 to both out params) when the compositor hasn't sent a current-mode event yet, when wl_output isn't bound, or before platform__init / after platform__shutdown. Callers that need a value before that point (e.g. shell-side gesture recognizers initialised at startup) should use FALSE as the cue to fall back to the cfg width/height. Output is in panel-native pixels, NOT logical / scale-adjusted pixels. That's the right value for code that translates raw libinput-derived touch coords (which the compositor forwards unscaled) -- and the wrong value for code laying out widgets in the meek-ui logical coordinate space.
 *
 * @param int* out_w -> Receives the current mode's pixel width.
 * @param int* out_h -> Receives the current mode's pixel height.
 *
 * @return boole -> TRUE if a real value was written, FALSE on fallback.
 */
boole platform_wayland__get_output_pixel_size(int* out_w, int* out_h)
{
    int w = _platform_linux_wayland_client_internal__g.output_pixel_w;
    int h = _platform_linux_wayland_client_internal__g.output_pixel_h;
    if (out_w != NULL_POINTER)
    {
        *out_w = w;
    }
    if (out_h != NULL_POINTER)
    {
        *out_h = h;
    }
    return (w > 0 && h > 0) ? TRUE : FALSE;
}

// Physical panel size in millimeters (from wl_output.geometry).
// Returns FALSE + writes 0s when the compositor hasn't sent a
// geometry event yet, or when the values are obviously bogus
// (some compositors send 0,0 for "unknown"). Used by meek-shell
// to compute a DPI-based default for the Wayland-scale slider.
/**
 * @brief physical panel size in millimeters from wl_output.geometry. Returns FALSE (and writes 0) when the compositor hasn't sent a geometry event yet OR when the values are 0 (some compositors report 0,0 for "unknown / virtual output"). meek-shell uses this with the pixel size to compute a DPI-based default for the Wayland-scale slider on first run.
 *
 * @param int* out_w -> Receives panel width in mm.
 * @param int* out_h -> Receives panel height in mm.
 *
 * @return boole -> TRUE if a real value was written, FALSE on fallback.
 */
boole platform_wayland__get_output_mm_size(int* out_w, int* out_h)
{
    int w = _platform_linux_wayland_client_internal__g.output_mm_w;
    int h = _platform_linux_wayland_client_internal__g.output_mm_h;
    if (out_w != NULL_POINTER)
    {
        *out_w = w;
    }
    if (out_h != NULL_POINTER)
    {
        *out_h = h;
    }
    return (w > 0 && h > 0) ? TRUE : FALSE;
}

// Refresh rate from the current wl_output mode, in mHz (so 60000 =
// 60.000 Hz, 119880 = 119.880 Hz, etc.). Returns FALSE + writes 0
// when the compositor hasn't sent a current-mode event yet.
/**
 * @brief read the current wl_output mode's refresh rate, in millihertz. 60000 = 60.000 Hz, 119880 = 119.880 Hz, etc. Returns FALSE (and writes 0) when the compositor hasn't sent a current-mode event yet.
 *
 * @param int* out_mhz -> Receives the rate in mHz.
 *
 * @return boole -> TRUE if a real value was written, FALSE on fallback.
 */
boole platform_wayland__get_output_refresh_mhz(int* out_mhz)
{
    int mhz = _platform_linux_wayland_client_internal__g.output_refresh_mhz;
    if (out_mhz != NULL_POINTER)
    {
        *out_mhz = mhz;
    }
    return (mhz > 0) ? TRUE : FALSE;
}

/**
 * @brief monotonic count of real rendered frames (post-eglSwapBuffers). Diff the value over a wall-clock window to compute true FPS: fps = (now_count - prev_count) * 1000 / window_ms. Unlike a per-platform__tick counter, this does NOT inflate during event-only ticks (touch / configure / frame-callback wakes that don't trigger a render).
 *
 * @return uint64 -> cumulative frame count since process start.
 */
uint64 platform_wayland__frames_rendered(void)
{
    return _platform_linux_wayland_client_internal__g.frames_rendered;
}

// === Render gating (Phase 2) ===
// platform__tick used to run resolve_styles + animator + layout +
// renderer + eglSwapBuffers on EVERY tick at whatever rate the
// host's loop pumped it (in meek-shell that's a tight non-blocking
// while). The gate skips that whole block when nothing has changed
// since the last render. The poll() timeout below is the second
// half of the same fix: when the gate would skip, we also block in
// poll() so the host's loop doesn't busy-spin between skipped ticks.
// Wake sources (set force_render = 1):
// * scene__on_* input dispatch from this file's wl_seat listeners
// * xdg_toplevel.configure size change
// * platform_wayland__request_render() from outside
// * animator__has_active() returning 1 (override; checked inline)
// Idle behaviour: when ACTIVITY_TIMEOUT_MS has passed with no
// actual rendering AND the gate would otherwise skip the render
// block, the tick blocks in poll() for up to IDLE_TIMEOUT_MS so
// the loop doesn't busy-spin. CPU in that state is near zero --
// the kernel's poll() is what we're sleeping in, not a userspace
// loop.
// While ACTIVE (within ACTIVITY_TIMEOUT_MS of the last render),
// poll uses a 0-timeout: animations + rapid input bursts get
// full vblank-rate rendering without any extra wait. The
// distinction matters: a 250 ms poll-sleep mid-animation makes
// the animation look like 4 fps. We only allow the sleep when
// the system has been visually quiet for ACTIVITY_TIMEOUT_MS,
// which means any frame we're about to draw is a wake-from-idle
// frame, not part of a continuous animation.
// 5 s ACTIVITY window matches what the user picked: long enough
// that a brief pause mid-animation doesn't drop us into idle, short
// enough that battery-savings kick in within seconds of the user
// stopping interacting.
#define _PLATFORM_LINUX_WAYLAND_CLIENT_INTERNAL__ACTIVITY_TIMEOUT_MS 10000
#define _PLATFORM_LINUX_WAYLAND_CLIENT_INTERNAL__IDLE_TIMEOUT_MS       250

/**
 * @brief run one iteration of the platform's main loop: pump OS events, capture the frame timestamp, resolve styles, tick the animator, lay out the tree, emit draws, and present. Returns FALSE when the user has asked to close the application (window close, Android APP_CMD_DESTROY, etc.).
 *
 * @return boole -> TRUE to continue running; FALSE to exit.
 */
boole platform__tick(void)
{
    if (_platform_linux_wayland_client_internal__g.should_close)
    {
        return FALSE;
    }
    struct timespec _ts_tick_entry;
    clock_gettime(CLOCK_MONOTONIC, &_ts_tick_entry);
    struct timespec _ts_after_prepare_read, _ts_after_poll, _ts_after_dispatch;

    // Pump any queued Wayland events. dispatch_pending() processes
    // events ALREADY in the read queue; we follow it with a read
    // so new events land there for next tick. Combined with the
    // flush-after-render below this gives a tick shape that
    // neither starves nor busy-waits.
    // -1 return means the connection went away (compositor died or
    // our protocol misuse closed us). Fail fast; the next tick
    // would just keep erroring.
    // CRITICAL: wl_display_dispatch_pending ONLY processes events
    // already read into the client queue. It does NOT read new
    // events from the socket. Without an explicit read pass, the
    // compositor's events (including wl_buffer.release for every
    // frame our eglSwapBuffers committed) pile up in the kernel
    // socket buffer. Mesa's wl_egl_window allocates new wl_buffers
    // on every swap when its pool is "empty" from Mesa's POV --
    // because it never sees the release events that would return
    // buffers to the pool. After ~4000 frames Mesa hits its
    // internal VkImage budget with
    // `CreateSwapchainKHR: VK_ERROR_OUT_OF_HOST_MEMORY`.
    // Fix: non-blocking read of any pending socket data before
    // dispatch. The prepare/read/dispatch trio is libwayland's
    // canonical drain pattern; we poll with a 0-timeout so it's
    // still non-blocking (a render-driven loop shouldn't block
    // waiting for events).
    struct wl_display* dpy = _platform_linux_wayland_client_internal__g.display;
    while (wl_display_prepare_read(dpy) != 0)
    {
        if (wl_display_dispatch_pending(dpy) < 0)
        {
            log_error("%s %d %s", "wl_display_dispatch_pending: connection lost (errno=", errno, "); terminating");
            _platform_linux_wayland_client_internal__g.should_close = TRUE;
            return FALSE;
        }
    }
    wl_display_flush(dpy);
    clock_gettime(CLOCK_MONOTONIC, &_ts_after_prepare_read);

    // Phase-2 poll-timeout selection.
    // Three cases:
    // 1. gate disabled OR force_render set OR animator running:
    // We KNOW we'll render this tick -> timeout = 0 (just
    // drain any events that arrived since last poll, no wait).
    // 2. Gate would skip BUT we rendered recently (within
    // ACTIVITY_TIMEOUT_MS): also timeout = 0. Animations + rapid
    // input bursts produce frames at vblank rate; an extra
    // 250 ms wait between them looks like 4 fps. We only allow
    // the sleep when the system has been visually quiet for
    // ACTIVITY_TIMEOUT_MS.
    // 3. Gate would skip AND nothing's rendered for >= ACTIVITY_
    // TIMEOUT_MS: timeout = IDLE_TIMEOUT_MS (true idle, sleep
    // and let the kernel wake us on the next event).
    // Any Wayland event arriving during the block wakes poll
    // immediately. The handlers we register (wl_seat listeners here,
    // meek_shell_v1 listeners in the shell calling
    // platform_wayland__request_render) set force_render which the
    // gate re-evaluates below.
    struct timespec ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    int64 now_ms = (int64)ts_now.tv_sec * 1000 + (int64)(ts_now.tv_nsec / 1000000);

    // Pre-poll timeout. Block only if nothing has happened for at
    // least ACTIVITY_TIMEOUT_MS AND nothing's forcing us to render
    // right now. ANY of the following counts as "something happening":
    // * force_render set (input, configure, foreign-buffer, etc.)
    // * animator__has_active (appear/disappear in flight)
    // * last_activity_ms was bumped within the window (recent
    // prior activity that hasn't aged out yet)
    int64 since_activity =
        now_ms - _platform_linux_wayland_client_internal__g.last_activity_ms;
    if (_platform_linux_wayland_client_internal__g.force_render || animator__has_active())
    {
        _platform_linux_wayland_client_internal__g.last_activity_ms = now_ms;
        since_activity = 0;
    }

    int poll_timeout_ms = 0;
    if (_platform_linux_wayland_client_internal__g.gating_disabled == 0 && since_activity >= _PLATFORM_LINUX_WAYLAND_CLIENT_INTERNAL__ACTIVITY_TIMEOUT_MS)
    {
        poll_timeout_ms = _PLATFORM_LINUX_WAYLAND_CLIENT_INTERNAL__IDLE_TIMEOUT_MS;
    }

    struct pollfd pfd = { .fd = wl_display_get_fd(dpy), .events = POLLIN, .revents = 0 };
    struct timespec _ts_before_poll;
    clock_gettime(CLOCK_MONOTONIC, &_ts_before_poll);
    int pr = poll(&pfd, 1, poll_timeout_ms);
    clock_gettime(CLOCK_MONOTONIC, &_ts_after_poll);
    if (pr > 0 && (pfd.revents & POLLIN))
    {
        if (wl_display_read_events(dpy) < 0)
        {
            log_error("%s %d %s", "wl_display_read_events: connection lost (errno=", errno, "); terminating");
            _platform_linux_wayland_client_internal__g.should_close = TRUE;
            return FALSE;
        }
    }
    else
    {
        wl_display_cancel_read(dpy);
    }

    if (wl_display_dispatch_pending(dpy) < 0)
    {
        log_error("%s %d %s", "wl_display_dispatch_pending: connection lost (errno=", errno, "); terminating");
        _platform_linux_wayland_client_internal__g.should_close = TRUE;
        return FALSE;
    }
    clock_gettime(CLOCK_MONOTONIC, &_ts_after_dispatch);
    if (_platform_linux_wayland_client_internal__g.should_close)
    {
        return FALSE;
    }

    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64 now_ms = (int64)ts.tv_sec * 1000 + (int64)(ts.tv_nsec / 1000000);
        scene__begin_frame_time(now_ms);
    }

    // Re-evaluate activity AFTER dispatch. The top-of-tick check
    // ran before poll(), so it could not see force_render / animator
    // signals raised by handlers that fired during
    // wl_display_dispatch_pending() above (input events,
    // configure, IPC wake-ups, etc.). If we don't pick those up
    // here, last_activity_ms remains anchored at the moment of the
    // last "naturally observed" signal, since_activity stays >=
    // ACTIVITY_TIMEOUT_MS forever, and every subsequent tick is
    // stuck at the 250 ms idle cadence even though the user is
    // actively interacting -- exactly the "only fast for first 10
    // seconds, then slow forever" symptom.
    // After this bump, the gate sees a fresh signal from THIS
    // tick's dispatch and the activity window is restored.
    {
        struct timespec ts2;
        clock_gettime(CLOCK_MONOTONIC, &ts2);
        int64 now_ms2 = (int64)ts2.tv_sec * 1000 + (int64)(ts2.tv_nsec / 1000000);
        if (_platform_linux_wayland_client_internal__g.force_render || animator__has_active())
        {
            _platform_linux_wayland_client_internal__g.last_activity_ms = now_ms2;
            since_activity = 0;
        }
        else
        {
            since_activity =
                now_ms2 - _platform_linux_wayland_client_internal__g.last_activity_ms;
        }
    }

    // Render gate (Phase 2 v3). The user's spec: "if pixels changed
    // on screen within the last 10 seconds, no timeout, full speed.
    // Otherwise, sleep." -- which means we must NOT skip the render
    // block during the active window. animator__has_active is
    // incomplete (it only tracks appear/disappear; transitions,
    // widget auto-repeat timers, and other animators aren't caught),
    // so during the active window we always render and let
    // eglSwapBuffers' triple-buffer back-pressure pace us.
    // Skip the render block ONLY when the system has been visually
    // quiet for ACTIVITY_TIMEOUT_MS AND nothing's forcing a render.
    // That's the true-idle case where blocking in poll above
    // already happened; if we got woken and still have nothing to
    // do (no force, no animator), don't bother rendering.
    int can_skip_render =
        _platform_linux_wayland_client_internal__g.gating_disabled == 0 &&
        _platform_linux_wayland_client_internal__g.force_render    == 0 &&
        animator__has_active() == 0 &&
        since_activity >= _PLATFORM_LINUX_WAYLAND_CLIENT_INTERNAL__ACTIVITY_TIMEOUT_MS;

    if (can_skip_render)
    {
        // Idle path. Still flush queued requests so the compositor
        // sees anything we sent during dispatch (e.g. xdg_surface.
        // ack_configure) without having to wait for the next render.
        wl_display_flush(_platform_linux_wayland_client_internal__g.display);
        return TRUE;
    }

    // Render path. Clear the force flag so a single forced render
    // doesn't keep us awake past its useful frame.
    // last_activity_ms is NOT bumped here. It's bumped only by real
    // activity signals (force_render set, animator active) at the
    // top of the tick. Bumping on every render would make the
    // window self-perpetuating: each render resets the clock, so
    // we'd never reach the idle threshold even when nothing is
    // happening except our own redraws of the same scene.
    _platform_linux_wayland_client_internal__g.force_render = 0;

    int64 vw = (int64)_platform_linux_wayland_client_internal__g.viewport_w;
    int64 vh = (int64)_platform_linux_wayland_client_internal__g.viewport_h;

    // Per-phase timing: temporary instrumentation to localise the
    // page-swipe FPS dip. Logged every ~30 frames; remove once
    // bottleneck is identified.
    struct timespec _ts_a, _ts_b, _ts_c, _ts_d, _ts_e;
    clock_gettime(CLOCK_MONOTONIC, &_ts_a);
    scene__resolve_styles();
    clock_gettime(CLOCK_MONOTONIC, &_ts_b);
    animator__tick();
    scene__layout(vw, vh);
    clock_gettime(CLOCK_MONOTONIC, &_ts_c);

    renderer__begin_frame(vw, vh, _platform_linux_wayland_client_internal__g.clear_color);
    scene__emit_draws();
    renderer__end_frame();
    clock_gettime(CLOCK_MONOTONIC, &_ts_d);

    // One-shot screenshot dump. When /tmp/meek-shot-trigger exists,
    // glReadPixels the freshly-rendered backbuffer into a raw RGBA
    // file at /tmp/meek-shot.rgba and delete the trigger. External
    // caller (an ssh command) creates the trigger; after the next
    // frame the file is on disk and can be converted via:
    // convert -size 1080x2246 -depth 8 rgba:/tmp/meek-shot.rgba out.png
    // (the viewport size is logged alongside).
    {
        struct stat _st_trig;
        if (stat("/tmp/meek-shot-trigger", &_st_trig) == 0)
        {
            int sw = (int)vw, sh = (int)vh;
            size_t bytes = (size_t)sw * (size_t)sh * 4;
            unsigned char* pix = (unsigned char*)GUI_MALLOC_T(bytes, MM_TYPE__GENERIC);
            if (pix != NULL_POINTER)
            {
                glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, pix);
                // GL framebuffer is bottom-left origin; flip vertically
                // so the saved file matches what's on screen.
                size_t row_bytes = (size_t)sw * 4;
                unsigned char* tmp = (unsigned char*)GUI_MALLOC_T(row_bytes, MM_TYPE__GENERIC);
                if (tmp != NULL_POINTER)
                {
                    for (int y = 0; y < sh / 2; y++)
                    {
                        unsigned char* top = pix + (size_t)y * row_bytes;
                        unsigned char* bot = pix + (size_t)(sh - 1 - y) * row_bytes;
                        stdlib__memcpy(tmp, top, row_bytes);
                        stdlib__memcpy(top, bot, row_bytes);
                        stdlib__memcpy(bot, tmp, row_bytes);
                    }
                    GUI_FREE(tmp);
                }
                if (fs__write_entire_file((char*)"/tmp/meek-shot.rgba", pix, (int64)bytes) == TRUE)
                {
                    log_info("%s %d %s %d %s", "[screenshot] dumped", sw, "x", sh, "to /tmp/meek-shot.rgba");
                }
                GUI_FREE(pix);
            }
            unlink("/tmp/meek-shot-trigger");
        }
    }

    // eglSwapBuffers on a wl_egl_window surface internally does
    // wl_surface.attach(new_buffer) + damage + commit. We never
    // touch wl_surface directly for buffer handoff -- that would
    // double-commit.
    eglSwapBuffers(_platform_linux_wayland_client_internal__g.egl_display, _platform_linux_wayland_client_internal__g.egl_surface);
    clock_gettime(CLOCK_MONOTONIC, &_ts_e);
    {
        long us_prepare = (_ts_after_prepare_read.tv_sec - _ts_tick_entry.tv_sec) * 1000000L + (_ts_after_prepare_read.tv_nsec - _ts_tick_entry.tv_nsec) / 1000L;
        long us_poll    = (_ts_after_poll.tv_sec - _ts_before_poll.tv_sec) * 1000000L + (_ts_after_poll.tv_nsec - _ts_before_poll.tv_nsec) / 1000L;
        long us_disp    = (_ts_after_dispatch.tv_sec - _ts_after_poll.tv_sec) * 1000000L + (_ts_after_dispatch.tv_nsec - _ts_after_poll.tv_nsec) / 1000L;
        long us_resolve = (_ts_b.tv_sec - _ts_a.tv_sec) * 1000000L + (_ts_b.tv_nsec - _ts_a.tv_nsec) / 1000L;
        long us_layout  = (_ts_c.tv_sec - _ts_b.tv_sec) * 1000000L + (_ts_c.tv_nsec - _ts_b.tv_nsec) / 1000L;
        long us_draws   = (_ts_d.tv_sec - _ts_c.tv_sec) * 1000000L + (_ts_d.tv_nsec - _ts_c.tv_nsec) / 1000L;
        long us_swap    = (_ts_e.tv_sec - _ts_d.tv_sec) * 1000000L + (_ts_e.tv_nsec - _ts_d.tv_nsec) / 1000L;
        long us_tick_total = (_ts_e.tv_sec - _ts_tick_entry.tv_sec) * 1000000L + (_ts_e.tv_nsec - _ts_tick_entry.tv_nsec) / 1000L;
        static int _ftc = 0;
        _ftc++;
        boole slow_frame = (us_tick_total > 25000L);
        if (_ftc % 30 == 0 || slow_frame)
        {
            log_info("%s %s %s %ld %s %ld %s %ld %s %ld %s %ld %s %ld %s %ld %s %ld", "[platform] frame us", slow_frame ? " [SLOW]" : "", ": prep=", us_prepare, "poll=", us_poll, "disp=", us_disp, "resolve=", us_resolve, "layout=", us_layout, "draws=", us_draws, "swap=", us_swap, "tick_total=", us_tick_total);
        }
    }
    // Bump the rendered-frame counter AFTER swap. This is the only
    // place it changes, so a consumer diffing the value over a
    // window gets actual rendered-FPS.
    _platform_linux_wayland_client_internal__g.frames_rendered++;

    // Periodic glFinish (every N ticks) forces the GL driver to
    // flush + wait on its command queue. On Mesa's Zink backend
    // (GLES-over-Vulkan, which is what Adreno uses here), per-frame
    // eglSwapBuffers accumulates Vulkan semaphores / swapchain
    // state that isn't reclaimed until a synchronous wait point.
    // Without this, long-running clients hit
    // `CreateSwapchainKHR: VK_ERROR_OUT_OF_HOST_MEMORY` around the
    // 80--120 s mark. glFinish stalls the pipeline briefly every
    // ~1 s at 60 Hz, which is invisible to the user but keeps the
    // driver's memory pressure bounded.
    static int _swap_counter = 0;
    if ((++_swap_counter % 60) == 0)
    {
        glFinish();
    }

    // Flush queued client->server traffic so the compositor sees
    // this frame's commit + any queued requests before our next
    // dispatch_pending.
    wl_display_flush(_platform_linux_wayland_client_internal__g.display);
    return TRUE;
}

/**
 * @brief tear down in reverse of init. Safe to call if init bailed out.
 *
 * @return void
 */
void platform__shutdown(void)
{
    widget_image__cache_shutdown();
    font__shutdown();
    renderer__shutdown();
    _platform_linux_wayland_client_internal__term_egl();
    _platform_linux_wayland_client_internal__close_display();
    memory_manager__shutdown();
}

/**
 * @brief bring the platform window to the top of the z-order and pin it there (Win32: SWP_TOPMOST + SetForegroundWindow). Called from the visual-regression test runner before capturing so nothing obscures the window's client area during BitBlt. No-op on platforms other than Windows.
 *
 * @return void
 */
void platform__set_topmost(void)
{
    // Wayland clients don't control their own z-order -- that's
    // the compositor's job. No-op.
}

/**
 * @brief capture the current window client area to a 32-bit BGRA .bmp file. Returns TRUE on success, FALSE on any IO or API failure. Intended for the visual-regression test runner under `tests/visual/`; not used by normal host apps. Path is a filesystem path in the native encoding. Overwrites any existing file. Format choice: BMP over PNG because BMP needs no compression dependency (stb_image_write + zlib) and every browser / image viewer displays it natively. Resulting files are larger than PNG but test captures are small (typical 800x600 ~= 1.8 MB). Must be called AFTER a frame has been rendered + presented (the captured pixels come from the window's composited client area); typically the test runner ticks 5+ frames to let animations settle, then calls this. Only the Win32 backend implements this today; other backends return FALSE.
 *
 * @param const char* path -> output filesystem path.
 *
 * @return boole -> TRUE on success.
 */
boole platform__capture_bmp(const char* path)
{
    (void)path;
    // Wayland has no XGetImage equivalent for clients to grab
    // their own window. screencopy protocols exist but require
    // separate binding; not needed for visual tests since the
    // renderer's output can be read back with glReadPixels if
    // A3+ wires that up. For now, return FALSE.
    return FALSE;
}

// ============================================================================
// Wayland-specific extension hook (platform_linux_wayland_client.h)
// ============================================================================

/**
 * @brief return the live Wayland display connection owned by the platform backend. Valid only between platform__init and platform__shutdown -- NULL before / after.
 *
 * @return struct wl_display* -> the connection, or NULL if not initialized.
 */
struct wl_display* platform_wayland__get_display(void)
{
    return _platform_linux_wayland_client_internal__g.display;
}

// EGL handles exposed as void* so consumers can import dmabufs
// forwarded by the compositor (meek_shell_v1.toplevel_buffer) into
// textures this context can sample. See header for the cast pattern.
// Before platform__init both are EGL_NO_DISPLAY / EGL_NO_CONTEXT,
// which EGL spec guarantees fit into a void* on every platform we
// target -- no NULL-specific check needed.
/**
 * @brief expose the backend's EGL display + context as opaque pointers so downstream code (meek-shell's meek_shell_v1 client) can `eglCreateImage(EGL_LINUX_DMA_BUF_EXT, ...)` dmabuf fds forwarded by the compositor and bind the resulting EGLImage as a GL texture via `glEGLImageTargetTexture2DOES`. Textures created against this context can be sampled by the same meek-ui renderer that's running the shell's scene, without any cross-display EGL surgery. Returned pointers are EGLDisplay / EGLContext, cast to void* to avoid dragging EGL headers into every consumer of this header. Caller casts back: `EGLDisplay d = (EGLDisplay)platform_wayland__get_egl_display();` Both are NULL before `platform__init` and after `platform__shutdown`.
 *
 * @return void* -> EGLContext, or NULL.
 */
void* platform_wayland__get_egl_display(void)
{
    return (void*)_platform_linux_wayland_client_internal__g.egl_display;
}

void* platform_wayland__get_egl_context(void)
{
    return (void*)_platform_linux_wayland_client_internal__g.egl_context;
}

// Create a worker-thread context sharing display + texture namespace
// with our main render context. The share_context parameter to
// eglCreateContext means anything the worker creates (textures,
// EGLImages bound to textures) is visible to the main context. The
// caller calls eglMakeCurrent on its own thread; the main thread's
// current context is NOT disturbed by this function.
/**
 * @brief create a worker-thread-friendly EGL context that shares texture + EGLImage namespace with the main render context. Used by off-main- thread import paths (meek-shell's texture_import_worker) that need to create GL textures the main thread can later sample without an extra copy. The new context uses the same EGLConfig and a `share_context` of the main render context, so: - glGenTextures on the worker produces ids visible to the main context. - eglCreateImage + glEGLImageTargetTexture2DOES on the worker produces textures that, after glFinish() on the worker, can be bound + sampled on the main thread without state corruption. The caller is responsible for eglMakeCurrent'ing this context on the worker thread (NOT on the main thread -- doing so would steal the current context from the renderer). Caller also owns eglDestroyContext on shutdown.
 *
 * @param void** out_ctx -> On success, written with the new EGLContext (cast to void*).
 *
 * @return boole -> TRUE on success, FALSE if the platform hasn't initialized EGL yet or eglCreateContext failed.
 */
boole platform_wayland__create_shared_egl_context(void** out_ctx)
{
    if (out_ctx == NULL_POINTER)
    {
        return FALSE;
    }
    *out_ctx = NULL_POINTER;
    EGLDisplay dpy = _platform_linux_wayland_client_internal__g.egl_display;
    EGLContext main_ctx = _platform_linux_wayland_client_internal__g.egl_context;
    EGLConfig  cfg = _platform_linux_wayland_client_internal__g.egl_config;
    if (dpy == EGL_NO_DISPLAY || main_ctx == EGL_NO_CONTEXT || cfg == NULL_POINTER)
    {
        log_warn("%s", "platform_wayland: create_shared_egl_context called before EGL ready");
        return FALSE;
    }
    EGLint attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE,
    };
    EGLContext shared = eglCreateContext(dpy, cfg, main_ctx, attrs);
    if (shared == EGL_NO_CONTEXT)
    {
        log_error("%s %x", "platform_wayland: eglCreateContext(shared) failed err=0x", eglGetError());
        return FALSE;
    }
    *out_ctx = (void*)shared;
    log_info("%s %p %s %p %s", "platform_wayland: shared EGL context created", (void*)shared, "(share_ctx=", (void*)main_ctx, ")");
    return TRUE;
}

// ============================================================================
// wl_seat / wl_pointer / wl_touch
// ============================================================================
// Input pipeline on the client side:
// compositor (libinput) -> wl_seat protocol -> our pointer/touch
// listeners -> translate to meek-ui coords -> scene__on_mouse_*
// calls -> meek-ui's hit-tester fires registered handlers from
// the .ui file (on_click, etc.)
// We currently unify pointer + touch into the same mouse event
// surface (scene__on_mouse_button(0, down, x, y)) because meek-ui's
// scene doesn't yet distinguish multi-touch; touch `id` is
// essentially ignored beyond "is there any finger down right now".
// Multi-touch (pinch, two-finger scroll) is future work.
// Coordinates arrive as wl_fixed_t (24.8 fixed point, in surface-
// local pixels). wl_fixed_to_int truncates the fractional part --
// fine for a hit-test at button resolution; we'd need
// wl_fixed_to_double for drag inertia / gestures later.

static void _platform_linux_wayland_client_internal__on_seat_capabilities(void* data, struct wl_seat* s, uint32_t caps)
{
    (void)data;

    // Pointer: create if present, destroy if removed. Capability
    // can flap at runtime (e.g. USB mouse plugged / unplugged) --
    // compositor re-sends the capabilities event each time.
    boole has_ptr = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;
    if (has_ptr && _platform_linux_wayland_client_internal__g.pointer == NULL_POINTER)
    {
        _platform_linux_wayland_client_internal__g.pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(_platform_linux_wayland_client_internal__g.pointer, &_platform_linux_wayland_client_internal__pointer_listener, NULL_POINTER);
        log_info("%s", "platform_linux_wayland_client: wl_pointer acquired");
    }
    else if (has_ptr == FALSE && _platform_linux_wayland_client_internal__g.pointer != NULL_POINTER)
    {
        wl_pointer_release(_platform_linux_wayland_client_internal__g.pointer);
        _platform_linux_wayland_client_internal__g.pointer = NULL_POINTER;
    }

    // Touch: same dance. On a phone this is usually the ONLY
    // capability advertised (no physical pointer, no keyboard).
    boole has_touch = (caps & WL_SEAT_CAPABILITY_TOUCH) != 0;
    if (has_touch && _platform_linux_wayland_client_internal__g.touch == NULL_POINTER)
    {
        _platform_linux_wayland_client_internal__g.touch = wl_seat_get_touch(s);
        wl_touch_add_listener(_platform_linux_wayland_client_internal__g.touch, &_platform_linux_wayland_client_internal__touch_listener, NULL_POINTER);
        log_info("%s", "platform_linux_wayland_client: wl_touch acquired");
    }
    else if (has_touch == FALSE && _platform_linux_wayland_client_internal__g.touch != NULL_POINTER)
    {
        wl_touch_release(_platform_linux_wayland_client_internal__g.touch);
        _platform_linux_wayland_client_internal__g.touch = NULL_POINTER;
    }

    // Keyboard: grab if present. The compositor will send a
    // wl_keyboard.keymap event first, which we mmap + feed to
    // xkbcommon so subsequent key/modifiers events translate
    // correctly. Needed for demo-settings' text field, shell
    // shortcuts, and any foreign key events the compositor
    // forwards via meek_shell_v1.route_keyboard_key.
    boole has_kb = (caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
    if (has_kb && _platform_linux_wayland_client_internal__g.keyboard == NULL_POINTER)
    {
        _platform_linux_wayland_client_internal__g.keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(_platform_linux_wayland_client_internal__g.keyboard, &_platform_linux_wayland_client_internal__keyboard_listener, NULL_POINTER);
        log_info("%s", "platform_linux_wayland_client: wl_keyboard acquired");
    }
    else if (has_kb == FALSE && _platform_linux_wayland_client_internal__g.keyboard != NULL_POINTER)
    {
        wl_keyboard_release(_platform_linux_wayland_client_internal__g.keyboard);
        _platform_linux_wayland_client_internal__g.keyboard = NULL_POINTER;
    }
}

static void _platform_linux_wayland_client_internal__on_seat_name(void* data, struct wl_seat* s, const char* name)
{
    (void)data; (void)s;
    log_info("%s %s %s", "platform_linux_wayland_client: seat name='", name, "'");
}

// -- wl_pointer -------------------------------------------------------------

static void _platform_linux_wayland_client_internal__on_pointer_enter(void* data, struct wl_pointer* p, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)p; (void)serial; (void)s;
    _platform_linux_wayland_client_internal__g.last_x = wl_fixed_to_int(x);
    _platform_linux_wayland_client_internal__g.last_y = wl_fixed_to_int(y);
    scene__on_mouse_move(_platform_linux_wayland_client_internal__g.last_x, _platform_linux_wayland_client_internal__g.last_y);
    _platform_linux_wayland_client_internal__force_render_now();
}

static void _platform_linux_wayland_client_internal__on_pointer_leave(void* data, struct wl_pointer* p, uint32_t serial, struct wl_surface* s)
{
    (void)data; (void)p; (void)serial; (void)s;
    // No dedicated "mouse-left" in meek-ui's scene API. Park the
    // cursor off-screen so any hover-state styling fades out on
    // the next tick.
    scene__on_mouse_move(-1, -1);
    _platform_linux_wayland_client_internal__force_render_now();
}

static void _platform_linux_wayland_client_internal__on_pointer_motion(void* data, struct wl_pointer* p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)p; (void)time;
    _platform_linux_wayland_client_internal__g.last_x = wl_fixed_to_int(x);
    _platform_linux_wayland_client_internal__g.last_y = wl_fixed_to_int(y);
    scene__on_mouse_move(_platform_linux_wayland_client_internal__g.last_x, _platform_linux_wayland_client_internal__g.last_y);
    _platform_linux_wayland_client_internal__force_render_now();
}

static void _platform_linux_wayland_client_internal__on_pointer_button(void* data, struct wl_pointer* p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    (void)data; (void)p; (void)serial; (void)time;
    // button codes are Linux evdev codes: BTN_LEFT=0x110,
    // BTN_RIGHT=0x111, BTN_MIDDLE=0x112. Map them to meek-ui's
    // 0/1/2 indices.
    int b;
    switch (button)
    {
        case 0x110: b = 0; break; // BTN_LEFT
        case 0x111: b = 1; break; // BTN_RIGHT
        case 0x112: b = 2; break; // BTN_MIDDLE
        default:    return; // side buttons; ignore
    }
    boole down = (state == WL_POINTER_BUTTON_STATE_PRESSED);
    scene__on_mouse_button(b, down, _platform_linux_wayland_client_internal__g.last_x, _platform_linux_wayland_client_internal__g.last_y);
    _platform_linux_wayland_client_internal__force_render_now();
}

static void _platform_linux_wayland_client_internal__on_pointer_axis(void* data, struct wl_pointer* p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data; (void)p; (void)time;
    // axis=VERTICAL_SCROLL is what scroll wheels report. value is
    // in units of "scroll distance" (usually ~10 per notch). meek-ui
    // expects +1/-1 per notch; scale down and clamp.
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
    {
        return;
    }
    float v = (float)wl_fixed_to_double(value) / 10.0f;
    if (v > 0) v =  1.0f; // normalise direction; actual magnitude doesn't matter for scene
    if (v < 0) v = -1.0f;
    scene__on_mouse_wheel(_platform_linux_wayland_client_internal__g.last_x, _platform_linux_wayland_client_internal__g.last_y, v);
    _platform_linux_wayland_client_internal__force_render_now();
}

static void _platform_linux_wayland_client_internal__on_pointer_frame(void* data, struct wl_pointer* p)
{
    (void)data; (void)p;
    // Frame boundary -- compositor groups related events (motion+button)
    // into a frame. We dispatch per-event already so nothing to do.
}

static void _platform_linux_wayland_client_internal__on_pointer_axis_source(void* data, struct wl_pointer* p, uint32_t source)
{ (void)data; (void)p; (void)source; }

static void _platform_linux_wayland_client_internal__on_pointer_axis_stop(void* data, struct wl_pointer* p, uint32_t time, uint32_t axis)
{ (void)data; (void)p; (void)time; (void)axis; }

static void _platform_linux_wayland_client_internal__on_pointer_axis_discrete(void* data, struct wl_pointer* p, uint32_t axis, int32_t discrete)
{ (void)data; (void)p; (void)axis; (void)discrete; }

// -- wl_output --------------------------------------------------------------
// We care about exactly one piece of wl_output state: the current
// mode's pixel dimensions. These feed gesture-classification code in
// downstream consumers (meek-shell's edge-swipe recognizer needs
// panel-native px to compute zone thirds; passing the shell's logical
// surface size produces wrong zones on phones where logical != native).
// geometry / done / scale are observed but not consumed.

static void _platform_linux_wayland_client_internal__on_output_geometry( void* data, struct wl_output* o, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char* make, const char* model, int32_t transform)
{
    (void)data; (void)o; (void)x; (void)y;
    (void)subpixel; (void)make; (void)model; (void)transform;
    // Cache physical panel dimensions (mm) so the shell can run
    // the same DPI-based scale auto-detect the compositor does --
    // used to seed scale_wayland_pct's default at first run.
    // Mode width/height arrive in a SEPARATE event (mode); the
    // shell waits for BOTH before computing scale.
    _platform_linux_wayland_client_internal__g.output_mm_w = (int)physical_width;
    _platform_linux_wayland_client_internal__g.output_mm_h = (int)physical_height;
    log_info("%s %d %s %d %s", "wl_output.geometry physical_size", (int)physical_width, "x", (int)physical_height, "mm");
}

static void _platform_linux_wayland_client_internal__on_output_mode( void* data, struct wl_output* o, uint32_t flags, int32_t width, int32_t height, int32_t refresh)
{
    (void)data; (void)o;
    // wl_output advertises every supported mode at registry time,
    // distinguishing them via flags. We only latch the one with
    // WL_OUTPUT_MODE_CURRENT set -- the mode the panel is actually
    // configured to right now.
    if ((flags & WL_OUTPUT_MODE_CURRENT) == 0)
    {
        return;
    }
    _platform_linux_wayland_client_internal__g.output_pixel_w = (int)width;
    _platform_linux_wayland_client_internal__g.output_pixel_h = (int)height;
    _platform_linux_wayland_client_internal__g.output_refresh_mhz = (int)refresh;
    log_info("%s %d %s %d %s %d %s %03d %s", "wl_output.mode current", (int)width, "x", (int)height, "@", refresh / 1000, ".", refresh % 1000, "Hz");
}

static void _platform_linux_wayland_client_internal__on_output_done( void* data, struct wl_output* o)
{ (void)data; (void)o; }

static void _platform_linux_wayland_client_internal__on_output_scale( void* data, struct wl_output* o, int32_t factor)
{ (void)data; (void)o; (void)factor; }

// -- wl_touch ---------------------------------------------------------------

static void _platform_linux_wayland_client_internal__on_touch_down(void* data, struct wl_touch* t, uint32_t serial, uint32_t time, struct wl_surface* s, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)t; (void)serial; (void)time; (void)s;
    log_trace("%s %d %s %d %s %d", "wl_touch.down id=", id, "x=", wl_fixed_to_int(x), "y=", wl_fixed_to_int(y));
    // First-finger-down. We only track one finger at a time for
    // now; additional fingers on an already-active gesture are
    // ignored. Multi-touch support would track per-id state here.
    if (_platform_linux_wayland_client_internal__g.touch_active_id != -1)
    {
        return; // already tracking another finger
    }
    _platform_linux_wayland_client_internal__g.touch_active_id = id;
    _platform_linux_wayland_client_internal__g.touch_x = wl_fixed_to_int(x);
    _platform_linux_wayland_client_internal__g.touch_y = wl_fixed_to_int(y);
    _platform_linux_wayland_client_internal__g.last_x  = _platform_linux_wayland_client_internal__g.touch_x;
    _platform_linux_wayland_client_internal__g.last_y  = _platform_linux_wayland_client_internal__g.touch_y;
    _platform_linux_wayland_client_internal__g.swipe_start_x = _platform_linux_wayland_client_internal__g.touch_x;
    _platform_linux_wayland_client_internal__g.swipe_start_y = _platform_linux_wayland_client_internal__g.touch_y;

    // Fire as "button 0 pressed". meek-ui's hit-tester + on_click
    // dispatch works on mouse semantics, which matches single-
    // finger touch cleanly (down + up at same position = click).
    scene__on_mouse_button(0, TRUE, _platform_linux_wayland_client_internal__g.touch_x, _platform_linux_wayland_client_internal__g.touch_y);
    _platform_linux_wayland_client_internal__force_render_now();
}

static void _platform_linux_wayland_client_internal__on_touch_up(void* data, struct wl_touch* t, uint32_t serial, uint32_t time, int32_t id)
{
    (void)data; (void)t; (void)serial; (void)time;
    // wl_touch.up carries no coords (the finger is gone). Use the
    // last-known position from down / motion. Only react if the
    // released finger is the one we were tracking.
    if (id != _platform_linux_wayland_client_internal__g.touch_active_id)
    {
        log_trace("%s %d %s %d %s", "wl_touch.up id=", id, "(not tracked active=", _platform_linux_wayland_client_internal__g.touch_active_id, ") -- ignored");
        return;
    }
    log_trace("%s %d %s %d %s %d %s", "wl_touch.up id=", id, "at last (", _platform_linux_wayland_client_internal__g.touch_x, ",", _platform_linux_wayland_client_internal__g.touch_y, ")");

    // Edge-swipe-back classifier.
    // Fires the named gesture handler "on_swipe_back" when the touch:
    // 1. started inside the left-edge zone (start_x < EDGE_PX),
    // 2. travelled rightwards by at least COMMIT_PX,
    // 3. moved more horizontally than vertically (so a diagonal
    // drag that's MOSTLY a vertical scroll doesn't trigger).
    // EDGE_PX is generous (48 px) so the gesture is reachable on a
    // 1080-wide phone without micro-aiming; COMMIT_PX is large
    // enough (96 px) that the user can't accidentally trigger it
    // mid-tap. Apps that don't bind on_swipe_back ignore the event
    // (scene__dispatch_gesture_by_name is a cheap no-op when no
    // handler is registered).
    {
        const int64 EDGE_PX   = 48;
        const int64 COMMIT_PX = 96;
        int64 dx = (int64)_platform_linux_wayland_client_internal__g.touch_x
                 - _platform_linux_wayland_client_internal__g.swipe_start_x;
        int64 dy = (int64)_platform_linux_wayland_client_internal__g.touch_y
                 - _platform_linux_wayland_client_internal__g.swipe_start_y;
        int64 abs_dy = (dy < 0) ? -dy : dy;
        if (_platform_linux_wayland_client_internal__g.swipe_start_x < EDGE_PX && dx >= COMMIT_PX && dx > abs_dy)
        {
            log_info("%s %d %s %d", "platform_linux_wayland_client: edge-swipe-back recognised dx=", (int)dx, "dy=", (int)dy);
            scene__dispatch_gesture_by_name("on_swipe_back");
        }
    }

    scene__on_mouse_button(0, FALSE, _platform_linux_wayland_client_internal__g.touch_x, _platform_linux_wayland_client_internal__g.touch_y);
    _platform_linux_wayland_client_internal__g.touch_active_id = -1;
    _platform_linux_wayland_client_internal__force_render_now();
}

static void _platform_linux_wayland_client_internal__on_touch_motion(void* data, struct wl_touch* t, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)t; (void)time;
    if (id != _platform_linux_wayland_client_internal__g.touch_active_id)
    {
        log_trace("%s %d %s %d %s", "wl_touch.motion id=", id, "(expected", _platform_linux_wayland_client_internal__g.touch_active_id, ") -- DROPPED");
        return;
    }
    _platform_linux_wayland_client_internal__g.touch_x = wl_fixed_to_int(x);
    _platform_linux_wayland_client_internal__g.touch_y = wl_fixed_to_int(y);
    _platform_linux_wayland_client_internal__g.last_x  = _platform_linux_wayland_client_internal__g.touch_x;
    _platform_linux_wayland_client_internal__g.last_y  = _platform_linux_wayland_client_internal__g.touch_y;
    scene__on_mouse_move(_platform_linux_wayland_client_internal__g.touch_x, _platform_linux_wayland_client_internal__g.touch_y);
    _platform_linux_wayland_client_internal__force_render_now();
}

static void _platform_linux_wayland_client_internal__on_touch_frame(void* data, struct wl_touch* t)
{ (void)data; (void)t; /*see pointer_frame*/ }

static void _platform_linux_wayland_client_internal__on_touch_cancel(void* data, struct wl_touch* t)
{
    (void)data; (void)t;
    // Compositor cancels touch when grab is broken (e.g. a gesture
    // was recognized and taken over by the compositor). Release
    // any held "button" so widgets don't stay stuck in :active.
    if (_platform_linux_wayland_client_internal__g.touch_active_id != -1)
    {
        log_trace("%s %d %s %d %s %d %s", "wl_touch.cancel -- releasing tracked id=", _platform_linux_wayland_client_internal__g.touch_active_id, "at (", _platform_linux_wayland_client_internal__g.touch_x, ",", _platform_linux_wayland_client_internal__g.touch_y, ")");
        scene__on_mouse_button(0, FALSE, _platform_linux_wayland_client_internal__g.touch_x, _platform_linux_wayland_client_internal__g.touch_y);
        _platform_linux_wayland_client_internal__g.touch_active_id = -1;
        _platform_linux_wayland_client_internal__force_render_now();
    }
}

static void _platform_linux_wayland_client_internal__on_touch_shape(void* data, struct wl_touch* t, int32_t id, wl_fixed_t major, wl_fixed_t minor)
{ (void)data; (void)t; (void)id; (void)major; (void)minor; }

static void _platform_linux_wayland_client_internal__on_touch_orientation(void* data, struct wl_touch* t, int32_t id, wl_fixed_t orientation)
{ (void)data; (void)t; (void)id; (void)orientation; }

// -- wl_keyboard -----------------------------------------------------------
// Protocol event order:
// keymap       once, on get_keyboard, with the xkb keymap as a
// mmap-able fd.
// repeat_info  once if version >= 4, says rate/delay for auto-
// repeat. We don't implement repeat yet; log only.
// enter        when a surface of ours gains keyboard focus.
// carries an array of already-pressed keys (empty
// for our use case).
// modifiers    any time shift/ctrl/alt/super state changes.
// must be applied to xkb_state BEFORE the next key
// event so translation uses the right state.
// key          press / release of a physical key. event's
// `key` field is a Linux evdev keycode minus 8
// (xkb convention: xkb_keycode = evdev_keycode + 8).
// leave        focus moves off our surface.
// Translation. Each `key` event carries an evdev keycode. We:
// 1. Add 8 to get the xkb keycode (evdev->xkb offset).
// 2. xkb_state_key_get_one_sym gives the xkb_keysym_t with
// current modifier state applied. Keysym is what the user
// "typed" (e.g. XKB_KEY_a vs XKB_KEY_A depending on shift).
// 3. xkb_keysym_to_utf32 converts keysym -> unicode codepoint
// for printable characters. Non-printable (arrows, F-keys,
// enter, backspace) return 0; we handle those via
// scene__on_key with a VK_ code.

static void _platform_linux_wayland_client_internal__on_keyboard_keymap(void* data, struct wl_keyboard* k, uint32_t format, int32_t fd, uint32_t size)
{
    (void)data; (void)k;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
    {
        log_warn("%s %u %s", "wl_keyboard.keymap: unexpected format=", format, "; ignoring");
        close(fd);
        return;
    }

    // Map the compositor's keymap file into our address space read-
    // only. MAP_PRIVATE is required per spec -- the compositor may
    // still have the fd open and we mustn't modify shared pages.
    void* map = mmap(NULL_POINTER, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED)
    {
        log_error("%s %u %s %s", "wl_keyboard.keymap: mmap(", size, "bytes) failed:", strerror(errno));
        close(fd);
        return;
    }

    if (_platform_linux_wayland_client_internal__g.xkb_ctx == NULL_POINTER)
    {
        _platform_linux_wayland_client_internal__g.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (_platform_linux_wayland_client_internal__g.xkb_ctx == NULL_POINTER)
        {
            log_error("%s", "wl_keyboard.keymap: xkb_context_new failed");
            munmap(map, size);
            close(fd);
            return;
        }
    }

    // Replace keymap + state atomically. xkb_keymap_new_from_string
    // copies the text, so we can unmap + close immediately after.
    // (String is NUL-terminated per protocol: the compositor writes
    // `strlen(keymap) + 1` bytes.)
    struct xkb_keymap* km = xkb_keymap_new_from_string( _platform_linux_wayland_client_internal__g.xkb_ctx, (const char*)map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    close(fd);

    if (km == NULL_POINTER)
    {
        log_error("%s", "wl_keyboard.keymap: xkb_keymap_new_from_string failed");
        return;
    }

    struct xkb_state* st = xkb_state_new(km);
    if (st == NULL_POINTER)
    {
        log_error("%s", "wl_keyboard.keymap: xkb_state_new failed");
        xkb_keymap_unref(km);
        return;
    }

    if (_platform_linux_wayland_client_internal__g.xkb_state != NULL_POINTER)
    {
        xkb_state_unref(_platform_linux_wayland_client_internal__g.xkb_state);
    }
    if (_platform_linux_wayland_client_internal__g.xkb_keymap != NULL_POINTER)
    {
        xkb_keymap_unref(_platform_linux_wayland_client_internal__g.xkb_keymap);
    }
    _platform_linux_wayland_client_internal__g.xkb_keymap = km;
    _platform_linux_wayland_client_internal__g.xkb_state  = st;

    log_info("%s %u %s", "platform_linux_wayland_client: xkb keymap installed (", size, "bytes)");
}

static void _platform_linux_wayland_client_internal__on_keyboard_enter(void* data, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s, struct wl_array* keys)
{
    (void)data; (void)k; (void)serial; (void)s; (void)keys;
    // We gained keyboard focus. No per-enter state to set up --
    // xkb_state persists across enter/leave. Clear any sticky
    // "this character is already held" accumulation (we don't
    // track it, but a future enter-carries-pressed-keys path
    // would iterate `keys` here).
    log_trace("%s", "wl_keyboard.enter");
}

static void _platform_linux_wayland_client_internal__on_keyboard_leave(void* data, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s)
{
    (void)data; (void)k; (void)serial; (void)s;
    log_trace("%s", "wl_keyboard.leave");
}

static void _platform_linux_wayland_client_internal__on_keyboard_key(void* data, struct wl_keyboard* k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    (void)data; (void)k; (void)serial; (void)time;
    if (_platform_linux_wayland_client_internal__g.xkb_state == NULL_POINTER)
    {
        // keymap event hasn't arrived yet (unusual; compositor is
        // supposed to send it BEFORE the first key). Drop.
        log_warn("%s %u %s %u %s", "wl_keyboard.key before keymap; dropping (key=", key, "state=", state, ")");
        return;
    }

    boole pressed = (state == WL_KEYBOARD_KEY_STATE_PRESSED);

    // xkb keycode = evdev keycode + 8. (Historical xkb convention
    // carried over from X11. Every Wayland client does this.)
    xkb_keycode_t xkb_kc = (xkb_keycode_t)(key + 8);
    xkb_keysym_t sym = xkb_state_key_get_one_sym( _platform_linux_wayland_client_internal__g.xkb_state, xkb_kc);

    // Translate to a unicode codepoint for printable keys, and a
    // VK_ code for non-printable ones. We deliver both when both
    // are meaningful (e.g. return = codepoint 0x0D AND VK_RETURN),
    // so widgets that listen on either path work.

    // VK code: Windows virtual-key convention, which is what
    // widget_input / widget_textarea listen for. Mirror of the
    // subset widget_keyboard uses on the dispatch side.
    int64 vk = 0;
    switch (sym)
    {
        case XKB_KEY_BackSpace: vk = 0x08; break;
        case XKB_KEY_Tab:       vk = 0x09; break;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:  vk = 0x0D; break;
        case XKB_KEY_Escape:    vk = 0x1B; break;
        case XKB_KEY_Left:      vk = 0x25; break;
        case XKB_KEY_Up:        vk = 0x26; break;
        case XKB_KEY_Right:     vk = 0x27; break;
        case XKB_KEY_Down:      vk = 0x28; break;
        case XKB_KEY_Delete:    vk = 0x2E; break;
        case XKB_KEY_Home:      vk = 0x24; break;
        case XKB_KEY_End:       vk = 0x23; break;
        default: break;
    }

    if (vk != 0)
    {
        // Non-printable key: deliver via scene__on_key only. Don't
        // also emit scene__on_char; widget_input/textarea already
        // listen for VK_BACK / VK_RETURN in on_key.
        scene__on_key(vk, pressed);
        _platform_linux_wayland_client_internal__force_render_now();
        log_trace("%s %u %s %#x %s %x %s", "wl_keyboard.key evdev=", key, "sym=", sym, "-> VK 0x", (unsigned)vk, pressed ? "DOWN" : "UP");
        return;
    }

    // Printable key. Only deliver char on press (matching how
    // widget_input.input_on_char expects single codepoints per
    // keystroke, not auto-repeat duplicates).
    if (pressed)
    {
        uint32_t cp = xkb_keysym_to_utf32(sym);
        if (cp != 0)
        {
            scene__on_char((uint)cp);
            _platform_linux_wayland_client_internal__force_render_now();
            log_trace("%s %u %s %#x %s %04X", "wl_keyboard.key evdev=", key, "sym=", sym, "-> codepoint U+", cp);
        }
        else
        {
            log_trace("%s %u %s %#x %s", "wl_keyboard.key evdev=", key, "sym=", sym, ": no codepoint, no VK (dropped)");
        }
    }
}

static void _platform_linux_wayland_client_internal__on_keyboard_modifiers(void* data, struct wl_keyboard* k, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)data; (void)k; (void)serial;
    if (_platform_linux_wayland_client_internal__g.xkb_state == NULL_POINTER)
    {
        return;
    }
    // Push modifier state into xkb_state so the next
    // xkb_state_key_get_one_sym call returns a shifted / ctrl'd
    // keysym as appropriate.
    xkb_state_update_mask(_platform_linux_wayland_client_internal__g.xkb_state, depressed, latched, locked, /*depressed_layout*/ 0, /*latched_layout*/   0, group);
    log_trace("%s %u %s %u %s %u %s %u", "wl_keyboard.modifiers d=", depressed, "l=", latched, "lk=", locked, "g=", group);
}

static void _platform_linux_wayland_client_internal__on_keyboard_repeat_info(void* data, struct wl_keyboard* k, int32_t rate, int32_t delay)
{
    (void)data; (void)k;
    // Auto-repeat not implemented yet. Log for diagnostics.
    log_trace("%s %d %s %d %s", "wl_keyboard.repeat_info rate=", rate, "delay=", delay, "(not implemented)");
}
