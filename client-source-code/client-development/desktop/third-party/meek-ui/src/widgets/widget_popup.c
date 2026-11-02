// widget_popup.c - <popup> widget.
// Modal overlay. Two types chosen via `type="..."`:
// confirm        Body message + two buttons (YES / NO). on_change
// fires on dismiss with ev->popup.confirmed = TRUE
// for YES, FALSE for NO.
// option-select  Body message + one button (OK). on_change fires
// with ev->popup.confirmed = TRUE on dismiss.
// (The "option-select" name is kept because the
// imperative scene__popup_ok / scene__popup_confirm
// API always routes through this single widget.)
// Attributes:
// text           body message
// ok-text        label for the OK / YES button (default "OK" or "Yes")
// cancel-text    label for the NO button (default "No") -- confirm only
// open           "true" to show at startup, otherwise hidden
// Input model:
// When the popup is open, it installs itself as the scene overlay
// via scene__set_overlay. Scene's hit-test routes all clicks inside
// the popup bounds to this node, so content behind the popup is
// effectively modal-blocked. Clicking outside the popup is ignored
// (no ambient dismissal -- matches dialog conventions).
// Imperative API (scene__popup_ok / scene__popup_confirm) builds a
// throwaway popup node, inserts it as a direct child of the current
// root, marks it open, and hands it the supplied handler name. The
// node frees itself after the dismiss event fires.

#include <string.h>
#include <stdlib.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "renderer.h"
#include "font.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "third_party/log.h"
#include "clib/stdlib.h"

static void _popup_internal__copy_str(char* dst, size_t dst_cap, const char* src);

