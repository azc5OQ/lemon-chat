// widget_process_window.c - <process-window/> draws another process's
// rendered buffer as a textured rectangle inside our scene.
// Canonical consumer: meek-shell. meek-shell receives dmabuf fds from
// meek-compositor via the meek_shell_v1 extension, imports them into
// GL textures (in meek-ui's EGL context so we can sample them here),
// and attaches each live texture to a <process-window> node placed
// wherever the shell wants the "app's window" to appear -- as a full-
// screen foreground, as a miniaturized task-switcher tile, whatever.
// Unlike widget_image, this widget does NOT own the texture. The
// texture lifecycle belongs to meek-shell's toplevel_registry. We
// just store the handle, the dimensions, and draw it on demand. If
// the registry replaces the texture (every commit from the source
// app, i.e. ~60 Hz), shell calls widget_process_window__set_texture
// on this node to point at the new GLuint. Old handles are freed by
// the registry, not by us.
// Pass: E4 (Phase 3 of the task-switcher + gestures feature pack).
// See meek-shell/session/roadmap.md section "Track E".

#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "renderer.h"
#include "scene.h" // scene__emit_default_bg + scene__frame_time_ms.
#include "third_party/log.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "widget_process_window.h"
#include "clib/stdlib.h"

// Per-node state. `gl_texture == 0` means "no buffer yet" -- we draw
// the node's bg rect in that case so a freshly-spawned tile isn't
// suddenly a black void before the first commit arrives.
typedef struct _widget_process_window_internal__state_t
{
    uint32_t gl_texture; // GL name cast from meek-shell's EGLImage import. 0 = unset.
    int      width; // source texture width in pixels.
    int      height; // source texture height in pixels.
    uint32_t handle; // meek_shell_v1 toplevel handle this widget tracks (informational, not used for draw).

    // Overlay subsurfaces (Track G3). Pushed by meek-shell via
    // widget_process_window__set_overlays whenever the
    // subsurface_registry for `handle` changes. Drawn on top of
    // the primary texture during emit, in arr-order, mapped into
    // bounds via the same width/height scale the primary uses.
    int      overlay_count;
    struct widget_process_window_overlay overlays[WIDGET_PROCESS_WINDOW_MAX_OVERLAYS];

    // Touch-forwarder hook. Set by the shell via
    // widget_process_window__set_touch_forwarder when this node
    // becomes the fullscreen-app target. NULL = not installed,
    // node falls back to standard meek-ui behavior.
    widget_process_window_touch_fn touch_forward_fn;
    void*                          touch_forward_userdata;

    // Track whether any motion fired between mouse_down and mouse_up.
    // If not (quick tap), we synthesize one before mouse_up because
    // some clients (firefox / GTK4 widgets) require at least one
    // touch.motion to recognize a tap as a click.
    int  touch_active;
    int  touch_motion_seen;
    int64 touch_last_sx;
    int64 touch_last_sy;
} _widget_process_window_internal__state_t;

static _widget_process_window_internal__state_t* _widget_process_window_internal__state_of(gui_node_t* n)
{
    if (n->user_data == NULL_POINTER)
    {
        // Lazy alloc, same pattern as widget_image. Nodes that never
        // get a handle set don't leak an empty state struct.
        n->user_data = GUI_CALLOC_T(1, sizeof(_widget_process_window_internal__state_t), MM_TYPE__NODE);
    }
    return (_widget_process_window_internal__state_t*)n->user_data;
}

// Parse a positive integer out of a decimal string. Returns 0 on
// parse failure, which is our sentinel for "no handle set" -- a
// malformed handle="" attribute is functionally indistinguishable
// from an unset one, which is fine for our purposes.
static uint32_t _widget_process_window_internal__parse_u32(const char* s)
{
    if (s == NULL_POINTER) return 0;
    uint32_t acc = 0;
    for (const char* p = s; *p != 0; p++)
    {
        if (*p < '0' || *p > '9') return 0;
        acc = acc * 10u + (uint32_t)(*p - '0');
    }
    return acc;
}

