// widget_radio.c - radio button (one-of-many selector).
// <radio class="size" id="sz-small"  on_change="on_size" />
// <radio class="size" id="sz-medium" on_change="on_size" value="1" />
// <radio class="size" id="sz-large"  on_change="on_size" />
// VISUAL:
// - Outer CIRCLE (high-radius rounded square, radius = min(w,h)/2).
// - When selected (n->value != 0) draws an inner filled circle
// centered inside with ~40% of the outer's diameter.
// GROUPING (the one-of-N semantics):
// - Radios with the SAME CLASS form a group. Selecting one
// deselects every other radio in the same class -- matches HTML's
// radios-with-same-name-attribute semantics but repurposes the
// class attribute since we already have that.
// - Group enforcement walks the scene tree from root on every click
// to find siblings. O(N_radios) per click which is fine for the
// sizes of radio groups real apps have (handful).
// - Radios with NO class are treated as independent toggles (their
// own one-button group). Rarely useful but well-defined.
// INPUT:
// - On mouse_up INSIDE the widget: walk the tree, set this radio's
// value = 1 and every other same-class radio's value = 0. Then
// dispatch on_change on THIS radio with the new value.
// - Clicking an already-selected radio is a no-op (consistent with
// HTML + native behavior).
// STYLING HOOKS:
// bg / has_bg   -- outer ring bg.
// fg / has_fg   -- inner filled dot when selected.
// :hover / :pressed / :appear -- same semantics as any other widget.

#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "renderer.h"
#include "font.h"

// space between the circle and its caption, in logical pixels
#define _WIDGET_RADIO_INTERNAL__TEXT_GAP 8.0f
#include "clib/stdlib.h"

static boole radio_apply_attribute(gui_node_t* n, char* name, char* value)
{
    // `value=` (legacy slot) and `checked=` (CSS-muscle-memory
    // alias) both mean "start selected if truthy". Note: if the
    // author passes a non-boolean `value=` (e.g. value="dark" as
    // an option identifier), we'll dutifully set n->value = 0 --
    // that matches the HTML idiom where `value="dark"` alone does
    // NOT select a radio; the selection requires `checked`.
    if (stdlib__strcmp(name, "value") == 0 || stdlib__strcmp(name, "checked") == 0)
    {
        if (stdlib__strcmp(value, "1") == 0 || stdlib__strcmp(value, "true") == 0 || stdlib__strcmp(value, "selected") == 0 || stdlib__strcmp(value, "checked") == 0)
        {
            n->value = 1.0f;
        }
        else
        {
            n->value = 0.0f;
        }
        return TRUE;
    }
    // `group=` is sugar: radios with the same group act as mutually
    // exclusive siblings. We implement grouping via class (see the
    // file-header comment), so `group="theme"` just becomes
    // `class="theme"`. Doesn't overwrite an existing class if one
    // was already set -- last-write-wins per file order matches
    // the parser's general attr semantics.
    if (stdlib__strcmp(name, "group") == 0)
    {
        size_t m = stdlib__strlen(value);
        if (m >= sizeof(n->klass))
        {
            m = sizeof(n->klass) - 1;
        }
        stdlib__memcpy(n->klass, value, m);
        n->klass[m] = 0;
        return TRUE;
    }
    return FALSE;
}

static void radio_layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale)
{
    (void)avail_w;
    (void)avail_h;
    gui_style_t* s = &n->resolved;
    float w = (s->size_w > 0.0f) ? s->size_w * scale : 20.0f * scale;
    float h = (s->size_h > 0.0f) ? s->size_h * scale : 20.0f * scale;
    // the text sits right of the circle and stretches the bounds, as
    // the caption of an HTML radio button does
    if (n->text[0] != 0)
    {
        float     size_px = (s->font_size > 0.0f ? s->font_size : 14.0f) * scale;
        gui_font_t* f       = font__at(s->font_family, size_px);
        if (f != NULL_POINTER)
        {
            float line_h = font__line_height_for(f, s, scale);
            w = w + _WIDGET_RADIO_INTERNAL__TEXT_GAP * scale + font__measure(f, n->text);
            if (line_h > h)
            {
                h = line_h;
            }
        }
    }
    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;
}