// Bounded string copy used wherever we drop a literal or attribute
// value into one of the fixed-size text slots (ok_text, cancel_text).
// Centralised so a future buffer-size change can't introduce a silent
// strcpy overflow if a literal grows past 32 bytes -- and so the
// pattern reads consistently next to the manually-bounded loops in
// popup_apply_attribute.
static void _popup_internal__copy_str(char* dst, size_t dst_cap, const char* src)
{
    if (dst == NULL_POINTER || dst_cap == 0)
    {
        return;
    }
    if (src == NULL_POINTER)
    {
        dst[0] = 0;
        return;
    }
    size_t i = 0;
    while (src[i] != 0 && i + 1 < dst_cap)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

typedef enum _popup_internal__kind_e
{
    _POPUP_INTERNAL__KIND_OK      = 0,
    _POPUP_INTERNAL__KIND_CONFIRM = 1,
} _popup_internal__kind_e;

typedef struct _popup_internal__state_t
{
    _popup_internal__kind_e kind;              // 0x0
    char                  ok_text[32];       // 0x4
    char                  cancel_text[32];   // 0x24
    // Cached button rects -- computed in the overlay draw callback
    // and read by on_mouse_down to decide which button was clicked.
    // Stored in screen space. NOTE: during the open / close
    // animation these rects slide along with the panel, but hit-
    // test compares against the LAST drawn positions so a click
    // mid-animation still lands on the right button (you can
    // dismiss early).
    gui_rect_t ok_btn;                         // 0x44
    gui_rect_t cancel_btn;                     // 0x54
    // TRUE when this popup was allocated by scene__popup_ok /
    // scene__popup_confirm -- the dismiss path then calls
    // scene__node_free on itself after firing the handler.
    boole    self_destruct;                  // 0x64
    boole    ok_hover;                       // 0x65
    boole    cancel_hover;                   // 0x66
    // Open / close animation progress. anim_t in [0, 1] where 1 is
    // fully open. closing flips true when the user clicked a
    // button -- the animator then ramps anim_t down to 0 and
    // fires the real dismiss (event + self-destruct) in the
    // draw callback after the out-animation finishes. Without
    // this the popup would vanish instantly on click; with it,
    // the out-animation plays before the handler sees the event.
    float    anim_t;                         // 0x68
    boole    closing;                        // 0x6C
    boole    confirmed_pending;              // 0x6D
} _popup_internal__state_t;

static _popup_internal__state_t* _popup_internal__state_of(gui_node_t* n)
{
    if (n->user_data == NULL_POINTER)
    {
        _popup_internal__state_t* s = (_popup_internal__state_t*)GUI_CALLOC_T(1, sizeof(_popup_internal__state_t), MM_TYPE__GENERIC);
        if (s != NULL_POINTER)
        {
            _popup_internal__copy_str(s->ok_text,     sizeof(s->ok_text),     "OK");
            _popup_internal__copy_str(s->cancel_text, sizeof(s->cancel_text), "Cancel");
        }
        n->user_data = s;
    }
    return (_popup_internal__state_t*)n->user_data;
}

// Draw callback registered with scene__set_overlay. Paints: full-
// viewport scrim, centered panel with body text + buttons.
static void _popup_internal__overlay_draw(gui_node_t* n, float scale);

static void _popup_internal__open(gui_node_t* n)
{
    _popup_internal__state_t* st = _popup_internal__state_of(n);
    n->is_open = TRUE;
    if (st != NULL_POINTER)
    {
        // Reset animation state. anim_t starts at 0 and the draw
        // callback ramps it toward 1 over _POPUP_INTERNAL__OPEN_MS.
        // closing is explicitly cleared so a popup that's re-opened
        // after a dismiss (same node, e.g. a reusable confirm
        // attached via open="true") animates in cleanly.
        st->anim_t            = 0.0f;
        st->closing           = FALSE;
        st->confirmed_pending = FALSE;
    }
    // Install ourselves as the scene overlay. Bounds = full viewport
    // so hit-test routes every click to the popup's on_mouse_down
    // while it's open (modal semantics).
    int64 vw = 0, vh = 0;
    scene__viewport(&vw, &vh);
    gui_rect_t full;
    full.x = 0.0f; full.y = 0.0f;
    full.w = (float)vw; full.h = (float)vh;
    scene__set_overlay(n, full, _popup_internal__overlay_draw);
}

// Begin the out-animation. Does NOT fire the dismiss event or tear
// down yet -- that happens at the tail of the draw callback when
// anim_t reaches 0. Splitting it this way lets the close animation
// play out before the handler sees the event, so the user sees the
// panel shrink + fade instead of popping out of existence.
static void _popup_internal__begin_close(gui_node_t* n, boole confirmed)
{
    _popup_internal__state_t* st = (_popup_internal__state_t*)n->user_data;
    if (st == NULL_POINTER || n->is_open == FALSE)
    {
        return;
    }
    st->closing           = TRUE;
    st->confirmed_pending = confirmed;
}

// Fire the dismiss event + drop the overlay + self-destruct (if
// applicable). Called from the draw callback after the close
// animation finishes, or directly from on_destroy for teardown.
static void _popup_internal__finalize_close(gui_node_t* n)
{
    if (n->is_open == FALSE)
    {
        return;
    }
    _popup_internal__state_t* st = (_popup_internal__state_t*)n->user_data;
    boole confirmed = (st != NULL_POINTER) ? st->confirmed_pending : FALSE;

    n->is_open = FALSE;
    scene__set_overlay(NULL_POINTER, (gui_rect_t){0.0f, 0.0f, 0.0f, 0.0f}, NULL_POINTER);

    gui_event_t ev;
    stdlib__memset(&ev, 0, sizeof(ev));
    ev.type             = GUI_EVENT_TYPE__CHANGE;
    ev.sender           = n;
    ev.popup.confirmed  = confirmed;
    ev.popup.index      = 0;
    scene__dispatch_event(n, &ev);

    if (st != NULL_POINTER && st->self_destruct)
    {
        // Detach + free. Safe here because we're at the end of the
        // overlay draw callback (outside the main tree walk).
        gui_node_t* parent = n->parent;
        if (parent != NULL_POINTER)
        {
            gui_node_t** pp = &parent->first_child;
            while (*pp != NULL_POINTER && *pp != n)
            {
                pp = &(*pp)->next_sibling;
            }
            if (*pp == n)
            {
                *pp = n->next_sibling;
                if (parent->last_child == n)
                {
                    // Walk to the new tail (or NULL if we were the
                    // only child). Cheap: popup insertion only ever
                    // appends, so this runs over a short list.
                    gui_node_t* tail = parent->first_child;
                    while (tail != NULL_POINTER && tail->next_sibling != NULL_POINTER)
                    {
                        tail = tail->next_sibling;
                    }
                    parent->last_child = tail;
                }
                parent->child_count--;
            }
        }
        n->parent       = NULL_POINTER;
        n->next_sibling = NULL_POINTER;
        scene__node_free(n);
    }
}

static boole popup_apply_attribute(gui_node_t* n, char* name, char* value)
{
    _popup_internal__state_t* st = _popup_internal__state_of(n);
    if (st == NULL_POINTER)
    {
        return FALSE;
    }
    if (stdlib__strcmp(name, "type") == 0)
    {
        if      (stdlib__strcmp(value, "confirm")       == 0)
        {
            st->kind = _POPUP_INTERNAL__KIND_CONFIRM;
            _popup_internal__copy_str(st->ok_text, sizeof(st->ok_text), "Yes");
            _popup_internal__copy_str(st->cancel_text, sizeof(st->cancel_text), "No");
        }
        else if (stdlib__strcmp(value, "option-select") == 0)
        {
            st->kind = _POPUP_INTERNAL__KIND_OK;
            _popup_internal__copy_str(st->ok_text, sizeof(st->ok_text), "OK");
        }
        else
        {
            log_warn("%s %s %s", "popup: unknown type '", value, "' (confirm|option-select)");
        }
        return TRUE;
    }
    if (stdlib__strcmp(name, "ok-text") == 0)
    {
        size_t i = 0;
        while (value[i] != 0 && i + 1 < sizeof(st->ok_text))
        {
            st->ok_text[i] = value[i];
            i++;
        }
        st->ok_text[i] = 0;
        return TRUE;
    }
    if (stdlib__strcmp(name, "cancel-text") == 0)
    {
        size_t i = 0;
        while (value[i] != 0 && i + 1 < sizeof(st->cancel_text))
        {
            st->cancel_text[i] = value[i];
            i++;
        }
        st->cancel_text[i] = 0;
        return TRUE;
    }
    if (stdlib__strcmp(name, "open") == 0)
    {
        if (stdlib__strcmp(value, "true") == 0)
        {
            n->is_open = TRUE;
        }
        return TRUE;
    }
    return FALSE;
}

// Layout: the popup is modal and owns its own screen position, so
// it contributes NOTHING to the parent's layout. We set bounds to
// zero so the column / row containing it doesn't reserve space.
static void popup_layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale)
{
    (void)avail_w; (void)avail_h; (void)scale;
    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = 0.0f;
    n->bounds.h = 0.0f;

    // If the popup was opened externally (e.g. via attribute `open="true"`
    // or the imperative API), install it as the scene overlay. We do
    // this every frame while open so hot reload rebuilding the tree
    // keeps the overlay live.
    if (n->is_open)
    {
        int64 vw = 0, vh = 0;
        scene__viewport(&vw, &vh);
        gui_rect_t full;
        full.x = 0.0f; full.y = 0.0f;
        full.w = (float)vw; full.h = (float)vh;
        // Only (re)install if we aren't already the overlay owner.
        if (scene__overlay_node() != n)
        {
            scene__set_overlay(n, full, _popup_internal__overlay_draw);
        }
    }
}