static boole process_window_apply_attribute(gui_node_t* n, char* name, char* value)
{
    // `handle="42"` binds this node to the meek_shell_v1 toplevel
    // with that handle. The texture isn't available until the shell
    // calls widget_process_window__set_texture -- this just records
    // which process the node is waiting for.
    if (stdlib__strcmp(name, "handle") == 0)
    {
        _widget_process_window_internal__state_t* st = _widget_process_window_internal__state_of(n);
        if (st == NULL_POINTER) return TRUE;
        st->handle = _widget_process_window_internal__parse_u32(value);
        return TRUE;
    }
    return FALSE;
}

static void process_window_layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale)
{
    gui_style_t* s = &n->resolved;

    // Size priority: explicit style size > source-texture natural
    // size > avail. The source texture's natural size (set via
    // set_texture from the shell) represents the app's preferred
    // window dimensions; we honor them if no style size overrides.
    _widget_process_window_internal__state_t* st = (_widget_process_window_internal__state_t*)n->user_data;

    // Size priority: explicit pixel (size_w/h) > percent of parent
    // (width_pct / height_pct) > source-texture natural size > avail.
    // Percent is what the shell uses for fullscreen presentation
    // (".fullscreen-app { width: 100%; height: 100%; }"); without it
    // we fall through to st->width which is the panel-native texture
    // size and can overflow the logical-size shell layout.
    float w;
    if (s->size_w > 0.0f)
    {
        w = s->size_w * scale;
    }
    else if (s->width_pct > 0.0f)
    {
        w = avail_w * (s->width_pct / 100.0f);
    }
    else if (st != NULL_POINTER && st->width > 0)
    {
        // Natural-size fallback. Source textures come from the
        // compositor in panel pixels; do NOT multiply by scale --
        // the source already rendered at its preferred size.
        w = (float)st->width;
    }
    else
    {
        w = avail_w;
    }

    float h;
    if (s->size_h > 0.0f)
    {
        h = s->size_h * scale;
    }
    else if (s->height_pct > 0.0f)
    {
        h = avail_h * (s->height_pct / 100.0f);
    }
    else if (st != NULL_POINTER && st->height > 0)
    {
        h = (float)st->height;
    }
    else
    {
        h = avail_h;
    }

    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;
}

