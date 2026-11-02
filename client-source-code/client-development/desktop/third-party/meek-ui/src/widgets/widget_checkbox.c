/** Two-state checkbox with independently styled track, thumb and checked mark.
 * Geometry uses logical pixels; :checked, :hover and :pressed select part styles.
 * Border drawing is shared with the scene renderer. No center-erasing fills.
 */

#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "renderer.h"
#include "border.h"
#include "font.h"
#include "animator.h"
#include "motion.h"

// space between the box and its caption, in logical pixels
#define _WIDGET_CHECKBOX_INTERNAL__TEXT_GAP 8.0f
#include "clib/stdlib.h"

typedef struct _widget_checkbox_internal__state_t
{
    int64 started_ms;
    float from;
    float target;
    boole initialized;
} _widget_checkbox_internal__state_t;

static void _widget_checkbox_internal__part(gui_rect_t bounds, const gui_checkbox_part_t* part, gui_color_t fallback, float radius, float scale, float opacity);
static void _widget_checkbox_internal__mark(gui_rect_t bounds, const gui_style_t* style, float scale, float opacity);
static float _widget_checkbox_internal__slide(gui_node_t* node);

/** @brief paints a checkbox track or thumb with independently styled fill and border
 * @param gui_rect_t bounds -> part bounds in pixels
 * @param const gui_checkbox_part_t* part -> resolved partial style
 * @param gui_color_t fallback -> legacy fill when a custom background is absent
 * @param float radius -> default corner radius in pixels
 * @param float scale -> UI scale
 * @param float opacity -> inherited opacity
 * @return void
 */
static void _widget_checkbox_internal__part(gui_rect_t bounds, const gui_checkbox_part_t* part, gui_color_t fallback, float radius, float scale, float opacity)
{
    gui_color_t color = {0};

    if (bounds.w <= 0.0f || bounds.h <= 0.0f)
    {
        return;
    }
    if (part->set & GUI_CHECKBOX_PART__RADIUS)
    {
        radius = part->radius * scale;
    }
    radius = stdlib__fminf(radius, stdlib__fminf(bounds.w, bounds.h) * 0.5f);
    color = (part->set & GUI_CHECKBOX_PART__BACKGROUND) ? part->background : fallback;
    color.a *= opacity;
    if ((part->set & GUI_CHECKBOX_PART__BACKGROUND) && part->has_gradient)
    {
        renderer__submit_gradient(bounds, &part->gradient, scale, opacity, radius);
    }
    else
    {
        renderer__submit_rect(bounds, color, radius);
    }
    if (part->set & GUI_CHECKBOX_PART__BORDER_COLOR)
    {
        color = part->border_color;
        border__draw(bounds, radius, part->border_width * scale, part->border_style, color,
            part->has_border_gradient ? &part->border_gradient : NULL_POINTER, scale, opacity);
    }
}

/** @brief draws the chosen checked mark independently of the box background and border
 * @param gui_rect_t bounds -> checkbox box
 * @param const gui_style_t* style -> resolved mark color, inset, width and shape
 * @param float scale -> UI scale
 * @param float opacity -> inherited opacity
 * @return void
 */
static void _widget_checkbox_internal__mark(gui_rect_t bounds, const gui_style_t* style, float scale, float opacity)
{
    gui_color_t color = {0};
    gui_vec2_t a = {0};
    gui_vec2_t b = {0};
    gui_vec2_t c = {0};
    float inset = 0.0f;
    float width = 0.0f;

    if (style->checkbox_mark == GUI_CHECKBOX_MARK__NONE)
    {
        return;
    }
    inset = style->has_checkbox_mark_inset ? style->checkbox_mark_inset * scale : stdlib__fminf(bounds.w, bounds.h) * 0.25f;
    bounds.x += inset;
    bounds.y += inset;
    bounds.w -= 2.0f * inset;
    bounds.h -= 2.0f * inset;
    if (bounds.w <= 0.0f || bounds.h <= 0.0f)
    {
        return;
    }
    width = style->has_checkbox_mark_width ? style->checkbox_mark_width * scale : 2.0f * scale;
    color = style->has_checkbox_mark_color ? style->checkbox_mark_color : style->has_accent_color ? style->accent_color : scene__rgb(0.39f, 0.40f, 0.95f);
    color.a *= opacity;
    if (style->checkbox_mark == GUI_CHECKBOX_MARK__CHECK)
    {
        a.x = bounds.x;
        a.y = bounds.y + bounds.h * 0.5f;
        b.x = bounds.x + bounds.w * 0.35f;
        b.y = bounds.y + bounds.h;
        c.x = bounds.x + bounds.w;
        c.y = bounds.y;
        renderer__submit_line(a, b, width, color);
        renderer__submit_line(b, c, width, color);
        renderer__submit_circle(b, width * 0.5f, color);
    }
    else if (style->checkbox_mark == GUI_CHECKBOX_MARK__CROSS)
    {
        a.x = bounds.x;
        a.y = bounds.y;
        b.x = bounds.x + bounds.w;
        b.y = bounds.y + bounds.h;
        renderer__submit_line(a, b, width, color);
        a.y = bounds.y + bounds.h;
        b.y = bounds.y;
        renderer__submit_line(a, b, width, color);
    }
    else if (style->checkbox_mark == GUI_CHECKBOX_MARK__BAR)
    {
        a.x = bounds.x;
        a.y = bounds.y + bounds.h * 0.5f;
        b.x = bounds.x + bounds.w;
        b.y = a.y;
        renderer__submit_line(a, b, width, color);
    }
    else
    {
        renderer__submit_rect(bounds, color, style->radius * scale * 0.5f);
    }
}