static void radio_emit_draws(gui_node_t* n, float scale)
{
    gui_style_t* s = &n->resolved;

    // The circle: the style size or 20 px, vertically centered in the
    // bounds (which are taller when the text's line is).
    gui_rect_t box;
    box.w = (s->size_w > 0.0f) ? s->size_w * scale : 20.0f * scale;
    box.h = (s->size_h > 0.0f) ? s->size_h * scale : 20.0f * scale;
    box.x = n->bounds.x;
    box.y = n->bounds.y + (n->bounds.h - box.h) * 0.5f;

    // Outer circle = rounded square with radius = half the shorter
    // side. The renderer's SDF path clamps radius to that max anyway,
    // so this just makes the intent explicit.
    float outer_r = (box.w < box.h ? box.w : box.h) * 0.5f;
    gui_color_t outer = s->has_background_color ? s->background_color : scene__rgb(0.22f, 0.23f, 0.28f);
    renderer__submit_rect(box, outer, outer_r);

    // Inner dot when selected. ~40% of the outer diameter, centered.
    if (n->value != 0.0f)
    {
        float inner_side = box.w * 0.40f;
        if (inner_side > box.h * 0.40f)
        {
            inner_side = box.h * 0.40f;
        }

        gui_rect_t inner;
        inner.x = box.x + (box.w - inner_side) * 0.5f;
        inner.y = box.y + (box.h - inner_side) * 0.5f;
        inner.w = inner_side;
        inner.h = inner_side;

        gui_color_t fill = s->has_accent_color ? s->accent_color : scene__rgb(0.39f, 0.40f, 0.95f);
        renderer__submit_rect(inner, fill, inner_side * 0.5f);
    }

    // Caption to the right of the circle, centered on the bounds' height.
    if (n->text[0] != 0)
    {
        float     size_px = (s->font_size > 0.0f ? s->font_size : 14.0f) * scale;
        gui_font_t* f       = font__at(s->font_family, size_px);
        if (f != NULL_POINTER)
        {
            float     line_h = font__line_height_for(f, s, scale);
            float     tx     = box.x + box.w + _WIDGET_RADIO_INTERNAL__TEXT_GAP * scale;
            float     ty     = n->bounds.y + (n->bounds.h - line_h) * 0.5f + font__baseline_for(f, s, scale);
            gui_color_t color  = s->has_font_color ? s->font_color : scene__rgb(1.0f, 1.0f, 1.0f);
            color.a *= n->effective_opacity;
            font__draw(f, tx, ty, color, n->text);
        }
    }
}

// Recursive helper: walk subtree and deselect every radio whose class
// matches `group`. Skips `self` so we don't immediately unselect the
// button the user clicked.
static void _widget_radio_internal__deselect_group(gui_node_t* n, char* group, gui_node_t* self)
{
    if (n == NULL_POINTER)
    {
        return;
    }
    if (n != self && n->type == GUI_NODE_TYPE__RADIO)
    {
        if (stdlib__strcmp(n->klass, group) == 0)
        {
            n->value = 0.0f;
        }
    }
    gui_node_t* c = n->first_child;
    while (c != NULL_POINTER)
    {
        _widget_radio_internal__deselect_group(c, group, self);
        c = c->next_sibling;
    }
}

static void radio_on_mouse_up(gui_node_t* n, int64 x, int64 y, int64 button)
{
    (void)button;
    // Only act when the release lands inside our bounds.
    float fx = (float)x, fy = (float)y;
    if (fx < n->bounds.x || fx >= n->bounds.x + n->bounds.w || fy < n->bounds.y || fy >= n->bounds.y + n->bounds.h)
    {
        return;
    }
    // Already selected -> no-op + no event (HTML behavior).
    if (n->value != 0.0f)
    {
        return;
    }

    // Deselect siblings in the same class-group, then select self.
    gui_node_t* root = scene__root();
    if (root != NULL_POINTER && n->klass[0] != 0)
    {
        _widget_radio_internal__deselect_group(root, n->klass, n);
    }
    n->value = 1.0f;
    scene__dispatch_change(n, n->value);
}

static const widget_vtable_t g_radio_vtable = {
    .type_name       = "radio",
    .apply_attribute = radio_apply_attribute,
    .layout          = radio_layout,
    .emit_draws      = radio_emit_draws,
    .on_mouse_up     = radio_on_mouse_up,
    .consumes_click  = TRUE,
};

void widget_radio__register(void)
{
    widget_registry__register(GUI_NODE_TYPE__RADIO, &g_radio_vtable);
}