static void process_window_emit_draws(gui_node_t* n, float scale)
{
    gui_style_t* s = &n->resolved;
    _widget_process_window_internal__state_t* st = (_widget_process_window_internal__state_t*)n->user_data;

    // Background + box-shadow + border via the standard helper so
    // the runtime effective_opacity cascade reaches us. Calling
    // submit_rect directly skipped the opacity multiply, which made
    // a fading parent (e.g. the task-switcher overlay during a
    // swipe-progress slide) leave the tile bg fully opaque while
    // the surrounding chrome dimmed. With emit_default_bg the bg /
    // shadow / border all multiply in op = effective_opacity, the
    // tile fades together with its backdrop.
    scene__emit_default_bg(n, scale);

    if (st == NULL_POINTER || st->gl_texture == 0)
    {
        // No texture yet. Render stops at the bg rect. Common case
        // during the single-frame gap between toplevel_added (node
        // created in shell) and the first toplevel_buffer (texture
        // set).
        return;
    }

    // Tint via accent_color (matches widget_image's behavior). Pure
    // black = sentinel for "no tint", use neutral white. Also
    // respect effective_opacity so scene-level fade animations work.
    gui_color_t tint;
    if (s->accent_color.r == 0.0f && s->accent_color.g == 0.0f && s->accent_color.b == 0.0f && s->accent_color.a == 0.0f)
    {
        tint.r = 1.0f; tint.g = 1.0f; tint.b = 1.0f; tint.a = 1.0f;
    }
    else
    {
        tint = s->accent_color;
    }
    tint.a *= n->effective_opacity;

    // Cast GLuint back to the void* form renderer__submit_image
    // accepts. The gles3 backend stashes a GLuint as (void*)(uintptr_t)
    // and unwraps the same way at sample time -- see
    // renderer__submit_image + renderer__create_texture_rgba in
    // gles3_renderer.c. For backends with no image pipeline (d3d11 /
    // d3d9 stubs return NULL from renderer__create_texture_rgba),
    // submit_image silently no-ops, so the bg rect is all you see.
    void* tex_as_void = (void*)(uintptr_t)st->gl_texture;
    renderer__submit_image(n->bounds, tex_as_void, tint);

    // ===== overlay subsurfaces (Track G3) =====
    // Map each overlay's parent-surface-local coords into our on-
    // screen bounds. The primary texture spans the full bounds at
    // its source dimensions (st->width x st->height); each overlay
    // is at (pos_x, pos_y) in that source space and has size
    // (overlay.width, overlay.height) in source pixels. So the
    // x-scale factor is bounds.w / st->width, and similarly for y.
    // If st->width or st->height are 0 (somehow no primary texture
    // dims yet but overlays present), fall back to drawing
    // overlays unscaled at their raw coords -- corner case worth
    // not crashing for. Mostly happens only during a brief startup
    // window before the first toplevel_buffer arrives.
    if (st->overlay_count > 0)
    {
        float sx = (st->width  > 0) ? (n->bounds.w / (float)st->width)  : 1.0f;
        float sy = (st->height > 0) ? (n->bounds.h / (float)st->height) : 1.0f;
        for (int i = 0; i < st->overlay_count; i++)
        {
            const struct widget_process_window_overlay* ov = &st->overlays[i];
            if (ov->gl_texture == 0)
            {
                continue;
            }
            gui_rect_t r;
            r.x = n->bounds.x + (float)ov->pos_x * sx;
            r.y = n->bounds.y + (float)ov->pos_y * sy;
            r.w = (float)ov->width  * sx;
            r.h = (float)ov->height * sy;
            renderer__submit_image(r, (void*)(uintptr_t)ov->gl_texture, tint);
        }
    }

    // Throttled diag: log effective_opacity once per ~1s when it's
    // visibly in fade range so we can confirm the cascade reaches
    // the texture (vs being stuck at 1.0). Disabled when fully
    // opaque -- only fires during the swipe-progress reveal.
    {
        static int64 last_log_ms = 0;
        if (n->effective_opacity < 0.99f)
        {
            int64 now_ms = scene__frame_time_ms();
            if (now_ms - last_log_ms > 500)
            {
                last_log_ms = now_ms;
                log_info("%s %.3f %s %.3f %s %.0f %s %.0f %.0f %s %.0f %s", "[dbg-fade] process-window effective_op=", n->effective_opacity, "tint.a=", tint.a, "bounds=(", n->bounds.x, ",", n->bounds.y, n->bounds.w, "x", n->bounds.h, ")");
            }
        }
    }
}

static void process_window_on_destroy(gui_node_t* n)
{
    // We don't own the texture -- meek-shell's toplevel_registry
    // does. Just free our state struct. Shell's
    // toplevel_registry__remove handles glDeleteTextures when the
    // source process exits.
    if (n->user_data != NULL_POINTER)
    {
        GUI_FREE(n->user_data);
        n->user_data = NULL_POINTER;
    }
}

// Coord translation: shell-pixel (x, y) within the widget's
// on-screen bounds → SOURCE-APP pixel space (the texture's
// natural width/height). Same math as the now-removed
// __process_window_tap synthesis used to do, factored out so
// every touch phase computes coords consistently.
// Returns 0 on success, -1 if state is unusable (no source
// dimensions yet — happens during the brief gap between
// toplevel_added and the first toplevel_buffer commit).
static int _widget_process_window_internal__translate_coords(gui_node_t* n, int64 x, int64 y, int32_t* out_sx, int32_t* out_sy)
{
    _widget_process_window_internal__state_t* st = (_widget_process_window_internal__state_t*)n->user_data;
    if (st == NULL_POINTER || st->width <= 0 || st->height <= 0)
    {
        return -1;
    }
    if (n->bounds.w <= 0.0f || n->bounds.h <= 0.0f)
    {
        return -1;
    }

    float fx = ((float)x - n->bounds.x) / n->bounds.w; // 0..1 within widget
    float fy = ((float)y - n->bounds.y) / n->bounds.h;
    *out_sx = (int32_t)(fx * (float)st->width);
    *out_sy = (int32_t)(fy * (float)st->height);
    return 0;
}

