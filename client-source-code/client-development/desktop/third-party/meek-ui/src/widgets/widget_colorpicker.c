// widget_colorpicker.c - <colorpicker type="horizontal|classic"/>
// Two modes:
// horizontal  hue strip only. Draws a 6-color hue gradient (red
// -> yellow -> green -> cyan -> blue -> magenta -> red)
// as 6 contiguous gradient rects. A thin vertical
// cursor marks the current hue. Drag to select.
// classic     saturation/value square on top, thin hue strip
// underneath. SV square colors are driven from the
// current hue; drag inside to pick (sat, value). A
// separate drag region on the hue strip sets the
// hue. Matches the GIMP / Photoshop picker shape.
// Selection is stored on the node:
// n->value        = hue in 0..1
// value_min       = saturation in 0..1 (reuses the unused slider slot)
// value_max       = value in 0..1
// user_data       = internal state (drag mode, latest rgba)
// on_change fires whenever any component moves. Handler reads the
// final RGBA from ev->change.color.value.

#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "renderer.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "third_party/log.h"
#include "clib/stdlib.h"

// The hue strip of the classic picker is 14 logical px tall with a 4 px
// gap above it unless the style's `gap` says otherwise; a horizontal
// picker with no height is one 24 px strip. All of it scales with the
// UI, and size / width / height / min / max from the style win.
#define _COLORPICKER_INTERNAL__STRIP_H         14.0f
#define _COLORPICKER_INTERNAL__GAP             4.0f
#define _COLORPICKER_INTERNAL__DEFAULT_STRIP_H 24.0f

typedef enum _colorpicker_internal__mode_e
{
    _COLORPICKER_INTERNAL__HORIZONTAL = 0,
    _COLORPICKER_INTERNAL__CLASSIC    = 1,
} _colorpicker_internal__mode_e;

typedef enum _colorpicker_internal__drag_e
{
    _COLORPICKER_INTERNAL__DRAG_NONE = 0,
    _COLORPICKER_INTERNAL__DRAG_HUE  = 1,
    _COLORPICKER_INTERNAL__DRAG_SV   = 2,
} _colorpicker_internal__drag_e;

typedef struct _colorpicker_internal__state_t
{
    _colorpicker_internal__mode_e mode;   // 0x0
    _colorpicker_internal__drag_e drag;   // 0x4
} _colorpicker_internal__state_t;

static _colorpicker_internal__state_t* _colorpicker_internal__state_of(gui_node_t* n)
{
    if (n->user_data == NULL_POINTER)
    {
        n->user_data = GUI_CALLOC_T(1, sizeof(_colorpicker_internal__state_t), MM_TYPE__GENERIC);
    }
    return (_colorpicker_internal__state_t*)n->user_data;
}

// HSV -> RGB at alpha 1.0. Standard formula. Input h/s/v all in 0..1.
static gui_color_t _colorpicker_internal__hsv_to_rgb(float h, float s, float v)
{
    gui_color_t c;
    c.a = 1.0f;
    if (s <= 0.0f)
    {
        c.r = v; c.g = v; c.b = v;
        return c;
    }
    float hh = h * 6.0f;
    if (hh >= 6.0f)
    {
        hh = 0.0f;
    }
    int   i = (int)hh;
    float f = hh - (float)i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * f);
    float t = v * (1.0f - s * (1.0f - f));
    switch (i)
    {
        case 0: c.r = v; c.g = t; c.b = p; break;
        case 1: c.r = q; c.g = v; c.b = p; break;
        case 2: c.r = p; c.g = v; c.b = t; break;
        case 3: c.r = p; c.g = q; c.b = v; break;
        case 4: c.r = t; c.g = p; c.b = v; break;
        default:c.r = v; c.g = p; c.b = q; break;
    }
    return c;
}

static void _colorpicker_internal__fire_change(gui_node_t* n)
{
    if (n->on_change_hash == 0)
    {
        return;
    }
    float h = n->value;
    float s = n->value_min;
    float v = n->value_max;
    gui_color_t rgba = _colorpicker_internal__hsv_to_rgb(h, s, v);
    gui_event_t ev;
    stdlib__memset(&ev, 0, sizeof(ev));
    ev.type        = GUI_EVENT_TYPE__CHANGE;
    ev.sender      = n;
    ev.color.value = rgba;
    scene__dispatch_event(n, &ev);
}