// Minimal inline text drawer -- we don't depend on widget_text so
// the popup stays self-contained. font__at(NULL, ...) picks the
// first registered family (usually Roboto-Regular). size is in
// physical pixels (already multiplied by scene__scale() at the
// call site).
static void _popup_internal__draw_text(float x, float y, gui_color_t c, char* text, float size)
{
    if (text == NULL_POINTER || text[0] == 0)
    {
        return;
    }
    gui_font_t* f = font__at(NULL_POINTER, size);
    if (f == NULL_POINTER)
    {
        return;
    }
    font__draw(f, x, y + font__ascent(f), c, text);
}

static float _popup_internal__measure(char* text, float size)
{
    if (text == NULL_POINTER || text[0] == 0)
    {
        return 0.0f;
    }
    gui_font_t* f = font__at(NULL_POINTER, size);
    if (f == NULL_POINTER)
    {
        return 0.0f;
    }
    return font__measure(f, text);
}

// Cubic ease-out-back: overshoots the target by ~10% around t=0.75
// then settles to 1.0 at t=1.0. Gives the panel a subtle "pop"
// feel on open -- think iOS alert entrance. The `s` constant
// controls overshoot amount; 1.70158 is the widely-used default
// (Robert Penner easing curves).
static float _popup_internal__ease_out_back(float t)
{
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    float tm = t - 1.0f;
    return 1.0f + c3 * tm * tm * tm + c1 * tm * tm;
}

