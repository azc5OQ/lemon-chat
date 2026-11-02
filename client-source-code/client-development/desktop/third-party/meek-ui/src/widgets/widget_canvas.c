// widget_canvas.c - RGBA8 drawing surface.
// A fixed-size pixel buffer owned by the node, uploaded to the
// renderer as an RGBA texture. Host apps call the canvas__* API from
// their handlers to paint pixels / lines / rects / circles; the
// widget flags itself dirty, re-uploads the buffer on the next
// emit_draws, and submits it as an image.
// Typical paint app shape:
// <canvas id="sketch" width="512" height="384" />
// on_mouse_drag handler on the same node (via host code calling
// scene__register_handler) walks (prev_x, prev_y) -> (x, y) and
// calls canvas__stroke_line to draw with the current brush color.
// API surface (declared in canvas.h):
// canvas__clear(node, color)
// canvas__set_pixel(node, x, y, color)
// canvas__fill_rect(node, x, y, w, h, color)
// canvas__stroke_line(node, x0, y0, x1, y1, color)
// canvas__fill_circle(node, cx, cy, radius, color)
// GPU side: buffer is uploaded via renderer__create_texture_rgba on
// first use and renderer__destroy_texture on widget destroy. After
// every mutation we set dirty; emit_draws destroys + re-creates the
// texture so the new pixels ship to the GPU. That's not the fastest
// path (glTexSubImage2D would be cheaper) but keeps the renderer
// interface surface small; swap for a proper sub-upload later.

#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "renderer.h"
#include "canvas.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "third_party/log.h"
#include "clib/stdlib.h"

typedef struct _widget_canvas_internal__state_t
{
    // surface width in PIXELS (not logical -- true bitmap size).
    int     pixel_w;   // 0x0
    int     pixel_h;   // 0x4
    // RGBA8 buffer, length = pixel_w * pixel_h * 4.
    ubyte*  pixels;    // 0x8
    // renderer texture handle; NULL if never uploaded or just invalidated.
    void*   tex;       // 0x10
    // TRUE when pixels changed since last upload.
    boole   dirty;     // 0x18
} _widget_canvas_internal__state_t;