static boole colorpicker_apply_attribute(gui_node_t* n, char* name, char* value)
{
    if (stdlib__strcmp(name, "type") == 0)
    {
        _colorpicker_internal__state_t* st = _colorpicker_internal__state_of(n);
        if      (stdlib__strcmp(value, "horizontal") == 0)
        {
            st->mode = _COLORPICKER_INTERNAL__HORIZONTAL;
        }
        else if (stdlib__strcmp(value, "classic")    == 0)
        {
            st->mode = _COLORPICKER_INTERNAL__CLASSIC;
        }
        else
        {
            log_warn("%s %s %s", "colorpicker: unknown type '", value, "' (horizontal|classic)");
        }
        return TRUE;
    }
    return FALSE;
}

static void colorpicker_init_defaults(gui_node_t* n)
{
    // Sensible defaults: fully saturated red at full value. Callers
    // can pre-seed with value="0.25" (hue), or drive via code before
    // the first draw if they need a different starting color.
    n->value     = 0.0f;
    n->value_min = 1.0f;
    n->value_max = 1.0f;
    _colorpicker_internal__state_of(n); // allocate state slot
}

static void colorpicker_layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale)
{
    gui_style_t*                    s   = NULL_POINTER;
    _colorpicker_internal__state_t* st  = NULL_POINTER;
    float                           w   = 0.0f;
    float                           h   = 0.0f;
    float                           gap = 0.0f;

    s   = &n->resolved;
    st  = (_colorpicker_internal__state_t*)n->user_data;
    w   = scene__layout_width(n, avail_w, scale);
    gap = (s->gap > 0.0f ? s->gap : _COLORPICKER_INTERNAL__GAP) * scale;

    // height: the style's, else a percentage of the space offered, else
    // the classic picker's square plus its hue strip, else one strip
    if (s->size_h > 0.0f)
    {
        h = s->size_h * scale;
    }
    else if (s->height_pct > 0.0f && avail_h > 0.0f)
    {
        h = avail_h * s->height_pct / 100.0f;
    }
    else if (st != NULL_POINTER && st->mode == _COLORPICKER_INTERNAL__CLASSIC)
    {
        h = w + _COLORPICKER_INTERNAL__STRIP_H * scale + gap;
    }
    else
    {
        h = _COLORPICKER_INTERNAL__DEFAULT_STRIP_H * scale;
    }

    scene__apply_min_max(n, &w, &h, scale);

    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;
}

// Hue strip: 6 contiguous gradient rects laid side by side covering
// red->yellow->green->cyan->blue->magenta->red. Emit_draws submits
// them via renderer__submit_rect_gradient (horizontal direction).
static void _colorpicker_internal__draw_hue_strip(gui_rect_t bounds, float radius, float opacity)
{
    static const gui_color_t stops[7] =
    {
        {1.0f, 0.0f, 0.0f, 1.0f}, // red
        {1.0f, 1.0f, 0.0f, 1.0f}, // yellow
        {0.0f, 1.0f, 0.0f, 1.0f}, // green
        {0.0f, 1.0f, 1.0f, 1.0f}, // cyan
        {0.0f, 0.0f, 1.0f, 1.0f}, // blue
        {1.0f, 0.0f, 1.0f, 1.0f}, // magenta
        {1.0f, 0.0f, 0.0f, 1.0f}, // red (loop close)
    };
    float seg = bounds.w / 6.0f;

    // Segments 0 and 5 carry a uniform-all-corners radius (the SDF
    // shader has no way to round only the outer two corners). That
    // gives each of them an INNER rounded cutout -- top-right +
    // bottom-right on seg 0, top-left + bottom-left on seg 5 --
    // inside their own bounds, in the `radius`-wide strip against
    // the neighboring segment. Segments 1 and 4 normally start /
    // end flush with the seg boundary, so they don't cover those
    // cutouts and the bg shows through as dark notches.
    // Fix: extend segments 1 and 4 by `radius` px toward the end
    // caps so they overdraw the rounded cutouts with flat gradient
    // colour. Segment 1 starts with yellow (= seg 0's end colour),
    // so the extended piece matches the colour seg 0 would have
    // been there to within a single gradient-position step -- the
    // human eye sees no seam.
    for (int i = 0; i < 6; i++)
    {
        float x_start = bounds.x + seg * (float)i;
        float width   = seg + 0.5f;
        float r_here  = 0.0f;
        if (i == 0 || i == 5)
        {
            r_here = radius;
        }
        else if (i == 1)
        {
            x_start -= radius;
            width   += radius;
        }
        else if (i == 4)
        {
            width   += radius;
        }

        gui_rect_t r;
        gui_color_t from = stops[i];
        gui_color_t to = stops[i + 1];
        r.x = x_start;
        r.y = bounds.y;
        r.w = width;
        r.h = bounds.h;
        // the strip follows the node's opacity, so an :appear / :disappear
        // fade or an opacity cascade reaches it like any other widget
        from.a *= opacity;
        to.a *= opacity;
        renderer__submit_rect_gradient(r, from, to, (int)GUI_GRADIENT_DIR__HORIZONTAL, r_here);
    }
}

