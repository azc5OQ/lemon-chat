// widget_slider.c - draggable horizontal slider.
// attributes:
// min     lower bound of the value (default 0)
// max     upper bound of the value (default 1)
// value   initial value (default 0)
// draw: track (node bg) + thumb (style fg, with a sensible default).
// thumb is square (width = track height), positioned along the
// track by the value fraction.
// input:
// mouse_down  snap value to cursor x (so a click on the empty track
// jumps the thumb there immediately).
// mouse_drag  same snap, called continuously while dragging.
// mouse_up    no-op; slider sets consumes_click so scene skips the
// on_click dispatch entirely (sliders are drag widgets).

#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "renderer.h"
#include "font.h"
#include "third_party/log.h"
#include "clib/stdlib.h"

static boole _widget_slider_internal__parse_float(gui_node_t* n, char* attr_name, char* value, float* out);
static void _widget_slider_internal__format(char* out, int64 cap, float value, int decimals);

// strtof-based parser that distinguishes "user wrote 0" from "atof
// silently returned 0 because the input wasn't a number". Logs a
// warning on garbage input and returns FALSE so the caller can keep
// the previous value (init_defaults seeded sensible numbers).
// atof() on its own is dangerous here: <slider value="oops" /> would
// return 0.0, the slider would silently be at min, and the bug would
// only surface when the user dragged it.
/**
 * @brief print a value with a fixed number of fraction digits (the library's own formatter; no printf inside the widgets)
 * @param char* out -> destination
 * @param int64 cap -> its capacity
 * @param float value -> the value
 * @param int decimals -> fraction digits, 0 to 4
 * @return void
 */
static void _widget_slider_internal__format(char* out, int64 cap, float value, int decimals)
{
    float scale = 1.0f;
    float rounded = 0.0f;
    int64 whole = 0;
    int64 fraction = 0;
    int64 length = 0;
    int i = 0;

    out[0] = 0;
    for (i = 0; i < decimals; i++)
    {
        scale *= 10.0f;
    }
    rounded = value * scale + ((value >= 0.0f) ? 0.5f : -0.5f);
    whole = (int64)(rounded / scale);
    fraction = (int64)rounded - whole * (int64)scale;
    if (fraction < 0)
    {
        fraction = -fraction;
    }
    stdlib__i64toa(out, cap, whole);
    if (decimals <= 0)
    {
        return;
    }
    length = (int64)stdlib__strlen(out);
    if (length + 1 < cap)
    {
        out[length++] = '.';
        out[length] = 0;
    }
    // the fraction keeps its leading zeros
    for (i = decimals - 1; i > 0; i--)
    {
        scale = 1.0f;
        {
            int j = 0;

            for (j = 0; j < i; j++)
            {
                scale *= 10.0f;
            }
        }
        if (fraction < (int64)scale && length + 1 < cap)
        {
            out[length++] = '0';
            out[length] = 0;
        }
    }
    stdlib__i64toa(out + length, cap - length, fraction);
}

static boole _widget_slider_internal__parse_float(gui_node_t* n, char* attr_name, char* value, float* out)
{
    if (value == NULL_POINTER || value[0] == 0)
    {
        log_warn("%s %s %s %s %s", "slider:", attr_name, "='' on id='", (n != NULL_POINTER && n->id[0] != 0) ? n->id : "(no id)", "' is empty; ignoring");
        return FALSE;
    }
    char* end = NULL_POINTER;
    float v   = stdlib__strtof(value, &end);
    if (end == value)
    {
        log_warn("%s %s %s %s %s %s %s", "slider:", attr_name, "='", value, "' on id='", (n != NULL_POINTER && n->id[0] != 0) ? n->id : "(no id)", "' is not a number; ignoring");
        return FALSE;
    }
    // Allow trailing whitespace (XML attribute values can be
    // padded), but anything else after a number means the input
    // wasn't entirely numeric -- e.g. "0.5px", "10%", "1.0,2.0".
    // Reject so the user notices.
    while (*end != 0)
    {
        if (stdlib__isspace((unsigned char)*end) == 0)
        {
            log_warn("%s %s %s %s %s %s %s", "slider:", attr_name, "='", value, "' on id='", (n != NULL_POINTER && n->id[0] != 0) ? n->id : "(no id)", "' has trailing garbage after number; ignoring");
            return FALSE;
        }
        end++;
    }
    *out = v;
    return TRUE;
}