static float _popup_internal__ease_out_quad(float t)
{
    return 1.0f - (1.0f - t) * (1.0f - t);
}

static float _popup_internal__ease_in_quad(float t)
{
    return t * t;
}

// Linearly map a source range [a, b] into t=[0, 1], clamped.
// Used in the scrim/panel layering so earlier stages of the open
// animation finish before later stages start (scrim fades first,
// panel scales in after).
static float _popup_internal__remap_clamped(float t, float a, float b)
{
    if (b <= a)
    {
        return t < a ? 0.0f : 1.0f;
    }
    float u = (t - a) / (b - a);
    if (u < 0.0f)
    {
        u = 0.0f;
    }
    if (u > 1.0f)
    {
        u = 1.0f;
    }
    return u;
}

// Animation timing. 220 ms to open, 160 ms to close. Slightly
// asymmetric: the out-animation is faster because the user already
// made a decision and waiting on a shrinking panel feels
// obstructive. Layering within the open window:
// t in [0.00, 0.70]   scrim fades in (linear).
// t in [0.15, 1.00]   panel fades + scales in with ease_out_back.
// The slight delay before the panel starts lets the scrim lay
// down the "this is modal" visual cue before the content arrives,
// and it means a hasty click on the scrim during the first 30 ms
// isn't frantically chasing a half-drawn panel.
#define _POPUP_INTERNAL__OPEN_MS    220.0f
#define _POPUP_INTERNAL__CLOSE_MS   160.0f