static void colorpicker_emit_draws(gui_node_t* n, float scale)
{
    gui_rect_t swatch;
    gui_color_t picked;

    stdlib__null_memory(&swatch, sizeof(swatch));
    stdlib__null_memory(&picked, sizeof(picked));
    (void)scale;
    scene__emit_default_bg(n, scale);
    _colorpicker_internal__state_t* st = (_colorpicker_internal__state_t*)n->user_data;
    if (st == NULL_POINTER)
    {
        return;
    }
    // color-swatch: the current colour as a square to the right of the
    // strip (the pure hue of a horizontal picker, the picked HSV of a
    // classic one)
    if (n->resolved.swatch_size > 0.0f)
    {
        picked = (st->mode == _COLORPICKER_INTERNAL__HORIZONTAL) ? _colorpicker_internal__hsv_to_rgb(n->value, 1.0f, 1.0f) : _colorpicker_internal__hsv_to_rgb(n->value, n->value_min, n->value_max);
        picked.a *= n->effective_opacity;
        swatch.w = n->resolved.swatch_size * scale;
        swatch.h = swatch.w;
        swatch.x = n->bounds.x + n->bounds.w + ((n->resolved.swatch_gap > 0.0f) ? n->resolved.swatch_gap : 12.0f) * scale;
        swatch.y = n->bounds.y + (n->bounds.h - swatch.h) * 0.5f;
        renderer__submit_rect(swatch, picked, n->resolved.swatch_radius * scale);
    }

    gui_color_t white = {1.0f, 1.0f, 1.0f, 1.0f};
    gui_color_t black = {0.0f, 0.0f, 0.0f, 1.0f};
    float op = n->effective_opacity;
    // Build-scale alpha on the cursor marker so it fades with the
    // parent opacity cascade.
    gui_color_t cursor_c = white;
    cursor_c.a *= op;

    if (st->mode == _COLORPICKER_INTERNAL__HORIZONTAL)
    {
        gui_rect_t hue = n->bounds;
        float radius = n->resolved.radius * scale;
        _colorpicker_internal__draw_hue_strip(hue, radius, n->effective_opacity);
        // Cursor: a 2px-wide vertical bar at `value * width`.
        float cx = hue.x + hue.w * n->value;
        gui_rect_t cursor;
        cursor.x = cx - 1.0f;
        cursor.y = hue.y - 2.0f;
        cursor.w = 2.0f;
        cursor.h = hue.h + 4.0f;
        renderer__submit_rect(cursor, cursor_c, 1.0f);
    }
    else
    {
        // Classic: SV square on top, hue strip underneath.
        float strip_h = _COLORPICKER_INTERNAL__STRIP_H * scale;
        float gap     = (n->resolved.gap > 0.0f ? n->resolved.gap : _COLORPICKER_INTERNAL__GAP) * scale;
        gui_rect_t sv;
        sv.x = n->bounds.x;
        sv.y = n->bounds.y;
        sv.w = n->bounds.w;
        sv.h = n->bounds.h - strip_h - gap;
        if (sv.h < 0.0f)
        {
            sv.h = 0.0f;
        }
        // SV square: horizontal lerp from white -> pure-hue, then
        // vertical lerp from that -> black. We can't express a
        // two-axis gradient with a single rect, so we draw two
        // superimposed gradients:
        // (1) horizontal from white -> hue, covering the whole square
        // (2) vertical   from transparent black -> opaque black, on top
        // The final pixel is hue lerped by x, then darkened by y.
        gui_color_t hue_c = _colorpicker_internal__hsv_to_rgb(n->value, 1.0f, 1.0f);
        hue_c.a = 1.0f * op;
        gui_color_t white_op = white; white_op.a *= op;
        renderer__submit_rect_gradient(sv, white_op, hue_c, (int)GUI_GRADIENT_DIR__HORIZONTAL, n->resolved.radius * scale);
        gui_color_t black_transparent = {0.0f, 0.0f, 0.0f, 0.0f};
        gui_color_t black_opaque      = {0.0f, 0.0f, 0.0f, 1.0f * op};
        renderer__submit_rect_gradient(sv, black_transparent, black_opaque, (int)GUI_GRADIENT_DIR__VERTICAL, n->resolved.radius * scale);

        // SV cursor: small cross-hair at (value_min, 1 - value_max).
        float cx = sv.x + sv.w * n->value_min;
        float cy = sv.y + sv.h * (1.0f - n->value_max);
        gui_rect_t cross_h; cross_h.x = cx - 5.0f; cross_h.y = cy - 1.0f; cross_h.w = 10.0f; cross_h.h = 2.0f;
        gui_rect_t cross_v; cross_v.x = cx - 1.0f; cross_v.y = cy - 5.0f; cross_v.w = 2.0f;  cross_v.h = 10.0f;
        gui_color_t shadow_black = black; shadow_black.a = 0.6f * op;
        renderer__submit_rect(cross_h, shadow_black, 1.0f);
        renderer__submit_rect(cross_v, shadow_black, 1.0f);
        renderer__submit_rect(cross_h, cursor_c, 1.0f);
        renderer__submit_rect(cross_v, cursor_c, 1.0f);

        // Hue strip below the square.
        gui_rect_t hue;
        hue.x = n->bounds.x;
        hue.y = sv.y + sv.h + gap;
        hue.w = n->bounds.w;
        hue.h = strip_h;
        _colorpicker_internal__draw_hue_strip(hue, n->resolved.radius * scale, n->effective_opacity);
        float hx = hue.x + hue.w * n->value;
        gui_rect_t hue_cursor;
        hue_cursor.x = hx - 1.0f; hue_cursor.y = hue.y - 2.0f;
        hue_cursor.w = 2.0f;      hue_cursor.h = hue.h + 4.0f;
        renderer__submit_rect(hue_cursor, cursor_c, 1.0f);
    }
}

