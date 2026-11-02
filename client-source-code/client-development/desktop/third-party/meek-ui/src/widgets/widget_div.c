// widget_div.c - generic styled block container with optional scrollbars.
// Like <Column> in that it lays out children vertically using the
// standard column placement helper, but with two differences:
// 1. <div> defaults to filling the available space in BOTH dimensions
// (width AND height) rather than collapsing to children-height.
// 2. <div> optionally shows scrollbars driven by the overflow_x /
// overflow_y style properties. Scrollable divs let the user drag
// the thumb to scroll content that exceeds the div's bounds, AND
// clip overflowing content via renderer__push_scissor.
// Use it as a styled panel / section / card / "page region" -- any
// time you want a sized block whose visuals (bg, radius, padding,
// scrollbars) you control, and whose children flow inside.
// Tag name is intentionally lowercase ("div") to match HTML convention.
// SCROLLING
// ---------
// All scrollbar mechanics (visibility decision, track + thumb rects,
// clamp, drag handling) live in scroll.c so widget_window.c can reuse
// the same code. This file just orchestrates: layout → emit_draws →
// scene recurses children → emit_draws_post.
// Overflow modes (gui_style_t.overflow_y):
// VISIBLE  no scrollbar, no clip, no scroll. default.
// HIDDEN   no scrollbar, but scissor still clips children to bounds.
// SCROLL   always show a scrollbar even if content fits; clip.
// AUTO     show a scrollbar only if content exceeds bounds; clip.
// Layout flow for a scrollable div:
// 1. Compute the div's own bounds from avail_w / avail_h / style.
// 2. Speculative pass: lay out children at the full width to
// measure content_h.
// 3. If a scrollbar will be shown (SCROLL mode, or AUTO with
// overflow), redo the layout with content_w narrowed by
// scrollbar_size so children don't paint into the bar slot.
// 4. scroll__clamp() snaps scroll_y back into the legal range.
// Draw flow:
// emit_draws       (called BEFORE scene recurses into children)
// push_scissor(div bounds) if clipping
// submit bg rect
// ...scene walks into children, who submit their own rects/text
// ...all clipped by the scissor pushed above...
// emit_draws_post  (called AFTER children)
// draw scrollbar (track + thumb)
// pop_scissor
// Drag interaction lives entirely in scroll.{h,c}; we just delegate
// the on_mouse_* hooks.


#include "types.h"
#include "clib/stdlib.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "compositor.h"
#include "renderer.h"
#include "scroll.h"
#include "flex_layout.h"

// Decide whether this div needs clipping at all. Clipping is on
// whenever the div's overflow_y is HIDDEN, SCROLL, or AUTO-with-
// overflowing-content (the same condition that matters for the bar).
// VISIBLE divs skip the scissor push to keep the renderer's flush
// count low for the common case.
static boole _widget_div_internal__needs_clip(gui_node_t* n)
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
        // AUTO mode only clips when there's actual overflow. We check
        // both axes so either direction of overflow triggers the clip.
        return (boole)(scroll__vbar_visible(n) || scroll__hbar_visible(n));
    }
    return FALSE;
}