static void slider_init_defaults(gui_node_t* n)
{
    n->value_min = 0.0f;
    n->value_max = 1.0f;
    n->value     = 0.0f;
    n->slider_drag_only = FALSE;
    n->slider_commit_on_release = FALSE;
    n->slider_thumb_aspect = 1.0f; // square thumb by default
}

// Tracks which node (if any) accepted the current drag in drag-only
// mode. NULL = no drag-only slider is currently engaged. Set on a
// successful mouse_down (touch landed on the thumb) and cleared
// implicitly when a different node receives mouse_down or on
// mouse_up. Single-touch is sufficient -- only one drag can be in
// flight at a time across the whole UI.
static gui_node_t* _widget_slider_internal__drag_armed = NULL_POINTER;
// Drag-only slider needs to remember where on the thumb the user
// initially landed so subsequent motion moves the thumb BY THE
// FINGER DELTA, not by snapping the thumb-center to the finger.
// Without this, a touch that lands on the LEFT half of the thumb
// jumps the thumb backward (toward 0) on the first drag tick,
// then refuses to advance until the finger crosses the original
// thumb-center -- user perceives the slider as stuck. Recorded at
// mouse_down (drag-only path), consumed by mouse_drag, cleared on
// mouse_up.
static int64 _widget_slider_internal__drag_initial_finger_x = 0;
static float _widget_slider_internal__drag_initial_value    = 0.0f;

static boole slider_apply_attribute(gui_node_t* n, char* name, char* value)
{
    if (stdlib__strcmp(name, "min") == 0)
    {
        float v;
        if (_widget_slider_internal__parse_float(n, "min", value, &v))
        {
            n->value_min = v;
        }
        return TRUE;
    }
    if (stdlib__strcmp(name, "max") == 0)
    {
        float v;
        if (_widget_slider_internal__parse_float(n, "max", value, &v))
        {
            n->value_max = v;
        }
        return TRUE;
    }
    if (stdlib__strcmp(name, "value") == 0)
    {
        float v;
        if (_widget_slider_internal__parse_float(n, "value", value, &v))
        {
            n->value = v;
        }
        return TRUE;
    }
    if (stdlib__strcmp(name, "step") == 0)
    {
        float v = 0.0f;

        // step="1" gives a whole-number range, step="0.05" a fractional
        // one; without it the value moves continuously
        if (_widget_slider_internal__parse_float(n, "step", value, &v) && v >= 0.0f)
        {
            n->slider_step = v;
        }
        return TRUE;
    }
    if (stdlib__strcmp(name, "thumb-aspect") == 0)
    {
        // <slider thumb-aspect="1.8" /> -- ratio of thumb width to
        // thumb height. 1.0 = square (legacy default). >1 stretches
        // the thumb horizontally for a rectangular handle look (the
        // classic iPhone slide-to-unlock pattern). <1 would make it
        // taller than wide, which is unusual but allowed.
        // Width math at draw time: thumb_w = bounds.h *
        // slider_thumb_aspect. So a 104-tall track at aspect 1.8 ->
        // 187-wide thumb. Travel range shrinks correspondingly
        // (slider.w - thumb_w) so the thumb can still reach both
        // ends of the track.
        float v;
        if (_widget_slider_internal__parse_float(n, "thumb-aspect", value, &v))
        {
            if (v > 0.0f)
            {
                n->slider_thumb_aspect = v;
            }
        }
        return TRUE;
    }
    if (stdlib__strcmp(name, "commit-on-release") == 0)
    {
        // <slider commit-on-release="1"> -- the slider value tracks
        // the finger during drag (visually the thumb follows), but
        // on_change is held back until the user releases. Single
        // dispatch on mouse_up with the final value. Used by slide-
        // to-confirm patterns where the action must only fire on
        // release, not mid-drag, so the user can change their mind
        // by dragging back to the start before lifting.
        if (stdlib__strcmp(value, "1") == 0 || stdlib__strcmp(value, "true") == 0 || stdlib__strcmp(value, "commit-on-release") == 0)
        {
            n->slider_commit_on_release = TRUE;
        }
        else
        {
            n->slider_commit_on_release = FALSE;
        }
        return TRUE;
    }
    if (stdlib__strcmp(name, "drag-only") == 0)
    {
        // <slider drag-only="1"> -- the slider only accepts touches
        // that LAND on the thumb. Tapping the empty track is a no-op
        // (no snap-to-cursor, no on_change). Used by meek-shell's
        // slide-to-unlock so a stray tap near the right edge can't
        // snap-unlock the device; the user has to grab the handle
        // and drag.
        // Truthy strings: "1", "true", "drag-only". Anything else
        // leaves drag-only off.
        if (stdlib__strcmp(value, "1") == 0 || stdlib__strcmp(value, "true") == 0 || stdlib__strcmp(value, "drag-only") == 0)
        {
            n->slider_drag_only = TRUE;
        }
        else
        {
            n->slider_drag_only = FALSE;
        }
        return TRUE;
    }
    return FALSE;
}