// Mouse handling. Classic mode: top area = SV drag, bottom strip =
// hue drag. Horizontal mode: everything = hue drag.
static void _colorpicker_internal__apply_drag(gui_node_t* n, int64 mx, int64 my)
{
    _colorpicker_internal__state_t* st = (_colorpicker_internal__state_t*)n->user_data;
    if (st == NULL_POINTER)
    {
        return;
    }
    float x = (float)mx;
    float y = (float)my;

    if (st->drag == _COLORPICKER_INTERNAL__DRAG_HUE)
    {
        gui_rect_t strip;
        if (st->mode == _COLORPICKER_INTERNAL__HORIZONTAL)
        {
            strip = n->bounds;
        }
        else
        {
            float strip_h = _COLORPICKER_INTERNAL__STRIP_H * scene__scale();
            strip.x = n->bounds.x;
            strip.y = n->bounds.y + n->bounds.h - strip_h;
            strip.w = n->bounds.w;
            strip.h = strip_h;
        }
        float t = (x - strip.x) / strip.w;
        if (t < 0.0f)
        {
            t = 0.0f;
        }
        if (t > 1.0f)
        {
            t = 1.0f;
        }
        n->value = t;
    }
    else if (st->drag == _COLORPICKER_INTERNAL__DRAG_SV)
    {
        float strip_h = _COLORPICKER_INTERNAL__STRIP_H * scene__scale();
        float gap     = (n->resolved.gap > 0.0f ? n->resolved.gap : _COLORPICKER_INTERNAL__GAP) * scene__scale();
        gui_rect_t sv;
        sv.x = n->bounds.x;
        sv.y = n->bounds.y;
        sv.w = n->bounds.w;
        sv.h = n->bounds.h - strip_h - gap;
        if (sv.h < 1.0f)
        {
            sv.h = 1.0f;
        }
        float sx = (x - sv.x) / sv.w;
        float sy = (y - sv.y) / sv.h;
        if (sx < 0.0f)
        {
            sx = 0.0f;
        }
        if (sx > 1.0f)
        {
            sx = 1.0f;
        }
        if (sy < 0.0f)
        {
            sy = 0.0f;
        }
        if (sy > 1.0f)
        {
            sy = 1.0f;
        }
        n->value_min = sx;
        n->value_max = 1.0f - sy;
    }

    _colorpicker_internal__fire_change(n);
}