// Finish at a defined time, with the same curve as the style transition.
// Reversals start at the visible position rather than jumping to an endpoint.
static float _widget_checkbox_internal__slide(gui_node_t* node)
{
    _widget_checkbox_internal__state_t* state = scene__widget_state(node);
    gui_style_t* style = &node->resolved;
    int64 now = scene__frame_time_ms();
    float target = node->value != 0.0f ? 1.0f : 0.0f;
    float duration = style->transition_duration_ms > 0.0f ? style->transition_duration_ms : 200.0f;
    float progress = 0.0f;
    float eased = 0.0f;
    gui_easing_e easing = style->transition_duration_ms > 0.0f ? style->transition_easing : GUI_EASING__EASE_OUT;

    if (state == NULL_POINTER)
    {
        return target;
    }
    if (state->initialized == FALSE || motion__reduced_motion())
    {
        state->initialized = TRUE;
        state->started_ms = now;
        state->from = target;
        state->target = target;
        node->widget_anim = target;
        node->widget_animating = FALSE;
        return target;
    }
    progress = stdlib__fminf(1.0f, stdlib__fmaxf(0.0f, (float)(now - state->started_ms) / duration));
    eased = animator__ease(easing, style->transition_easing_params, progress);
    node->widget_anim = state->from + (state->target - state->from) * eased;
    if (state->target != target)
    {
        state->started_ms = now;
        state->from = node->widget_anim;
        state->target = target;
        progress = 0.0f;
    }
    node->widget_animating = (boole)(progress < 1.0f && state->from != state->target);
    return node->widget_anim;
}

// Per-type defaults. Checkboxes default to 20x20 so they're tappable on
// touch devices without being huge. Overrideable via size: in .style.
static void checkbox_init_defaults(gui_node_t* n)
{
    (void)n;
    // nothing to initialize beyond the zero-initialized state --
    // value = 0 (unchecked) is the sensible default.
}

static boole checkbox_apply_attribute(gui_node_t* n, char* name, char* value)
{
    // Accept both `value=` (our original slot) and `checked=`
    // (HTML/CSS muscle memory). Truthy strings set the node's
    // value to 1; anything else leaves it 0.
    if (stdlib__strcmp(name, "value") == 0 || stdlib__strcmp(name, "checked") == 0)
    {
        if (stdlib__strcmp(value, "1") == 0 || stdlib__strcmp(value, "true") == 0 || stdlib__strcmp(value, "checked") == 0)
        {
            n->value = 1.0f;
        }
        else
        {
            n->value = 0.0f;
        }
        return TRUE;
    }
    return FALSE;
}