// Scroll-aware column layout: the shared column flow with the div's
// scroll offsets, plus the content extent the scrollbars need.
static void _widget_div_internal__layout_children(gui_node_t* parent, float scale, float content_w, boole definite_h)
{
    gui_style_t* s        = &parent->resolved;
    // Border eats from BOTH sides of each axis -- inner area shrinks
    // by 2*bw and the child origin shifts by bw so children sit
    // inside the line, not under it. Same gating as scene__emit_border
    // via scene__border_width: bw is 0 for unbordered divs, so this
    // collapses to the original math in the common case.
    float      bw       = scene__border_width(parent, scale);
    float      cx       = parent->bounds.x + s->pad_l * scale + bw;
    float      cy       = parent->bounds.y + s->pad_t * scale + bw;
    // a hugging div offers its children no height to fill (see widget_column.c)
    float      ch       = definite_h ? parent->bounds.h - (s->pad_t + s->pad_b) * scale - 2.0f * bw : 0.0f;
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

    // The shared column flow places the children (alignment, margins,
    // gap, relative / sticky offsets, the absolute / fixed post-pass)
    // shifted by the scroll offsets, and reports the children's extent
    // as if unscrolled; content_w / content_h are that extent plus the
    // trailing padding, which the scrollbars compare against the bounds.
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

/**
 * @brief handles the div's own attributes: scroll-x / scroll-y set the initial scroll offset in logical pixels (scaled like every style length), which lets a scene start in a scrolled state (the visual tests use it to capture sticky children pinned)
 *
 * @param gui_node_t* n -> the div node
 * @param char* name -> attribute name
 * @param char* value -> attribute value
 *
 * @return boole -> TRUE when the attribute was consumed
 */
static boole div_apply_attribute(gui_node_t* n, char* name, char* value)
{
    // the target is set too: the animator snaps scroll_y to scroll_y_target
    // every frame when no smooth scrolling is configured, so a bare
    // scroll_y would be back at 0 before the first draw
    if (stdlib__strcmp(name, "scroll-y") == 0)
    {
        n->scroll_y        = (float)stdlib__atof(value) * scene__scale();
        n->scroll_y_target = n->scroll_y;
        return TRUE;
    }

    if (stdlib__strcmp(name, "scroll-x") == 0)
    {
        n->scroll_x        = (float)stdlib__atof(value) * scene__scale();
        n->scroll_x_target = n->scroll_x;
        return TRUE;
    }

    return FALSE;
}

static void div_layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale)
{
    if (n->resolved.display == GUI_DISPLAY__FLEX)
    {
        if (n->resolved.flex_direction == 2)
        {
            flex_layout__column(n, x, y, avail_w, avail_h, scale);
        }
        else
        {
            flex_layout__row(n, x, y, avail_w, avail_h, scale);
        }
        return;
    }
    // Width / height come from the layout helpers which already
    // honor `width: N%`, `width: Npx`, `size: WxH`, and parent-
    // available fallbacks. A parent asking for the max-content width
    // (avail_w < 0) gets the widest child, measured unconstrained,
    // plus padding and border, the CSS fit-content of a block.
    gui_style_t* s = &n->resolved;
    float w = 0.0f;
    float h = scene__layout_height(n, avail_h, scale);
    // the height is definite when the style sets it or the parent offers
    // definite space (which a div fills); a hugging div's is not
    boole definite_h = (boole)(s->size_h > 0.0f || avail_h > 0.0f);
    if (avail_w < 0.0f && s->size_w <= 0.0f && s->width_pct <= 0.0f)
    {
        float bw    = scene__border_width(n, scale);
        float max_w = 0.0f;
        for (gui_node_t* c = n->first_child; c != NULL_POINTER; c = c->next_sibling)
        {
            if (c->resolved.display == GUI_DISPLAY__NONE || c->resolved.position == GUI_POSITION__ABSOLUTE || c->resolved.position == GUI_POSITION__FIXED)
            {
                continue;
            }
            scene__measure_node(c, 0.0f, 0.0f, -1.0f, 0.0f);
            if (c->bounds.w + (c->resolved.margin_l + c->resolved.margin_r) * scale > max_w)
            {
                max_w = c->bounds.w + (c->resolved.margin_l + c->resolved.margin_r) * scale;
            }
        }
        w = max_w + (s->pad_l + s->pad_r) * scale + 2.0f * bw;
    }
    else
    {
        w = scene__layout_width(n, avail_w, scale);
    }
    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;

    // Two-pass layout for AUTO overflow:
    // pass 1 measures content_w and content_h with the full width;
    // pass 2 (only if either bar will be visible) re-lays out with
    // dimensions narrowed by the bar(s) they'd occupy. A
    // visible vbar eats from the right edge, an hbar eats
    // from the bottom. The layout helper doesn't care about
    // the height narrowing directly -- the scroll_y clamp at
    // the end handles that -- but the width narrowing matters
    // because children that auto-fit to cw must not bleed
    // under the vbar.
    _widget_div_internal__layout_children(n, scale, w, definite_h);

    // nothing offered and nothing set: the div hugs its content, as a
    // block does in CSS (a tile in a collection with auto rows, a div in
    // a hugging container). content_h is the children's extent from the
    // content origin plus the bottom padding, so the top padding and the
    // border close the box; the children are placed again with the real
    // inner height so alignment and percent heights see it.
    if (h <= 0.0f && s->size_h <= 0.0f)
    {
        h = n->content_h + s->pad_t * scale + 2.0f * scene__border_width(n, scale);
        scene__apply_min_max(n, NULL_POINTER, &h, scale);
        n->bounds.h = h;
        definite_h  = FALSE;
        _widget_div_internal__layout_children(n, scale, w, FALSE);
    }
    boole need_v = scroll__vbar_visible(n);
    boole need_h = scroll__hbar_visible(n);
    if (need_v || need_h)
    {
        float narrow_w = w;
        if (need_v)
        {
            narrow_w -= scroll__bar_size(n, scale);
            if (narrow_w < 0.0f)
            {
                narrow_w = 0.0f;
            }
        }
        _widget_div_internal__layout_children(n, scale, narrow_w, definite_h);
    }

    float previous_x = n->scroll_x;
    float previous_y = n->scroll_y;
    scroll__clamp(n);
    if (previous_x != n->scroll_x || previous_y != n->scroll_y)
    {
        // Content may have shrunk. Place children using the clamped offset in
        // this frame, including sticky/positioned children, not one frame later.
        float final_w = w;
        if (need_v)
        {
            final_w = stdlib__fmaxf(0.0f, w - scroll__bar_size(n, scale));
        }
        _widget_div_internal__layout_children(n, scale, final_w, definite_h);
    }
}