static void _popup_internal__overlay_draw(gui_node_t* n, float scale)
{
    _popup_internal__state_t* st = (_popup_internal__state_t*)n->user_data;
    if (st == NULL_POINTER)
    {
        return;
    }

    // Advance anim_t. scene__frame_delta_ms() is clamped at its
    // source so a paused / resumed app doesn't fast-forward the
    // animation to completion.
    float dt_ms = (float)scene__frame_delta_ms();
    if (st->closing)
    {
        float step = dt_ms / _POPUP_INTERNAL__CLOSE_MS;
        st->anim_t -= step;
        if (st->anim_t <= 0.0f)
        {
            st->anim_t = 0.0f;
            // Close animation complete: fire the dismiss event +
            // self-destruct (if applicable). Must happen BEFORE we
            // issue any more render calls referencing `n`, because
            // finalize_close may free it.
            _popup_internal__finalize_close(n);
            return;
        }
    }
    else
    {
        float step = dt_ms / _POPUP_INTERNAL__OPEN_MS;
        st->anim_t += step;
        if (st->anim_t > 1.0f)
        {
            st->anim_t = 1.0f;
        }
    }
    float t = st->anim_t;

    int64 vw = 0, vh = 0;
    scene__viewport(&vw, &vh);

    // Bracket everything the popup submits with a viewport-sized
    // scissor push/pop. The push drains the main scene's queued
    // rects + text first so tree labels don't end up drawn ON TOP
    // of the popup's panel bg (text batches always flush after
    // rect batches; without this split, every tree label would
    // visibly bleed through our scrim + panel). The pop drains the
    // popup's own submissions in the right order.
    gui_rect_t full;
    full.x = 0.0f; full.y = 0.0f;
    full.w = (float)vw; full.h = (float)vh;
    renderer__push_scissor(full);

    // Scrim fade. Linear alpha ramp; doesn't need easing because
    // the target is "visible enough to hint modality" not "exact
    // value". Layered earlier than the panel (t_scrim hits 1 at
    // t=0.70) so the scrim is fully up before the panel lands --
    // fancier than everything fading in lockstep.
    float t_scrim = _popup_internal__remap_clamped(t, 0.0f, 0.70f);
    gui_color_t scrim;
    scrim.r = 0.0f; scrim.g = 0.0f; scrim.b = 0.0f; scrim.a = 0.55f * t_scrim;
    renderer__submit_rect(full, scrim, 0.0f);

    // Panel geometry. Every measurement here is in PHYSICAL PIXELS
    // and multiplied by `scale` (the global UI scale factor, fed
    // in via the overlay draw callback) so the panel stays a
    // consistent physical size across a 1.0x desktop and a 3.0x
    // Android phone. Previously the clamps were in raw pixels and
    // a 3x scale phone rendered a 480-px-max panel that read as
    // ~160 logical pixels wide -- tiny on a 6-inch screen.
    if (scale <= 0.0f)
    {
        scale = 1.0f;
    }

    float panel_w = (float)vw * 0.80f;
    float min_w   = 320.0f * scale;
    float max_w   = 560.0f * scale;
    if (panel_w < min_w)
    {
        panel_w = min_w;
    }
    if (panel_w > max_w)
    {
        panel_w = max_w;
    }
    // On a narrow phone we often can't even hit the min without
    // eating the whole width; fall back so there's at least a
    // small margin on each side.
    float edge_margin = 16.0f * scale;
    if (panel_w > (float)vw - 2.0f * edge_margin)
    {
        panel_w = (float)vw - 2.0f * edge_margin;
    }

    // Body font + padding. Bumped from 14 to 16 so single-line
    // messages read comfortably on a phone. Also drives button
    // size via touch-target sizing rules.
    float body_size = 16.0f * scale;
    float btn_size  = 15.0f * scale;
    float pad_x     = 22.0f * scale;
    float pad_top   = 26.0f * scale;
    float btn_h     = 44.0f * scale; // 44 pt matches Apple HIG + Android min touch target
    float btn_min_w = 96.0f * scale;
    float btn_gap   = 10.0f * scale;
    float btn_pad_x = 18.0f * scale; // horizontal padding inside each button

    // Measure button widths from the text so localized labels
    // ("Confirm" vs "OK", "Annuler" vs "Cancel") don't clip.
    float ok_text_w     = _popup_internal__measure(st->ok_text, btn_size);
    float cancel_text_w = _popup_internal__measure(st->cancel_text, btn_size);
    float ok_w     = ok_text_w + 2.0f * btn_pad_x;
    float cancel_w = cancel_text_w + 2.0f * btn_pad_x;
    if (ok_w     < btn_min_w)
    {
        ok_w     = btn_min_w;
    }
    if (cancel_w < btn_min_w)
    {
        cancel_w = btn_min_w;
    }

    // Dynamic height: space for body text + buttons + padding.
    // Body text today is single-line; an upgrade to measure-with-
    // wrap would bump this once we pass a wrap-width into the
    // font path. For now height grows with scale which is what
    // phones need.
    float body_line_h = body_size * 1.4f;
    float panel_h     = pad_top + body_line_h + 20.0f * scale + btn_h + pad_top * 0.8f;

    // Scale + translate around the panel's center for the open
    // animation. ease_out_back starts at ~0.92 (0.8 * t + 0.12
    // offset), overshoots ~1.02 around t=0.8, and settles at 1.0.
    // We also fade the panel in with an ease_out_quad to hide the
    // initial "tiny" frames visually.
    float t_panel        = _popup_internal__remap_clamped(t, 0.15f, 1.00f);
    float scale_anim     = 0.92f + 0.08f * _popup_internal__ease_out_back(t_panel);
    float opacity_anim   = _popup_internal__ease_out_quad(t_panel);

    // Slight upward glide -- panel rises ~8*scale pixels during
    // the in-animation, more subtle than a full drop-from-above
    // but still provides a sense of motion.
    float glide_anim = (1.0f - t_panel) * 8.0f * scale;

    // Apply animation to the panel rect: center-anchored scale +
    // glide offset.
    float panel_anim_w = panel_w * scale_anim;
    float panel_anim_h = panel_h * scale_anim;
    gui_rect_t panel;
    panel.x = ((float)vw - panel_anim_w) * 0.5f;
    panel.y = ((float)vh - panel_anim_h) * 0.5f + glide_anim;
    panel.w = panel_anim_w;
    panel.h = panel_anim_h;

    // Soft shadow behind panel. Shadow opacity + offset also
    // animate so it feels like the panel is landing from above.
    // Radius (shape) and blur (feather) are scaled so the shadow
    // reads at the same physical softness across densities.
    gui_color_t sh; sh.r = 0.0f; sh.g = 0.0f; sh.b = 0.0f;
    sh.a = 0.45f * opacity_anim;
    float shadow_dy = (4.0f + 16.0f * t_panel) * scale;
    gui_rect_t shadow;
    shadow.x = panel.x;
    shadow.y = panel.y + shadow_dy;
    shadow.w = panel.w;
    shadow.h = panel.h;
    renderer__submit_shadow(shadow, sh, 16.0f * scale, 28.0f * scale);

    // Panel body. The bg alpha is multiplied by opacity_anim so
    // the panel ghosts in during the open animation instead of
    // popping.
    gui_color_t panel_bg;
    panel_bg.r = 0.10f; panel_bg.g = 0.11f; panel_bg.b = 0.15f;
    panel_bg.a = opacity_anim;
    renderer__submit_rect(panel, panel_bg, 14.0f * scale);

    // Inner highlight stroke -- one pixel-ish lighter rect inset
    // by 1 for a subtle glass edge. Alpha driven by the same
    // animation. Mirrors the iOS / Material "material" feel
    // without needing a real backdrop blur.
    gui_color_t hi;
    hi.r = 1.0f; hi.g = 1.0f; hi.b = 1.0f; hi.a = 0.04f * opacity_anim;
    gui_rect_t inner = panel;
    float inset = 1.0f * scale;
    inner.x += inset; inner.y += inset;
    inner.w -= 2.0f * inset; inner.h -= 2.0f * inset;
    renderer__submit_rect(inner, hi, 13.0f * scale);

    gui_color_t body_color;
    body_color.r = 0.9f; body_color.g = 0.92f; body_color.b = 0.96f;
    body_color.a = opacity_anim;

    gui_color_t title_color;
    title_color.r = 1.0f; title_color.g = 1.0f; title_color.b = 1.0f;
    title_color.a = opacity_anim;

    // Body text. Simple single-line; wraps would require widget_text
    // integration which is heavier than this widget justifies.
    // Positioned inside pad_x/pad_top margins scaled with density.
    _popup_internal__draw_text( panel.x + pad_x, panel.y + pad_top - body_size * 0.1f, title_color, n->text, body_size);

    // Buttons row at the bottom. Height + widths measured above;
    // we right-align: OK at the right edge, Cancel (if confirm
    // kind) to its left.
    float btn_y = panel.y + panel.h - btn_h - pad_top * 0.8f;

    gui_rect_t ok;
    ok.w = ok_w; ok.h = btn_h;
    ok.x = panel.x + panel.w - ok_w - edge_margin;
    ok.y = btn_y;
    st->ok_btn = ok;

    gui_color_t ob;
    if (st->ok_hover)
    {
        ob.r = 0.51f; ob.g = 0.55f; ob.b = 0.97f;
    }
    else
    {
        ob.r = 0.39f; ob.g = 0.40f; ob.b = 0.94f;
    }
    ob.a = opacity_anim;
    renderer__submit_rect(ok, ob, 9.0f * scale);
    _popup_internal__draw_text( ok.x + (ok.w - ok_text_w) * 0.5f, ok.y + (ok.h - btn_size) * 0.5f, body_color, st->ok_text, btn_size);

    if (st->kind == _POPUP_INTERNAL__KIND_CONFIRM)
    {
        gui_rect_t cancel;
        cancel.w = cancel_w; cancel.h = btn_h;
        cancel.x = ok.x - cancel_w - btn_gap;
        cancel.y = btn_y;
        st->cancel_btn = cancel;

        gui_color_t cb;
        if (st->cancel_hover)
        {
            cb.r = 0.22f; cb.g = 0.24f; cb.b = 0.30f;
        }
        else
        {
            cb.r = 0.17f; cb.g = 0.19f; cb.b = 0.24f;
        }
        cb.a = opacity_anim;
        renderer__submit_rect(cancel, cb, 9.0f * scale);
        _popup_internal__draw_text( cancel.x + (cancel.w - cancel_text_w) * 0.5f, cancel.y + (cancel.h - btn_size) * 0.5f, body_color, st->cancel_text, btn_size);
    }
    else
    {
        // OK-only kind: nothing in the cancel slot.
        st->cancel_btn.w = 0.0f;
        st->cancel_btn.h = 0.0f;
    }

    // Pair for the scissor push at the top of the function. Flushes
    // the popup's own rect + text batches cleanly.
    renderer__pop_scissor();
}