static void slider_layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale)
{
    (void)avail_h;
    gui_style_t* s = &n->resolved;
    // Width precedence: explicit pixel `size: 240x20` wins, then a
    // percentage `width: 100%` taken against the available row width
    // (same pattern as widget_image), then the 240px fallback so a
    // bare <slider /> still renders something useful.
    float w;
    if (s->size_w > 0.0f)
    {
        w = s->size_w * scale;
    }
    else if (s->width_pct > 0.0f)
    {
        w = avail_w * (s->width_pct / 100.0f);
    }
    else
    {
        w = 240.0f * scale;
    }
    float h = (s->size_h > 0.0f) ? s->size_h * scale : 20.0f * scale;
    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;
}

// compute the value from a cursor x. shared by mouse_down and mouse_drag.
// dispatches on_change only if the value actually moved.
static void slider_apply_drag(gui_node_t* n, int64 cursor_x)
{
    // Thumb width follows the slider's height by default (square
    // thumb). Setting slider_thumb_aspect > 1 stretches it
    // horizontally -- common for slide-to-confirm patterns. The
    // hit-test we do here uses the same width so a finger that
    // landed on the rectangular thumb is detected as such.
    float thumb_w = n->bounds.h * n->slider_thumb_aspect;
    float travel  = n->bounds.w - thumb_w;
    if (travel <= 0.0f)
    {
        return;
    }

    float local_x = (float)cursor_x - n->bounds.x - thumb_w * 0.5f;
    if (local_x < 0.0f)
    {
        local_x = 0.0f;
    }
    if (local_x > travel)
    {
        local_x = travel;
    }

    float fraction = local_x / travel;
    float range    = n->value_max - n->value_min;
    if (range <= 0.0f)
    {
        range = 1.0f;
    }
    float new_value = n->value_min + fraction * range;

    // step: the value snaps to a multiple of it, counted from the minimum
    if (n->slider_step > 0.0f)
    {
        new_value = n->value_min + stdlib__floorf((new_value - n->value_min) / n->slider_step + 0.5f) * n->slider_step;
        if (new_value > n->value_max)
        {
            new_value = n->value_max;
        }
    }

    if (new_value == n->value)
    {
        return;
    }
    n->value = new_value;
    // commit-on-release mode: update n->value so the thumb visually
    // follows the finger, but suppress on_change dispatch during
    // drag. The single on_change for this gesture will fire from
    // slider_on_mouse_up with the FINAL value at release.
    if (n->slider_commit_on_release)
    {
        return;
    }
    scene__dispatch_change(n, new_value);
}

