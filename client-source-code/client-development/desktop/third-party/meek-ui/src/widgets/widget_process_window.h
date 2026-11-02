#ifndef WIDGET_PROCESS_WINDOW_H
#define WIDGET_PROCESS_WINDOW_H

// widget_process_window.h - public API for the <process-window/>
// widget. Consumed by meek-shell (the one consumer we expect) to
// attach the live texture of an external process's rendered buffer
// to a specific scene node.
// The widget itself (widget_process_window.c) lives in meek-ui
// because it's a regular meek-ui widget with a vtable; its
// TEXTURE SOURCE is external (the shell's toplevel_registry), which
// is why the setters here exist rather than a load-from-path
// attribute like widget_image uses.

#include <stdint.h>

#include "../gui_api.h"

struct gui_node_t;

GUI_API void     widget_process_window__register(void);

GUI_API void     widget_process_window__set_texture(struct gui_node_t* n, uint32_t gl_texture, int width, int height);

GUI_API uint32_t widget_process_window__get_handle(struct gui_node_t* n);

GUI_API void     widget_process_window__get_buffer_size(struct gui_node_t* n, int32_t* out_w, int32_t* out_h);

GUI_API void     widget_process_window__set_handle(struct gui_node_t* n, uint32_t handle);

// one overlay (sub-region) composited on top of the primary texture during emit
// coordinates and dimensions are in the source app's surface-local pixel space (the space the primary texture is in), so the widget maps them into its on-screen bounds with the same scale that maps the primary texture
// used by meek-shell to forward wl_subsurface state from the compositor without leaking the subsurface concept into meek-ui: this struct is just "a list of textured rectangles to draw on top of the main one"
struct widget_process_window_overlay
{
    // 0 = skip this entry
    uint32_t gl_texture;   // 0x0
    int32_t  pos_x;        // 0x4
    int32_t  pos_y;        // 0x8
    int      width;        // 0xC
    int      height;       // 0x10
};

#define WIDGET_PROCESS_WINDOW_MAX_OVERLAYS 16

GUI_API void widget_process_window__set_overlays(struct gui_node_t* n, const struct widget_process_window_overlay* arr, int count);

// Touch event phase, passed to the touch-forwarder callback.
enum widget_process_window_touch_phase
{
    WIDGET_PROCESS_WINDOW_TOUCH_DOWN   = 0,
    WIDGET_PROCESS_WINDOW_TOUCH_MOTION = 1,
    WIDGET_PROCESS_WINDOW_TOUCH_UP     = 2,
};

// Touch-forwarder callback. The widget calls this on every
// touch event (mouse_down / mouse_drag / mouse_up) it receives.
// `sx`, `sy` are the touch position translated into the SOURCE
// APP'S surface-local pixel space (using the same scale that
// maps the primary texture into the widget's bounds).
// The shell binds a forwarder that calls
// `meek_shell_v1_client__route_touch_*` for each phase, giving
// the foreign app a real-time touch stream — essential for
// scrolling and drag gestures, which the on_click + tap-synthesis
// path can't reproduce (clicks fire only on completed taps with
// no preserved motion data).
// `userdata` is the pointer the shell registered with set_touch_forwarder.
typedef void (*widget_process_window_touch_fn)(struct gui_node_t* n, enum widget_process_window_touch_phase phase, int32_t sx, int32_t sy, void* userdata);

GUI_API void widget_process_window__set_touch_forwarder(struct gui_node_t* n, widget_process_window_touch_fn fn, void* userdata);

#endif