static void process_window_on_mouse_down(gui_node_t* n, int64 x, int64 y, int64 button)
{
    (void)button;
    _widget_process_window_internal__state_t* st = _widget_process_window_internal__state_of(n);
    if (st == NULL_POINTER || st->touch_forward_fn == NULL_POINTER)
    {
        return;
    }

    int32_t sx = 0, sy = 0;
    if (_widget_process_window_internal__translate_coords(n, x, y, &sx, &sy) != 0)
    {
        return;
    }

    st->touch_active = 1;
    st->touch_motion_seen = 0;
    st->touch_last_sx = sx;
    st->touch_last_sy = sy;
    st->touch_forward_fn(n, WIDGET_PROCESS_WINDOW_TOUCH_DOWN, sx, sy, st->touch_forward_userdata);
}

static boole process_window_on_mouse_drag(gui_node_t* n, int64 x, int64 y)
{
    _widget_process_window_internal__state_t* st = (_widget_process_window_internal__state_t*)n->user_data;
    if (st == NULL_POINTER || st->touch_forward_fn == NULL_POINTER || st->touch_active == FALSE)
    {
        return FALSE;
    }

    int32_t sx = 0, sy = 0;
    if (_widget_process_window_internal__translate_coords(n, x, y, &sx, &sy) != 0)
    {
        return FALSE;
    }

    // Skip motion events that don't actually move (some platforms
    // synthesize redundant updates). A real-pixel-difference threshold
    // of 1 catches stationary drift without dropping fine scroll motion.
    if (sx == st->touch_last_sx && sy == st->touch_last_sy)
    {
        return TRUE;
    }

    st->touch_last_sx = sx;
    st->touch_last_sy = sy;
    st->touch_motion_seen = 1;
    st->touch_forward_fn(n, WIDGET_PROCESS_WINDOW_TOUCH_MOTION, sx, sy, st->touch_forward_userdata);
    return TRUE;
}

static void process_window_on_mouse_up(gui_node_t* n, int64 x, int64 y, int64 button)
{
    (void)button;
    _widget_process_window_internal__state_t* st = (_widget_process_window_internal__state_t*)n->user_data;
    if (st == NULL_POINTER || st->touch_forward_fn == NULL_POINTER || st->touch_active == FALSE)
    {
        return;
    }

    int32_t sx = 0, sy = 0;
    if (_widget_process_window_internal__translate_coords(n, x, y, &sx, &sy) != 0)
    {
        sx = (int32_t)st->touch_last_sx;
        sy = (int32_t)st->touch_last_sy;
    }

    // Quick-tap fallback: GTK4 / firefox widgets require at least
    // one touch.motion between down and up to recognize a tap as a
    // click. If the user did a quick tap with no drag (motion_seen
    // false), synthesize a motion event before the up so the
    // gesture is recognized. Real drag/scroll already saw real
    // motions and skips this.
    // IMPORTANT: motion sent at SAME coords as down, not at +1/+1.
    // libadwaita's AdwStatusPage wraps its content in a
    // GtkScrolledWindow, whose kinetic-scroll gesture controller
    // runs on the CAPTURE phase -- it sees touches before the
    // deepest widget (the button inside the status page) does.
    // A motion with any position delta makes the scroll gesture
    // start treating the touch as a potential drag and claim it
    // away from the button's GtkGestureClick. Result: header-bar
    // buttons (outside the scrolled window) react fine, but the
    // empty-state "Discover new stations" button gets no click
    // because the scrolled-window already took the gesture.
    // Zero-delta motion still satisfies GTK4's "motion seen
    // between down and up" requirement for click recognition
    // without triggering the scroll-vs-tap heuristic.
    if (st->touch_motion_seen == 0)
    {
        st->touch_forward_fn(n, WIDGET_PROCESS_WINDOW_TOUCH_MOTION, sx, sy, st->touch_forward_userdata);
    }

    st->touch_forward_fn(n, WIDGET_PROCESS_WINDOW_TOUCH_UP, sx, sy, st->touch_forward_userdata);

    st->touch_active = 0;
    st->touch_motion_seen = 0;
}