static boole _popup_internal__hit(gui_rect_t r, int64 x, int64 y)
{
    float fx = (float)x;
    float fy = (float)y;
    return (boole)(fx >= r.x && fy >= r.y && fx < r.x + r.w && fy < r.y + r.h);
}

static void popup_on_mouse_down(gui_node_t* n, int64 x, int64 y, int64 button)
{
    (void)button;
    _popup_internal__state_t* st = (_popup_internal__state_t*)n->user_data;
    if (st == NULL_POINTER || n->is_open == FALSE)
    {
        return;
    }
    // Already closing? Swallow further clicks so rage-clicking
    // during the out-animation can't fire the handler twice.
    if (st->closing)
    {
        return;
    }
    if (_popup_internal__hit(st->ok_btn, x, y))
    {
        _popup_internal__begin_close(n, TRUE);
        return;
    }
    if (st->kind == _POPUP_INTERNAL__KIND_CONFIRM && _popup_internal__hit(st->cancel_btn, x, y))
    {
        _popup_internal__begin_close(n, FALSE);
        return;
    }
    // Clicks in the scrim / panel body are swallowed (no ambient
    // dismiss). Modal semantics.
}

static boole popup_on_mouse_drag(gui_node_t* n, int64 x, int64 y)
{
    // Track button-hover during drag so the buttons light up when
    // the mouse is over them.
    _popup_internal__state_t* st = (_popup_internal__state_t*)n->user_data;
    if (st == NULL_POINTER)
    {
        return FALSE;
    }
    st->ok_hover     = _popup_internal__hit(st->ok_btn,     x, y);
    st->cancel_hover = _popup_internal__hit(st->cancel_btn, x, y);
    return TRUE;
}