static void slider_emit_draws(gui_node_t* n, float scale)
{
    gui_rect_t track;
    gui_rect_t fill;
    gui_color_t track_color;
    gui_color_t fill_color;
    gui_color_t text_color;
    gui_font_t* f = NULL_POINTER;
    char text[32];
    float text_w = 0.0f;
    float size_px = 0.0f;
    float fade = 0.0f;

    stdlib__null_memory(&track, sizeof(track));
    stdlib__null_memory(&fill, sizeof(fill));
    stdlib__null_memory(&track_color, sizeof(track_color));
    stdlib__null_memory(&fill_color, sizeof(fill_color));
    stdlib__null_memory(&text_color, sizeof(text_color));
    text[0] = 0;
    // Drain pending batches BEFORE submitting anything for this
    // slider. The renderer's image batch drains AFTER its rect
    // batch in a single flush -- if we let the slider's track
    // rect land in the same flush as a still-pending wallpaper
    // image (lock-screen case), the wallpaper would paint OVER
    // the track and hide it. By flushing first, both the track
    // AND the thumb end up in a fresh batch that drains last,
    // landing on top of any prior images.
    renderer__flush_pending_draws();

    // Track: route through the shared default-bg helper so sliders
    // inherit gradient / shadow / bg-image / blur / border-gradient
    // automatically. The thumb is submitted separately below, on top.
    if (n->resolved.slider_track_h > 0.0f)
    {
        // slider-track-height: the range-input look, a thin track
        // centered in the box in the background color
        track = n->bounds;
        track.h = n->resolved.slider_track_h * scale;
        track.y = n->bounds.y + (n->bounds.h - track.h) * 0.5f;
        track_color = n->resolved.has_background_color ? n->resolved.background_color : scene__rgb(0.8f, 0.8f, 0.8f);
        track_color.a *= n->effective_opacity;
        renderer__submit_rect(track, track_color, n->resolved.radius * scale);
    }
    else
    {
        scene__emit_default_bg(n, scale);
    }

    // thumb: square positioned along the track by the value fraction.
    float range = n->value_max - n->value_min;
    if (range <= 0.0f)
    {
        range = 1.0f;
    }
    float fraction = (n->value - n->value_min) / range;
    if (fraction < 0.0f)
    {
        fraction = 0.0f;
    }
    if (fraction > 1.0f)
    {
        fraction = 1.0f;
    }

    // Thumb width: square (= track height) for legacy default,
    // wider for rectangular handles (slider_thumb_aspect > 1); a
    // slider-thumb-width from the style wins
    float thumb_w = (n->resolved.slider_thumb_w > 0.0f) ? n->resolved.slider_thumb_w * scale : n->bounds.h * n->slider_thumb_aspect;
    float travel  = n->bounds.w - thumb_w;
    if (travel < 0.0f)
    {
        travel = 0.0f;
    }

    gui_rect_t thumb;
    thumb.x = n->bounds.x + travel * fraction;
    thumb.y = n->bounds.y;
    thumb.w = thumb_w;
    thumb.h = n->bounds.h;

    // Fallback thumb color when style doesn't set accent-color.
    // Neutral light gray reads against most backgrounds; once the
    // .style file specifies accent-color, this is never reached.
    gui_color_t thumb_color;
    if (n->resolved.has_accent_color)
    {
        thumb_color = n->resolved.accent_color;
    }
    else
    {
        thumb_color = scene__rgb(0.8f, 0.8f, 0.8f);
    }

    // slider-fill: the track from its start to the thumb, drawn over the
    // track and under the thumb (the filled part of a range)
    if (n->resolved.has_slider_fill == TRUE && n->resolved.slider_track_h > 0.0f)
    {
        fill = n->bounds;
        fill.h = n->resolved.slider_track_h * scale;
        fill.y = n->bounds.y + (n->bounds.h - fill.h) * 0.5f;
        fill.w = thumb.x + thumb.w * 0.5f - n->bounds.x;
        if (fill.w < 0.0f)
        {
            fill.w = 0.0f;
        }
        fill_color = n->resolved.slider_fill;
        fill_color.a *= n->effective_opacity;
        renderer__submit_rect(fill, fill_color, n->resolved.radius * scale);
    }
    renderer__submit_rect(thumb, thumb_color, n->resolved.radius * scale);

    // slider-value: the rounded value above the thumb, while it is held
    // (drag) or always
    // (drag) or always; the label fades in and out over about 150 ms
    if (n->resolved.slider_value_mode != 0)
    {
        fade = scene__widget_ease(n, (n->resolved.slider_value_mode == 2 || n->state == GUI_NODE_STATE__PRESSED) ? 1.0f : 0.0f, 70.0f);
    }
    if (fade > 0.01f)
    {
        size_px = ((n->resolved.font_size > 0.0f) ? n->resolved.font_size : 12.0f) * scale;
        f = font__at(n->resolved.font_family, size_px);
        _widget_slider_internal__format(text, (int64)sizeof(text), n->value, n->resolved.slider_decimals);
        if (f != NULL_POINTER)
        {
            text_w = font__measure(f, text);
            text_color = n->resolved.has_slider_value_color ? n->resolved.slider_value_color : (n->resolved.has_font_color ? n->resolved.font_color : scene__rgb(1.0f, 1.0f, 1.0f));
            text_color.a *= n->effective_opacity * fade;
            // slider-value-position: above the thumb, or beside the track
            if (n->resolved.slider_value_pos == 1)
            {
                font__draw(f, n->bounds.x + n->bounds.w + 6.0f * scale, n->bounds.y + (n->bounds.h + size_px * 0.7f) * 0.5f, text_color, text);
            }
            else if (n->resolved.slider_value_pos == 2)
            {
                font__draw(f, n->bounds.x - text_w - 6.0f * scale, n->bounds.y + (n->bounds.h + size_px * 0.7f) * 0.5f, text_color, text);
            }
            else
            {
                font__draw(f, thumb.x + thumb.w * 0.5f - text_w * 0.5f, thumb.y - 3.0f * scale - (1.0f - fade) * 3.0f * scale, text_color, text);
            }
        }
    }
}