static const widget_vtable_t g_process_window_vtable = {
    .type_name       = "process-window",
    .apply_attribute = process_window_apply_attribute,
    .layout          = process_window_layout,
    .emit_draws      = process_window_emit_draws,
    .on_destroy      = process_window_on_destroy,
    .on_mouse_down   = process_window_on_mouse_down,
    .on_mouse_drag   = process_window_on_mouse_drag,
    .on_mouse_up     = process_window_on_mouse_up,
    .captures_drag   = TRUE, // pin all drag events on this node, not propagated up
};

/**
 * @brief register the <process-window> widget type with meek-ui. Must be called once at startup, alongside the other widget_*__register entries. meek-ui's widget_registry__register_all (or the host's equivalent) fires this -- same place all other widget types get wired.
 *
 * @return void
 */
void widget_process_window__register(void)
{
    widget_registry__register(GUI_NODE_TYPE__PROCESS_WINDOW, &g_process_window_vtable);
}

// ============================================================================
// public API -- shell calls these to bind a node to a live texture.
// ============================================================================

/**
 * @brief attach (or replace) the live texture for this process-window node. `gl_texture` is a raw GL texture name (GLuint cast to uint32_t to avoid GL headers leaking into this .h). The texture MUST be bound in the same EGL context as meek-ui's renderer -- meek-shell imports buffers via the platform's EGL display/context exactly so this is true. The widget does NOT take ownership. Caller (meek-shell's toplevel_registry) must glDeleteTextures when the source process exits. Passing `gl_texture == 0` signals "no texture available"; the widget renders its bg rect only. Safe to call every frame (60 Hz commits from the source process end up here via meek_shell_v1_client.c's on_toplevel_buffer).
 *
 * @param gui_node_t* n -> target node; must have type GUI_NODE_TYPE__PROCESS_WINDOW.
 * @param uint32_t gl_texture -> GL texture name; 0 = unset.
 * @param int width -> source pixel width (used by layout for natural sizing).
 * @param int height -> source pixel height.
 *
 * @return void
 */
void widget_process_window__set_texture(gui_node_t* n, uint32_t gl_texture, int width, int height)
{
    if (n == NULL_POINTER) return;
    if (n->type != GUI_NODE_TYPE__PROCESS_WINDOW)
    {
        log_warn("%s %d %s", "widget_process_window__set_texture: node is not <process-window> (type=", (int)n->type, ")");
        return;
    }
    _widget_process_window_internal__state_t* st = _widget_process_window_internal__state_of(n);
    if (st == NULL_POINTER) return;
    st->gl_texture = gl_texture;
    st->width      = width;
    st->height     = height;
}

/**
 * @brief read back the meek_shell_v1 handle stored on this widget. Used by the shell's scene-update logic to find the right node for a given handle when an import arrives. Returns 0 if the node isn't a process-window or has no handle set.
 *
 * @return uint32_t -> void
 */
uint32_t widget_process_window__get_handle(gui_node_t* n)
{
    if (n == NULL_POINTER || n->type != GUI_NODE_TYPE__PROCESS_WINDOW) return 0;
    _widget_process_window_internal__state_t* st = (_widget_process_window_internal__state_t*)n->user_data;
    return st == NULL_POINTER ? 0 : st->handle;
}

/**
 * @brief get the source (foreign-app buffer) dimensions for this widget. The touch_forward callback receives sx/sy in this coordinate space (post-translation from widget panel coords). Callers that need to compare sy against a panel-pixel threshold must convert through this size, or use sy/height as a 0..1 fraction. Writes 0 to *out_w / *out_h if the node isn't a process-window or has no buffer yet.
 *
 * @return void
 */