static void popup_on_destroy(gui_node_t* n)
{
    // If we're still overlay owner at destroy time, clear the slot
    // so scene doesn't chase a dangling pointer. This fires for
    // hot-reload tree rebuilds (parser tears down the old tree
    // before the new one goes up) and for scene__node_free calls
    // from the finalize path above. In both cases we want the
    // overlay slot cleared without trying to replay the close
    // animation -- the node's going away immediately.
    if (scene__overlay_node() == n)
    {
        scene__set_overlay(NULL_POINTER, (gui_rect_t){0.0f, 0.0f, 0.0f, 0.0f}, NULL_POINTER);
    }
    if (n->user_data != NULL_POINTER)
    {
        GUI_FREE(n->user_data);
        n->user_data = NULL_POINTER;
    }
}

static void popup_init_defaults(gui_node_t* n)
{
    _popup_internal__state_of(n);
}

static const widget_vtable_t g_popup_vtable = {
    .type_name       = "popup",
    .init_defaults   = popup_init_defaults,
    .apply_attribute = popup_apply_attribute,
    .layout          = popup_layout,
    .emit_draws      = NULL_POINTER, // popup draws only via overlay callback
    .on_mouse_down   = popup_on_mouse_down,
    .on_mouse_drag   = popup_on_mouse_drag,
    .on_destroy      = popup_on_destroy,
    .consumes_click  = TRUE,
};

