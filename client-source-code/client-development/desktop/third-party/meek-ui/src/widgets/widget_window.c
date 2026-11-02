// widget_window.c - root container that fills the viewport.
// Window's bounds are the full viewport; no per-Window styling
// governs its size. It lays out children as a column inside its
// content rect (honoring its own pad / gap).
// SCROLLING (whole-page scrollbar)
// --------------------------------
// Window now honors the same overflow_x / overflow_y style as <div>
// via the shared scroll module. Set `overflow_y: auto` (or `scroll`)
// on Window and content taller than the viewport scrolls with a
// draggable scrollbar on the right edge -- the "browser-style" page
// scrollbar.
// Layout flow mirrors widget_div: speculative pass measures content,
// then a real pass with width narrowed by the scrollbar slot if a
// bar will be visible. scroll__clamp keeps scroll_y inside the legal
// range across viewport resizes and content changes.
// Default: overflow stays VISIBLE (no scrollbar) so existing scenes
// without an explicit overflow declaration behave exactly as before.

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "renderer.h"
#include "scroll.h"

// Same logic as widget_div's needs_clip: clip whenever a scrollbar is
// (or could be) involved, OR overflow is HIDDEN. Avoids pushing
// scissor when overflow is plain VISIBLE so the renderer's flush
// count stays low for the common case.
static boole _widget_window_internal__needs_clip(gui_node_t* n)
{
    gui_overflow_e ox = n->resolved.overflow_x;
    gui_overflow_e oy = n->resolved.overflow_y;
    if (ox == GUI_OVERFLOW__HIDDEN || ox == GUI_OVERFLOW__SCROLL)
    {
        return TRUE;
    }
    if (oy == GUI_OVERFLOW__HIDDEN || oy == GUI_OVERFLOW__SCROLL)
    {
        return TRUE;
    }
    if (ox == GUI_OVERFLOW__AUTO || oy == GUI_OVERFLOW__AUTO)
    {
        return (boole)(scroll__vbar_visible(n) || scroll__hbar_visible(n));
    }
    return FALSE;
}

// Scroll-aware column layout: the shared column flow with the page's
// scroll offsets, plus the content extent the page scrollbar needs.
static void _widget_window_internal__layout_children(gui_node_t* parent, float scale, float content_w)
{
    gui_style_t* s        = &parent->resolved;
    // Same border-aware shrink as widget_div uses; see comment there.
    float      bw       = scene__border_width(parent, scale);
    float      cx       = parent->bounds.x + s->pad_l * scale + bw;
    float      cy       = parent->bounds.y + s->pad_t * scale + bw;
    float      ch       = parent->bounds.h - (s->pad_t + s->pad_b) * scale - 2.0f * bw;
    float      cw       = content_w        - (s->pad_l + s->pad_r) * scale - 2.0f * bw;
    float      extent_w = 0.0f;
    float      extent_h = 0.0f;
    if (cw < 0.0f)
    {
        cw = 0.0f;
    }
    if (ch < 0.0f)
    {
        ch = 0.0f;
    }

    // The shared column flow, shifted by the page scroll offsets; the
    // extent it reports plus the trailing padding is what the page
    // scrollbar compares against the viewport.
    // the rubber-band offset (overscroll-behavior) moves the children
    // with the scroll; the reported extent is unaffected by either
    scene__layout_column_flow(parent, cx, cy, cw, ch, parent->scroll_x + parent->overscroll_x, parent->scroll_y + parent->overscroll_y, &extent_w, &extent_h);

    parent->content_w = extent_w + s->pad_r * scale;
    if (parent->content_w < cw)
    {
        parent->content_w = cw;
    }
    parent->content_h = extent_h;
    if (parent->content_h < 0.0f)
    {
        parent->content_h = 0.0f;
    }
    parent->content_h += s->pad_b * scale;
}

static void window_layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale)
{
    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = avail_w;
    n->bounds.h = avail_h;

    // Two-pass layout for the AUTO overflow case (mirrors widget_div).
    // Speculative pass measures content_h with full width; the real
    // pass narrows the content width when a scrollbar will be visible.
    _widget_window_internal__layout_children(n, scale, avail_w);
    boole need_v = scroll__vbar_visible(n);
    boole need_h = scroll__hbar_visible(n);
    if (need_v || need_h)
    {
        float cw = avail_w;
        if (need_v)
        {
            cw -= scroll__bar_size(n, scale);
            if (cw < 0.0f)
            {
                cw = 0.0f;
            }
        }
        _widget_window_internal__layout_children(n, scale, cw);
    }
    scroll__clamp(n);
}

// Drawing: bg first (under children), scrollbar last (over them).
// Scissor is pushed here and popped in emit_draws_post so children's
// draws -- and ONLY children's draws -- get clipped to the window.
static void window_emit_draws(gui_node_t* n, float scale)
{
    if (_widget_window_internal__needs_clip(n))
    {
        renderer__push_scissor(n->bounds);
    }

    // Shared bg helper: gradient + shadow + bg-image + blur + solid
    // bg + border. Previously this function only emitted solid bg +
    // border, so a <window> with a background-gradient painted
    // nothing. Same fix applied to widget_div.
    scene__emit_default_bg(n, scale);
}

static void window_emit_draws_post(gui_node_t* n, float scale)
{
    if (scroll__vbar_visible(n))
    {
        scroll__draw_vbar(n, scale);
    }
    if (scroll__hbar_visible(n))
    {
        scroll__draw_hbar(n, scale);
    }
    if (_widget_window_internal__needs_clip(n))
    {
        renderer__pop_scissor();
    }
}

static void window_on_mouse_down(gui_node_t* n, int64 x, int64 y, int64 button)
{
    (void)button;
    scroll__on_mouse_down(n, x, y, scene__scale());
}

static void window_on_mouse_up(gui_node_t* n, int64 x, int64 y, int64 button)
{
    (void)x;
    (void)y;
    (void)button;
    scroll__on_mouse_up(n);
}

static boole window_on_mouse_drag(gui_node_t* n, int64 x, int64 y)
{
    int64 coord = (n->scroll_drag_axis == 2) ? x : y;
    return scroll__on_mouse_drag(n, coord, scene__scale());
}

static const widget_vtable_t g_window_vtable = {
    .type_name        = "window",
    .init_defaults    = NULL_POINTER,
    .apply_attribute  = NULL_POINTER,
    .layout           = window_layout,
    .emit_draws       = window_emit_draws,
    .emit_draws_post  = window_emit_draws_post,
    .on_mouse_down    = window_on_mouse_down,
    .on_mouse_up      = window_on_mouse_up,
    .on_mouse_drag    = window_on_mouse_drag,
    .consumes_click   = FALSE,
};

void widget_window__register(void)
{
    widget_registry__register(GUI_NODE_TYPE__WINDOW, &g_window_vtable);
}