void widget_process_window__get_buffer_size(gui_node_t* n, int32_t* out_w, int32_t* out_h)
{
    if (out_w != NULL_POINTER)
    {
        *out_w = 0;
    }
    if (out_h != NULL_POINTER)
    {
        *out_h = 0;
    }
    if (n == NULL_POINTER || n->type != GUI_NODE_TYPE__PROCESS_WINDOW)
    {
        return;
    }
    _widget_process_window_internal__state_t* st = (_widget_process_window_internal__state_t*)n->user_data;
    if (st == NULL_POINTER)
    {
        return;
    }
    if (out_w != NULL_POINTER)
    {
        *out_w = st->width;
    }
    if (out_h != NULL_POINTER)
    {
        *out_h = st->height;
    }
}

/**
 * @brief set the meek_shell_v1 handle this widget tracks. Same thing the `handle="N"` .ui attribute does, but available at runtime so the shell can spawn process-window nodes dynamically (one per toplevel_added event from the compositor).
 *
 * @return void
 */
void widget_process_window__set_handle(gui_node_t* n, uint32_t handle)
{
    if (n == NULL_POINTER || n->type != GUI_NODE_TYPE__PROCESS_WINDOW) return;
    _widget_process_window_internal__state_t* st = _widget_process_window_internal__state_of(n);
    if (st == NULL_POINTER) return;
    st->handle = handle;
}

/**
 * @brief replaces the overlay list; the widget copies up to WIDGET_PROCESS_WINDOW_MAX_OVERLAYS entries from arr into its internal slot table, and subsequent emits draw the primary texture first, then each overlay in arr-order on top
 *
 * @param gui_node_t* n -> process-window node
 * @param const struct widget_process_window_overlay* arr -> overlay entries to copy; pass NULL_POINTER to clear
 * @param int count -> number of entries in arr; pass 0 to clear
 *
 * @attention same layering note as set_texture: the widget does not take ownership of any GL texture, the caller manages lifecycle
 *
 * @return void
 */
void widget_process_window__set_overlays(gui_node_t* n, const struct widget_process_window_overlay* arr, int count)
{
    if (n == NULL_POINTER || n->type != GUI_NODE_TYPE__PROCESS_WINDOW) return;
    _widget_process_window_internal__state_t* st = _widget_process_window_internal__state_of(n);
    if (st == NULL_POINTER) return;

    if (count < 0)
    {
        count = 0;
    }
    if (count > WIDGET_PROCESS_WINDOW_MAX_OVERLAYS)
    {
        count = WIDGET_PROCESS_WINDOW_MAX_OVERLAYS;
    }
    st->overlay_count = count;
    if (count > 0 && arr != NULL_POINTER)
    {
        stdlib__memcpy(st->overlays, arr, sizeof(struct widget_process_window_overlay) * (size_t)count);
    }
}

/**
 * @brief install a touch-forwarder callback on this process-window node. Setting `fn = NULL` clears the forwarder (the node falls back to standard meek-ui behavior — taps fire on_click only). Typical use: on enter-fullscreen, the shell binds a forwarder that pipes touch events to the focused app via meek_shell_v1. On exit-fullscreen, clear it.
 *
 * @return void
 */
void widget_process_window__set_touch_forwarder(gui_node_t* n, widget_process_window_touch_fn fn, void* userdata)
{
    if (n == NULL_POINTER || n->type != GUI_NODE_TYPE__PROCESS_WINDOW) return;
    _widget_process_window_internal__state_t* st = _widget_process_window_internal__state_of(n);
    if (st == NULL_POINTER) return;
    st->touch_forward_fn       = fn;
    st->touch_forward_userdata = userdata;
    // Reset any in-progress sequence on forwarder change. If the
    // forwarder is being installed mid-tap (unlikely but possible
    // on enter-fullscreen during an active gesture), the next
    // mouse_down starts a clean sequence.
    st->touch_active           = 0;
    st->touch_motion_seen      = 0;
}