static void checkbox_layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale)
{
    (void)avail_w;
    (void)avail_h;
    gui_style_t* s = &n->resolved;
    float w = (s->size_w > 0.0f ? s->size_w : s->switch_look ? 26.0f : 20.0f) * scale;
    float h = (s->size_h > 0.0f) ? s->size_h * scale : 20.0f * scale;
    // the text sits right of the box and stretches the bounds, as the
    // caption of an HTML checkbox does
    if (n->text[0] != 0)
    {
        float     size_px = (s->font_size > 0.0f ? s->font_size : 14.0f) * scale;
        gui_font_t* f       = font__at(s->font_family, size_px);
        if (f != NULL_POINTER)
        {
            float line_h = font__line_height_for(f, s, scale);
            w = w + _WIDGET_CHECKBOX_INTERNAL__TEXT_GAP * scale + font__measure(f, n->text);
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

static void checkbox_emit_draws(gui_node_t* n, float scale)
{
    gui_style_t* s = &n->resolved;
    gui_font_t* font = NULL_POINTER;
    gui_rect_t box = {0};
    gui_rect_t knob = {0};
    gui_checkbox_part_t track = {0};
    gui_color_t pill = {0};
    gui_color_t on_color = {0};
    gui_color_t off_color = {0};
    gui_color_t color = {0};
    gui_color_t thumb_on = {0};
    gui_color_t thumb_off = {0};
    float pad = 0.0f;
    float slide = 0.0f;
    float size_px = 0.0f;
    float line_h = 0.0f;
    float tx = 0.0f;
    float ty = 0.0f;
    float control_width = 0.0f;
    float inset = 0.0f;

    track = s->checkbox_track;
    if (!(track.set & GUI_CHECKBOX_PART__BORDER_WIDTH))
    {
        track.border_width = s->border_width;
    }
    if (!(track.set & GUI_CHECKBOX_PART__BORDER_STYLE))
    {
        track.border_style = s->border_style;
    }
    if (!(track.set & GUI_CHECKBOX_PART__BORDER_COLOR) && s->has_border_color)
    {
        track.border_color = s->border_color;
        track.border_gradient = s->border_gradient;
        track.has_border_gradient = s->has_border_gradient;
        track.set |= GUI_CHECKBOX_PART__BORDER_COLOR;
    }
    if (!(track.set & GUI_CHECKBOX_PART__BACKGROUND) && s->has_bg_gradient)
    {
        track.gradient = s->bg_gradient;
        track.background = s->bg_gradient.stops[0].color;
        track.has_gradient = TRUE;
        track.set |= GUI_CHECKBOX_PART__BACKGROUND;
    }
    box.w = (s->size_w > 0.0f ? s->size_w : s->switch_look ? 26.0f : 20.0f) * scale;
    box.h = (s->size_h > 0.0f ? s->size_h : s->switch_look ? 13.0f : 20.0f) * scale;
    if (s->switch_look && s->switch_track_h > 0.0f)
    {
        box.h = stdlib__fminf(s->switch_track_h * scale, n->bounds.h);
    }
    control_width = box.w;
    box.x = n->bounds.x;
    box.y = n->bounds.y + (n->bounds.h - box.h) * 0.5f;
    if (track.set & GUI_CHECKBOX_PART__WIDTH)
    {
        box.w = stdlib__fminf(track.width * scale, control_width);
        box.x = n->bounds.x + (control_width - box.w) * 0.5f;
    }
    if (track.set & GUI_CHECKBOX_PART__HEIGHT)
    {
        box.h = stdlib__fminf(track.height * scale, n->bounds.h);
        box.y = n->bounds.y + (n->bounds.h - box.h) * 0.5f;
    }
    if (track.set & GUI_CHECKBOX_PART__INSET)
    {
        inset = stdlib__fminf(track.inset * scale, stdlib__fminf(box.w, box.h) * 0.5f);
        box.x += inset;
        box.y += inset;
        box.w -= inset * 2.0f;
        box.h -= inset * 2.0f;
    }
    if (s->switch_look == TRUE)
    {
        slide = _widget_checkbox_internal__slide(n);
        on_color = s->has_accent_color ? s->accent_color : scene__rgb(0.15f, 0.8f, 0.15f);
        off_color = s->has_background_color ? s->background_color : scene__rgb(0.48f, 0.5f, 0.52f);
        pill.r = off_color.r + (on_color.r - off_color.r) * slide;
        pill.g = off_color.g + (on_color.g - off_color.g) * slide;
        pill.b = off_color.b + (on_color.b - off_color.b) * slide;
        pill.a = off_color.a + (on_color.a - off_color.a) * slide;
        _widget_checkbox_internal__part(box, &track, pill, s->radius > 0.0f ? s->radius * scale : box.h * 0.5f, scale, n->effective_opacity);
        pad = s->switch_thumb_size > 0.0f ? 0.0f : box.h * 0.08f;
        knob.h = s->switch_thumb_size > 0.0f ? stdlib__fminf(s->switch_thumb_size * scale, stdlib__fminf(n->bounds.h, box.w)) : box.h - pad * 2.0f;
        knob.w = knob.h;
        if (s->checkbox_thumb.set & GUI_CHECKBOX_PART__WIDTH)
        {
            knob.w = stdlib__fminf(s->checkbox_thumb.width * scale, box.w);
        }
        if (s->checkbox_thumb.set & GUI_CHECKBOX_PART__HEIGHT)
        {
            knob.h = stdlib__fminf(s->checkbox_thumb.height * scale, n->bounds.h);
        }
        if (s->checkbox_thumb.set & GUI_CHECKBOX_PART__INSET)
        {
            pad = stdlib__fminf(s->checkbox_thumb.inset * scale, (box.w - knob.w) * 0.5f);
        }
        pad = stdlib__fminf(pad, (box.w - knob.w) * 0.5f);
        knob.y = box.y + (box.h - knob.h) * 0.5f;
        knob.x = box.x + pad + (box.w - pad * 2.0f - knob.w) * slide;
        thumb_on = s->has_switch_thumb_color ? s->switch_thumb_color : scene__rgba(0.96f, 0.96f, 0.96f, 1.0f);
        thumb_off = s->has_switch_thumb_off_color ? s->switch_thumb_off_color : thumb_on;
        color.r = thumb_off.r + (thumb_on.r - thumb_off.r) * slide;
        color.g = thumb_off.g + (thumb_on.g - thumb_off.g) * slide;
        color.b = thumb_off.b + (thumb_on.b - thumb_off.b) * slide;
        color.a = thumb_off.a + (thumb_on.a - thumb_off.a) * slide;
        _widget_checkbox_internal__part(knob, &s->checkbox_thumb, color, stdlib__fminf(knob.h, knob.w) * 0.5f, scale, n->effective_opacity);
    }
    else
    {
        color = s->has_background_color ? s->background_color : scene__rgb(0.22f, 0.23f, 0.28f);
        _widget_checkbox_internal__part(box, &track, color, s->radius * scale, scale, n->effective_opacity);
        if (n->value != 0.0f)
        {
            _widget_checkbox_internal__mark(box, s, scale, n->effective_opacity);
        }
    }
    if (n->text[0] != 0)
    {
        size_px = (s->font_size > 0.0f ? s->font_size : 14.0f) * scale;
        font = font__at(s->font_family, size_px);
        if (font != NULL_POINTER)
        {
            line_h = font__line_height_for(font, s, scale);
            tx = n->bounds.x + control_width + _WIDGET_CHECKBOX_INTERNAL__TEXT_GAP * scale;
            ty = n->bounds.y + (n->bounds.h - line_h) * 0.5f + font__baseline_for(font, s, scale);
            color = s->has_font_color ? s->font_color : scene__rgb(1.0f, 1.0f, 1.0f);
            color.a *= n->effective_opacity;
            font__draw(font, tx, ty, color, n->text);
        }
    }
}

// Toggle on release-over-self. We use on_mouse_up rather than on_click
// so the toggle happens deterministically even when the host didn't
// supply an on_click="" attribute.
static void checkbox_on_mouse_up(gui_node_t* n, int64 x, int64 y, int64 button)
{
    (void)button;
    // Make sure the release landed on us. scene tracks the "pressed"
    // node and calls our on_mouse_up with the original pressed node,
    // but scene also passes the release coordinates so we can confirm
    // the cursor is still inside our bounds -- prevents "drag off to
    // cancel" style toggles, matching native platform behavior.
    float fx = (float)x, fy = (float)y;
    if (fx < n->bounds.x || fx >= n->bounds.x + n->bounds.w || fy < n->bounds.y || fy >= n->bounds.y + n->bounds.h)
    {
        return;
    }

    // Flip and fire on_change. scene__dispatch_change is a no-op when
    // the node has no on_change handler wired.
    n->value = (n->value != 0.0f) ? 0.0f : 1.0f;
    scene__dispatch_change(n, n->value);
}

static const widget_vtable_t g_checkbox_vtable = {
    .type_name       = "checkbox",
    .init_defaults   = checkbox_init_defaults,
    .apply_attribute = checkbox_apply_attribute,
    .layout          = checkbox_layout,
    .emit_draws      = checkbox_emit_draws,
    .on_mouse_up     = checkbox_on_mouse_up,
    // consumes_click = TRUE so scene doesn't ALSO fire our on_click
    // handler (we handle the whole interaction in on_mouse_up).
    .consumes_click  = TRUE,
    .state_size     = sizeof(_widget_checkbox_internal__state_t),
};

void widget_checkbox__register(void)
{
    widget_registry__register(GUI_NODE_TYPE__CHECKBOX, &g_checkbox_vtable);
}