static void colorpicker_on_mouse_down(gui_node_t* n, int64 x, int64 y, int64 button)
{
    (void)button;
    _colorpicker_internal__state_t* st = _colorpicker_internal__state_of(n);
    // In classic mode, figure out which region the press landed in.
    // In horizontal mode it's always the hue strip.
    if (st->mode == _COLORPICKER_INTERNAL__CLASSIC)
    {
        float strip_h = 14.0f;
        float strip_y = n->bounds.y + n->bounds.h - strip_h;
        st->drag = ((float)y >= strip_y) ? _COLORPICKER_INTERNAL__DRAG_HUE : _COLORPICKER_INTERNAL__DRAG_SV;
    }
    else
    {
        st->drag = _COLORPICKER_INTERNAL__DRAG_HUE;
    }
    _colorpicker_internal__apply_drag(n, x, y);
}

static boole colorpicker_on_mouse_drag(gui_node_t* n, int64 x, int64 y)
{
    _colorpicker_internal__state_t* st = (_colorpicker_internal__state_t*)n->user_data;
    if (st == NULL_POINTER || st->drag == _COLORPICKER_INTERNAL__DRAG_NONE)
    {
        return FALSE;
    }
    _colorpicker_internal__apply_drag(n, x, y);
    return TRUE;
}

static void colorpicker_on_mouse_up(gui_node_t* n, int64 x, int64 y, int64 button)
{
    (void)x; (void)y; (void)button;
    _colorpicker_internal__state_t* st = (_colorpicker_internal__state_t*)n->user_data;
    if (st == NULL_POINTER)
    {
        return;
    }
    st->drag = _COLORPICKER_INTERNAL__DRAG_NONE;
}

static void colorpicker_on_destroy(gui_node_t* n)
{
    if (n->user_data != NULL_POINTER)
    {
        GUI_FREE(n->user_data);
        n->user_data = NULL_POINTER;
    }
}

static const widget_vtable_t g_colorpicker_vtable = {
    .type_name       = "colorpicker",
    .init_defaults   = colorpicker_init_defaults,
    .apply_attribute = colorpicker_apply_attribute,
    .layout          = colorpicker_layout,
    .emit_draws      = colorpicker_emit_draws,
    .on_mouse_down   = colorpicker_on_mouse_down,
    .on_mouse_drag   = colorpicker_on_mouse_drag,
    .on_mouse_up     = colorpicker_on_mouse_up,
    .on_destroy      = colorpicker_on_destroy,
    .consumes_click  = TRUE,
    // Classic picker's SV square needs vertical drags (value axis);
    // we can't let Android's touch state machine hijack them for
    // scroll once the finger has moved >16 px.
    .captures_drag   = TRUE,
    // Hot-reload preserves the picked color (HSV stored on
    // user_data) so a .style edit doesn't reset the user's
    // current selection.
    .preserve_user_data = TRUE,
};

void widget_colorpicker__register(void)
{
    widget_registry__register(GUI_NODE_TYPE__COLOR_PICKER, &g_colorpicker_vtable);
}