static void slider_on_mouse_down(gui_node_t* n, int64 x, int64 y, int64 button)
{
    (void)y;
    (void)button;
    if (n->slider_drag_only)
    {
        // Drag-only mode: only accept touches that land WITHIN the
        // current thumb rect. Touches outside the thumb leave the
        // slider untouched and the drag-armed marker cleared so any
        // subsequent mouse_drag is also ignored.
        float range = n->value_max - n->value_min;
        if (range <= 0.0f)
        {
            range = 1.0f;
        }
        float fraction = (n->value - n->value_min) / range;
        if (fraction < 0.0f)
        {
            fraction = 0.0f;
        }
        if (fraction > 1.0f)
        {
            fraction = 1.0f;
        }
        float thumb_w = n->bounds.h * n->slider_thumb_aspect;
        float travel  = n->bounds.w - thumb_w;
        if (travel < 0.0f)
        {
            travel = 0.0f;
        }
        float thumb_x = n->bounds.x + travel * fraction;
        if ((float)x < thumb_x || (float)x > thumb_x + thumb_w)
        {
            _widget_slider_internal__drag_armed = NULL_POINTER;
            log_info("%s %s %s %lld %s %.1f %s %.1f %s %.3f %s %.3f %s %.0f %s %.0f %.0f %s %.0f %s", "[slider-dbg] mouse_down REJECT id='", n->id, "' touch_x=", (long long)x, "thumb=[", thumb_x, ",", thumb_x + thumb_w, "] value=", n->value, "frac=", fraction, "bounds=(", n->bounds.x, ",", n->bounds.y, n->bounds.w, "x", n->bounds.h, ")");
            return;
        }
        // Touch landed on the thumb. Record this node as the active
        // drag target so mouse_drag knows to honor the drag. Don't
        // snap value yet -- the user just touched, no motion
        // necessary. The first mouse_drag will move the value.
        _widget_slider_internal__drag_armed = n;
        _widget_slider_internal__drag_initial_finger_x = x;
        _widget_slider_internal__drag_initial_value    = n->value;
        log_info("%s %s %s %lld %s %.1f %s %.1f %s %.3f %s %.0f %s %.0f %.0f %s %.0f %s", "[slider-dbg] mouse_down ACCEPT id='", n->id, "' touch_x=", (long long)x, "thumb=[", thumb_x, ",", thumb_x + thumb_w, "] value=", n->value, "bounds=(", n->bounds.x, ",", n->bounds.y, n->bounds.w, "x", n->bounds.h, ")");
        return;
    }
    slider_apply_drag(n, x);
}