// emit_draws is called BEFORE scene recurses into children. Anything
// submitted here renders UNDER children. So this is the right spot
// for: scissor push (bracketing children's draws), and the bg rect.
static void div_emit_draws(gui_node_t* n, float scale)
{
    // Push the scissor first so the bg itself, while drawn at the
    // same rect as the scissor (so a no-op for the bg), STAYS pushed
    // until emit_draws_post pops it. Children then inherit the clip.
    if (_widget_div_internal__needs_clip(n))
    {
        renderer__push_scissor(n->bounds);
    }

    // Delegate bg painting to the shared helper so we pick up
    // gradient + shadow + bg-image + blur in addition to plain
    // background-color. Earlier this function only handled
    // has_background_color, which meant a <div> styled with ONLY a
    // background-gradient drew nothing -- the gradient test scene
    // caught it. scene__emit_default_bg also calls scene__emit_border
    // internally, so that's covered too.
    if (n->first_child == NULL_POINTER && scroll__vbar_visible(n) == FALSE && scroll__hbar_visible(n) == FALSE && compositor__reuse_decoration(n, scale) == TRUE)
    {
        return;
    }
    scene__emit_default_bg(n, scale);
}

// emit_draws_post runs AFTER scene has recursed through children.
// Submissions here paint over children -- ideal for scrollbars,
// focus rings, etc. Also where the scissor pushed in emit_draws
// gets popped, so subsequent siblings/uncles draw unclipped.
static void div_emit_draws_post(gui_node_t* n, float scale)
{
    if (scroll__vbar_visible(n))
    {
        scroll__draw_vbar(n, scale);
    }
    if (scroll__hbar_visible(n))
    {
        scroll__draw_hbar(n, scale);
    }
    if (_widget_div_internal__needs_clip(n))
    {
        renderer__pop_scissor();
    }
}

static void div_on_mouse_down(gui_node_t* n, int64 x, int64 y, int64 button)
{
    (void)button;
    scroll__on_mouse_down(n, x, y, scene__scale());
}

static void div_on_mouse_up(gui_node_t* n, int64 x, int64 y, int64 button)
{
    (void)x;
    (void)y;
    (void)button;
    scroll__on_mouse_up(n);
}

static boole div_on_mouse_drag(gui_node_t* n, int64 x, int64 y)
{
    // Route the coordinate the scroll helper expects based on which
    // axis is currently being dragged. scroll_drag_axis was set by
    // scroll__on_mouse_down; 1 = y-thumb drag, 2 = x-thumb drag.
    int64 coord = (n->scroll_drag_axis == 2) ? x : y;
    return scroll__on_mouse_drag(n, coord, scene__scale());
}

static const widget_vtable_t g_div_vtable = {
    .type_name        = "div",
    .init_defaults    = NULL_POINTER,
    .apply_attribute  = div_apply_attribute,
    .layout           = div_layout,
    .emit_draws       = div_emit_draws,
    .emit_draws_post  = div_emit_draws_post,
    .on_mouse_down    = div_on_mouse_down,
    .on_mouse_up      = div_on_mouse_up,
    .on_mouse_drag    = div_on_mouse_drag,
    .consumes_click   = FALSE,
};

void widget_div__register(void)
{
    widget_registry__register(GUI_NODE_TYPE__DIV, &g_div_vtable);
}