void widget_popup__register(void)
{
    widget_registry__register(GUI_NODE_TYPE__POPUP, &g_popup_vtable);
}

// Imperative API helpers. Called from scene.c wrappers.
void _widget_popup__build_and_attach(char* message, char* handler_name, _popup_internal__kind_e kind);

void _widget_popup__build_and_attach(char* message, char* handler_name, _popup_internal__kind_e kind)
{
    gui_node_t* root = scene__root();
    if (root == NULL_POINTER)
    {
        log_warn("%s", "scene__popup_*: no scene root installed yet");
        return;
    }
    gui_node_t* popup = scene__node_new(GUI_NODE_TYPE__POPUP);
    if (popup == NULL_POINTER)
    {
        return;
    }
    _popup_internal__state_t* st = _popup_internal__state_of(popup);
    if (st != NULL_POINTER)
    {
        st->kind          = kind;
        st->self_destruct = TRUE;
        if (kind == _POPUP_INTERNAL__KIND_CONFIRM)
        {
            _popup_internal__copy_str(st->ok_text,     sizeof(st->ok_text),     "Yes");
            _popup_internal__copy_str(st->cancel_text, sizeof(st->cancel_text), "No");
        }
        else
        {
            _popup_internal__copy_str(st->ok_text, sizeof(st->ok_text), "OK");
        }
    }
    // Copy message text into the popup node's text field.
    if (message != NULL_POINTER)
    {
        if (scene__set_text(popup, message, (int64)stdlib__strlen(message)) == FALSE)
        {
            scene__node_free(popup);
            return;
        }
    }
    if (handler_name != NULL_POINTER)
    {
        scene__set_on_change(popup, handler_name);
    }
    scene__add_child(root, popup);
    _popup_internal__open(popup);
}

/**
 * @brief imperative popup helpers. Both are wrappers around a shared internal popup implementation: they build a minimal popup node, install it as the scene overlay, and fire the supplied handler when the user dismisses it. Handler receives ev->popup.confirmed (TRUE for OK / Yes, FALSE for Cancel / No) and, for option-select, ev->popup.index.
 *
 * @param char* message -> Body text. Copied internally.
 * @param char* handler_name -> Registered handler name; NULL for fire-and-forget.
 *
 * @return void
 */
void scene__popup_ok(char* message, char* handler_name)
{
    _widget_popup__build_and_attach(message, handler_name, _POPUP_INTERNAL__KIND_OK);
}

/**
 * @brief yes/No confirmation popup. Pairs with scene__popup_ok; see above.
 *
 * @return void
 */
void scene__popup_confirm(char* message, char* handler_name)
{
    _widget_popup__build_and_attach(message, handler_name, _POPUP_INTERNAL__KIND_CONFIRM);
}