static boole slider_on_mouse_drag(gui_node_t* n, int64 x, int64 y)
{
    (void)y;
    // In drag-only mode, ignore drag events on sliders that didn't
    // accept the corresponding mouse_down (touch landed off-thumb).
    // Without this gate, a touch starting on the track but outside
    // the thumb would suppress mouse_down's snap but then the very
    // first mouse_drag would still snap -- defeating the purpose of
    // drag-only.
    if (n->slider_drag_only && _widget_slider_internal__drag_armed != n)
    {
        log_info("%s %s %s %lld %s %p %s %p %s", "[slider-dbg] mouse_drag IGNORED id='", n->id, "' touch_x=", (long long)x, "(drag_armed=", (void*)_widget_slider_internal__drag_armed, ", this=", (void*)n, ")");
        return TRUE;
    }
    log_info("%s %s %s %lld %s %.3f", "[slider-dbg] mouse_drag APPLY id='", n->id, "' touch_x=", (long long)x, "value=", n->value);
    // Drag-only mode uses RELATIVE motion: the thumb moves by the
    // same px delta the finger moved, starting from where the user
    // landed. Avoids the "finger jumps left of thumb-center -> thumb
    // snaps to 0" trap that the absolute apply_drag has. Non-drag-
    // only sliders (audio volume etc.) keep the old absolute
    // semantics so a tap on the track jumps the thumb to the tap.
    if (n->slider_drag_only)
    {
        float thumb_w = n->bounds.h * n->slider_thumb_aspect;
        float travel  = n->bounds.w - thumb_w;
        if (travel <= 0.0f)
        {
            return TRUE;
        }
        int64 dx = x - _widget_slider_internal__drag_initial_finger_x;
        float range = n->value_max - n->value_min;
        if (range <= 0.0f)
        {
            range = 1.0f;
        }
        float dvalue = ((float)dx / travel) * range;
        float new_value = _widget_slider_internal__drag_initial_value + dvalue;
        if (new_value < n->value_min)
        {
            new_value = n->value_min;
        }
        if (new_value > n->value_max)
        {
            new_value = n->value_max;
        }
        if (new_value == n->value)
        {
            return TRUE;
        }
        n->value = new_value;
        if (n->slider_commit_on_release == FALSE)
        {
            scene__dispatch_change(n, new_value);
        }
        return TRUE;
    }
    slider_apply_drag(n, x);
    return TRUE; // suppress hover update while dragging.
}

// On mouse_up:
// 1. If commit-on-release mode AND this slider was actually
// engaged (either drag-only off, or drag-only on AND the
// touch had landed on the thumb at mouse_down), dispatch the
// held-back on_change with the final value. This is the
// slide-to-confirm commit point.
// 2. Clear the drag-armed marker if it pointed at us. Strictly
// speaking the next mouse_down would reset it anyway, but
// explicit cleanup keeps reasoning local: after touch-up, no
// slider is armed.
static void slider_on_mouse_up(gui_node_t* n, int64 x, int64 y, int64 button)
{
    (void)x;
    (void)y;
    (void)button;
    // "Engaged" = the slider accepted this gesture. For drag-only
    // sliders that's specifically the case where the touch landed
    // on the thumb (drag_armed == n). For regular sliders, any
    // mouse_down on the slider engages.
    boole engaged = TRUE;
    if (n->slider_drag_only && _widget_slider_internal__drag_armed != n)
    {
        engaged = FALSE;
    }
    if (engaged && n->slider_commit_on_release)
    {
        scene__dispatch_change(n, n->value);
    }
    if (_widget_slider_internal__drag_armed == n)
    {
        _widget_slider_internal__drag_armed = NULL_POINTER;
    }
}

static const widget_vtable_t g_slider_vtable = {
    .type_name        = "slider",
    .init_defaults    = slider_init_defaults,
    .apply_attribute  = slider_apply_attribute,
    .layout           = slider_layout,
    .emit_draws       = slider_emit_draws,
    .on_mouse_down    = slider_on_mouse_down,
    .on_mouse_up      = slider_on_mouse_up,
    .on_mouse_drag    = slider_on_mouse_drag,
    .consumes_click   = TRUE, // drag widget; never fires on_click.
};

// Public accessor: returns the slider node that currently has a
// drag-armed touch (drag-only mode only), or NULL if no slider is
// being dragged. Used by host code (e.g. meek-shell's slide-to-
// unlock snap-back animation) to avoid fighting user input -- if
// the snap-back tries to lerp value while the user is dragging,
// the per-tick rewrite clobbers the drag and the user sees the
// thumb barely move.
gui_node_t* widget_slider__drag_armed_node(void)
{
    return _widget_slider_internal__drag_armed;
}

void widget_slider__register(void)
{
    widget_registry__register(GUI_NODE_TYPE__SLIDER, &g_slider_vtable);
}