static _widget_canvas_internal__state_t* _widget_canvas_internal__state_of(gui_node_t* n)
{
    if (n == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    return (_widget_canvas_internal__state_t*)n->user_data;
}

static _widget_canvas_internal__state_t* _widget_canvas_internal__ensure(gui_node_t* n, int w, int h)
{
    _widget_canvas_internal__state_t* st = _widget_canvas_internal__state_of(n);
    if (st == NULL_POINTER)
    {
        st = (_widget_canvas_internal__state_t*)GUI_CALLOC_T(1, sizeof(_widget_canvas_internal__state_t), MM_TYPE__IMAGE);
        if (st == NULL_POINTER)
        {
            return NULL_POINTER;
        }
        n->user_data = st;
    }
    if (st->pixel_w == w && st->pixel_h == h && st->pixels != NULL_POINTER)
    {
        return st;
    }
    // Size change (or first allocation). Drop any existing texture +
    // buffer and allocate fresh. Initial fill is transparent black so
    // an un-drawn canvas renders as nothing rather than a solid color.
    if (st->tex != NULL_POINTER)
    {
        renderer__destroy_texture(st->tex);
        st->tex = NULL_POINTER;
    }
    if (st->pixels != NULL_POINTER)
    {
        GUI_FREE(st->pixels);
        st->pixels = NULL_POINTER;
    }
    if (w <= 0 || h <= 0)
    {
        st->pixel_w = 0;
        st->pixel_h = 0;
        return st;
    }
    size_t bytes = (size_t)w * (size_t)h * 4;
    st->pixels = (ubyte*)GUI_CALLOC_T(1, bytes, MM_TYPE__IMAGE);
    if (st->pixels == NULL_POINTER)
    {
        log_error("%s %d %s %d %s", "canvas: failed to allocate", w, "x", h, "buffer");
        return st;
    }
    st->pixel_w = w;
    st->pixel_h = h;
    st->dirty   = TRUE;
    return st;
}

// Bounds-safe pixel write. RGBA float components clamped 0..1 and
// converted to 8-bit on write.
static void _widget_canvas_internal__put(ubyte* px, int stride, int x, int y, int w, int h, gui_color_t c)
{
    if (x < 0 || y < 0 || x >= w || y >= h)
    {
        return;
    }
    float r = c.r < 0.0f ? 0.0f : (c.r > 1.0f ? 1.0f : c.r);
    float g = c.g < 0.0f ? 0.0f : (c.g > 1.0f ? 1.0f : c.g);
    float b = c.b < 0.0f ? 0.0f : (c.b > 1.0f ? 1.0f : c.b);
    float a = c.a < 0.0f ? 0.0f : (c.a > 1.0f ? 1.0f : c.a);
    // Standard RGBA byte addressing: stride is bytes-per-row
    // (pixel_w * 4), so y * stride steps full rows; x * 4 steps
    // pixels within the row (4 bytes each). NOT a double-multiply
    // -- the *4 lives in `stride` for the y axis and inline for the
    // x axis. If you find yourself thinking "this looks doubled",
    // remember stride is BYTES not pixels.
    ubyte* p = px + y * stride + x * 4;
    p[0] = (ubyte)(r * 255.0f + 0.5f);
    p[1] = (ubyte)(g * 255.0f + 0.5f);
    p[2] = (ubyte)(b * 255.0f + 0.5f);
    p[3] = (ubyte)(a * 255.0f + 0.5f);
}

// Public API. All take a gui_node_t*; looked up by id via scene__find_by_id
// by the host. We check the node type is actually CANVAS so accidental
// calls on the wrong kind of node log rather than corrupt memory.
static boole _widget_canvas_internal__check(gui_node_t* n)
{
    if (n == NULL_POINTER)
    {
        return FALSE;
    }
    if (n->type != GUI_NODE_TYPE__CANVAS)
    {
        log_warn("%s %s %s", "canvas: node id='", n->id, "' is not a <canvas>");
        return FALSE;
    }
    return TRUE;
}

/**
 * @brief clear the entire canvas to a solid color.
 *
 * @return void
 */
void canvas__clear(gui_node_t* n, gui_color_t c)
{
    if (_widget_canvas_internal__check(n) == FALSE)
    {
        return;
    }
    _widget_canvas_internal__state_t* st = _widget_canvas_internal__state_of(n);
    if (st == NULL_POINTER || st->pixels == NULL_POINTER)
    {
        return;
    }
    int stride = st->pixel_w * 4;
    for (int y = 0; y < st->pixel_h; y++)
    {
        for (int x = 0; x < st->pixel_w; x++)
        {
            _widget_canvas_internal__put(st->pixels, stride, x, y, st->pixel_w, st->pixel_h, c);
        }
    }
    st->dirty = TRUE;
}

/**
 * @brief write one pixel. Out-of-bounds coords are silently clipped.
 *
 * @return void
 */
void canvas__set_pixel(gui_node_t* n, int x, int y, gui_color_t c)
{
    if (_widget_canvas_internal__check(n) == FALSE)
    {
        return;
    }
    _widget_canvas_internal__state_t* st = _widget_canvas_internal__state_of(n);
    if (st == NULL_POINTER || st->pixels == NULL_POINTER)
    {
        return;
    }
    int stride = st->pixel_w * 4;
    _widget_canvas_internal__put(st->pixels, stride, x, y, st->pixel_w, st->pixel_h, c);
    st->dirty = TRUE;
}

/**
 * @brief filled axis-aligned rectangle.
 *
 * @return void
 */
void canvas__fill_rect(gui_node_t* n, int x, int y, int w, int h, gui_color_t c)
{
    if (_widget_canvas_internal__check(n) == FALSE)
    {
        return;
    }
    _widget_canvas_internal__state_t* st = _widget_canvas_internal__state_of(n);
    if (st == NULL_POINTER || st->pixels == NULL_POINTER)
    {
        return;
    }
    int stride = st->pixel_w * 4;
    for (int yy = y; yy < y + h; yy++)
    {
        for (int xx = x; xx < x + w; xx++)
        {
            _widget_canvas_internal__put(st->pixels, stride, xx, yy, st->pixel_w, st->pixel_h, c);
        }
    }
    st->dirty = TRUE;
}

/**
 * @brief one-pixel-wide Bresenham line.
 *
 * @return void
 */
void canvas__stroke_line(gui_node_t* n, int x0, int y0, int x1, int y1, gui_color_t c)
{
    // Bresenham. Plain one-pixel-wide line; thicker strokes can be
    // approximated by drawing a small filled disc at each step via
    // canvas__fill_circle.
    if (_widget_canvas_internal__check(n) == FALSE)
    {
        return;
    }
    _widget_canvas_internal__state_t* st = _widget_canvas_internal__state_of(n);
    if (st == NULL_POINTER || st->pixels == NULL_POINTER)
    {
        return;
    }
    int stride = st->pixel_w * 4;
    int dx  = (x1 - x0);  if (dx < 0) { dx = -dx; }
    int dy  = -((y1 - y0 < 0) ? -(y1 - y0) : (y1 - y0));
    int sx  = (x0 < x1) ? 1 : -1;
    int sy  = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    for (;;)
    {
        _widget_canvas_internal__put(st->pixels, stride, x0, y0, st->pixel_w, st->pixel_h, c);
        if (x0 == x1 && y0 == y1)
        {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
    st->dirty = TRUE;
}

/**
 * @brief filled disc.
 *
 * @return void
 */
void canvas__fill_circle(gui_node_t* n, int cx, int cy, int radius, gui_color_t c)
{
    if (_widget_canvas_internal__check(n) == FALSE)
    {
        return;
    }
    _widget_canvas_internal__state_t* st = _widget_canvas_internal__state_of(n);
    if (st == NULL_POINTER || st->pixels == NULL_POINTER)
    {
        return;
    }
    int stride = st->pixel_w * 4;
    int r2 = radius * radius;
    for (int dy = -radius; dy <= radius; dy++)
    {
        for (int dx = -radius; dx <= radius; dx++)
        {
            if (dx* dx + dy * dy <= r2)
            {
                _widget_canvas_internal__put(st->pixels, stride, cx + dx, cy + dy, st->pixel_w, st->pixel_h, c);
            }
        }
    }
    st->dirty = TRUE;
}

/**
 * @brief current canvas resolution in pixels. Either out may be NULL.
 *
 * @return void
 */
void canvas__size(gui_node_t* n, int* out_w, int* out_h)
{
    if (_widget_canvas_internal__check(n) == FALSE)
    {
        return;
    }
    _widget_canvas_internal__state_t* st = _widget_canvas_internal__state_of(n);
    if (out_w != NULL_POINTER)
    {
        *out_w = (st != NULL_POINTER) ? st->pixel_w : 0;
    }
    if (out_h != NULL_POINTER)
    {
        *out_h = (st != NULL_POINTER) ? st->pixel_h : 0;
    }
}

// Converts a screen pixel (from an on_mouse_* event) into canvas-local
// pixel coordinates. Useful when the canvas is scaled visually to fit
// its bounds but the backing buffer is a different size.
/**
 * @brief convert a mouse/touch event's screen coordinate into canvas-local pixel coords. Accounts for the canvas widget's bounds being scaled (either by the style `size` or the UI scale slider) relative to the backing buffer's pixel resolution.
 *
 * @return void
 */
void canvas__screen_to_pixel(gui_node_t* n, int64 sx, int64 sy, int* out_px, int* out_py)
{
    if (out_px == NULL_POINTER || out_py == NULL_POINTER)
    {
        return;
    }
    *out_px = 0;
    *out_py = 0;
    if (_widget_canvas_internal__check(n) == FALSE)
    {
        return;
    }
    _widget_canvas_internal__state_t* st = _widget_canvas_internal__state_of(n);
    if (st == NULL_POINTER || st->pixel_w <= 0 || st->pixel_h <= 0 || n->bounds.w <= 0.0f || n->bounds.h <= 0.0f)
    {
        return;
    }
    float tx = ((float)sx - n->bounds.x) / n->bounds.w;
    float ty = ((float)sy - n->bounds.y) / n->bounds.h;
    int px = (int)(tx * (float)st->pixel_w);
    int py = (int)(ty * (float)st->pixel_h);
    if (px < 0)
    {
        px = 0;
    }
    if (py < 0)
    {
        py = 0;
    }
    if (px >= st->pixel_w)
    {
        px = st->pixel_w - 1;
    }
    if (py >= st->pixel_h)
    {
        py = st->pixel_h - 1;
    }
    *out_px = px;
    *out_py = py;
}

// Widget vtable hooks.
static void canvas_init_defaults(gui_node_t* n)
{
    // Lazy-alloc: don't allocate any pixel buffer up front. The
    // public canvas__* draw functions check `st == NULL ||
    // st->pixels == NULL` and bail safely; apply_attribute below
    // (or the host's first canvas__set_pixel etc.) will trigger
    // the real allocation at the right size. Previously this
    // pre-allocated a 256x256 RGBA buffer (256 KB) for every
    // <canvas> node, then re-allocated to the user's actual size
    // when width=/height= attributes arrived -- pure waste.
    (void)n;
}

// Sane bounds on canvas dimensions. atoi("100000") would otherwise
// pass straight to ensure() which calls GUI_CALLOC for w*h*4 bytes
// (~40 GB at 100000x100000). Cap matches widget_image's decoded-
// dimension cap.
#define _WIDGET_CANVAS_INTERNAL__MAX_DIM 8192

static int _widget_canvas_internal__sanitize_dim(int v, char* attr_name, gui_node_t* n)
{
    if (v < 1)
    {
        log_warn("%s %s %s %d %s %s %s", "canvas:", attr_name, "='", v, "' on id='", n->id, "' is non-positive; clamping to 1");
        return 1;
    }
    if (v > _WIDGET_CANVAS_INTERNAL__MAX_DIM)
    {
        log_warn("%s %s %s %d %s %s %s %d %s", "canvas:", attr_name, "='", v, "' on id='", n->id, "' exceeds", _WIDGET_CANVAS_INTERNAL__MAX_DIM, "cap; clamping");
        return _WIDGET_CANVAS_INTERNAL__MAX_DIM;
    }
    return v;
}

static boole canvas_apply_attribute(gui_node_t* n, char* name, char* value)
{
    if (stdlib__strcmp(name, "width") == 0)
    {
        int w = _widget_canvas_internal__sanitize_dim(stdlib__atoi(value), "width", n);
        int h = 0;
        _widget_canvas_internal__state_t* st = _widget_canvas_internal__state_of(n);
        if (st != NULL_POINTER)
        {
            h = st->pixel_h;
        }
        if (h <= 0)
        {
            h = 256;
        }
        _widget_canvas_internal__ensure(n, w, h);
        return TRUE;
    }
    if (stdlib__strcmp(name, "height") == 0)
    {
        int h = _widget_canvas_internal__sanitize_dim(stdlib__atoi(value), "height", n);
        int w = 0;
        _widget_canvas_internal__state_t* st = _widget_canvas_internal__state_of(n);
        if (st != NULL_POINTER)
        {
            w = st->pixel_w;
        }
        if (w <= 0)
        {
            w = 256;
        }
        _widget_canvas_internal__ensure(n, w, h);
        return TRUE;
    }
    return FALSE;
}

static void canvas_layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale)
{
    gui_style_t* s = &n->resolved;
    _widget_canvas_internal__state_t* st = _widget_canvas_internal__state_of(n);
    float w;
    float h;
    if (s->size_w > 0.0f)
    {
        w = s->size_w * scale;
    }
    else if (st != NULL_POINTER && st->pixel_w > 0)
    {
        w = (float)st->pixel_w;
    }
    else
    {
        w = avail_w;
    }
    if (s->size_h > 0.0f)
    {
        h = s->size_h * scale;
    }
    else if (st != NULL_POINTER && st->pixel_h > 0)
    {
        h = (float)st->pixel_h;
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

static void canvas_emit_draws(gui_node_t* n, float scale)
{
    scene__emit_default_bg(n, scale);
    _widget_canvas_internal__state_t* st = _widget_canvas_internal__state_of(n);
    if (st == NULL_POINTER || st->pixels == NULL_POINTER)
    {
        return;
    }

    // Re-upload on dirty. We tear down and re-create the texture each
    // time for simplicity; the renderer interface doesn't expose a
    // sub-region update today. For a 512x512 canvas this is a few
    // hundred microseconds per frame while drawing is active -- fine
    // for PoC paint apps; swap for a sub-upload path later.
    if (st->dirty || st->tex == NULL_POINTER)
    {
        if (st->tex != NULL_POINTER)
        {
            renderer__destroy_texture(st->tex);
            st->tex = NULL_POINTER;
        }
        st->tex = renderer__create_texture_rgba(st->pixels, st->pixel_w, st->pixel_h);
        st->dirty = FALSE;
    }

    if (st->tex != NULL_POINTER)
    {
        gui_color_t tint;
        tint.r = 1.0f; tint.g = 1.0f; tint.b = 1.0f; tint.a = n->effective_opacity;
        renderer__submit_image(n->bounds, st->tex, tint);
    }
}

static void canvas_on_destroy(gui_node_t* n)
{
    // Read user_data directly instead of going through _state_of
    // (which lazy-allocates) -- nothing to clean up if the canvas
    // was never drawn on, and we'd otherwise alloc+free an empty
    // state struct in that path.
    _widget_canvas_internal__state_t* st = (_widget_canvas_internal__state_t*)n->user_data;
    if (st == NULL_POINTER)
    {
        return;
    }
    if (st->tex != NULL_POINTER)
    {
        renderer__destroy_texture(st->tex);
        st->tex = NULL_POINTER;
    }
    if (st->pixels != NULL_POINTER)
    {
        GUI_FREE(st->pixels);
        st->pixels = NULL_POINTER;
    }
    GUI_FREE(st);
    n->user_data = NULL_POINTER;
}

// Paint events. The canvas doesn't do any drawing itself in response
// to mouse input -- host code owns the brush and color. What we do
// provide is event dispatch: on_mouse_down fires an on_change with
// mouse.x/y and mouse.button=0 (press); on_mouse_drag fires with
// button=1 (continuing stroke). Handler reads ev->mouse.* and calls
// canvas__stroke_line (or similar).
// Why overload on_change instead of adding a new event type: keeps
// the gui_event_t ABI small and lets the existing scene__set_on_change
// attribute wiring work without a new "on_paint" attribute name.
static void canvas_on_mouse_down(gui_node_t* n, int64 x, int64 y, int64 button)
{
    if (n->on_change_hash == 0)
    {
        return;
    }
    gui_event_t ev;
    stdlib__memset(&ev, 0, sizeof(ev));
    ev.type         = GUI_EVENT_TYPE__CHANGE;
    ev.sender       = n;
    ev.mouse.x      = x;
    ev.mouse.y      = y;
    ev.mouse.button = 0; // sentinel: 0 = press (start stroke)
    (void)button;
    scene__dispatch_event(n, &ev);
}

static boole canvas_on_mouse_drag(gui_node_t* n, int64 x, int64 y)
{
    if (n->on_change_hash == 0)
    {
        return TRUE;
    }
    gui_event_t ev;
    stdlib__memset(&ev, 0, sizeof(ev));
    ev.type         = GUI_EVENT_TYPE__CHANGE;
    ev.sender       = n;
    ev.mouse.x      = x;
    ev.mouse.y      = y;
    ev.mouse.button = 1; // sentinel: 1 = drag (continue stroke)
    scene__dispatch_event(n, &ev);
    return TRUE;
}

static const widget_vtable_t g_canvas_vtable = {
    .type_name        = "canvas",
    .init_defaults    = canvas_init_defaults,
    .apply_attribute  = canvas_apply_attribute,
    .layout           = canvas_layout,
    .emit_draws       = canvas_emit_draws,
    .on_mouse_down    = canvas_on_mouse_down,
    .on_mouse_drag    = canvas_on_mouse_drag,
    .on_destroy       = canvas_on_destroy,
    .consumes_click   = TRUE,
    // Keep the Android touch state machine from stealing vertical
    // finger movement for scroll; we need the drags for paint
    // strokes.
    .captures_drag    = TRUE,
    // Hot-reload: preserve user_data (the pixel buffer + texture)
    // so the user doesn't lose their painting when the .ui file
    // changes. The .ui can't carry meaningful canvas state anyway
    // beyond the width/height attributes, which the parser sets
    // on the gui_node_t directly, not on user_data.
    .preserve_user_data = TRUE,
};

void widget_canvas__register(void)
{
    widget_registry__register(GUI_NODE_TYPE__CANVAS, &g_canvas_vtable);
}
