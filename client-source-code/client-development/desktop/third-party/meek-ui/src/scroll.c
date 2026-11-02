// scroll.c - implementation of the shared scrollbar mechanics.
// Used by widget_div.c and widget_window.c. Pure CPU code -- the only
// renderer interaction is renderer__submit_rect from scroll__draw_vbar.
// Layout/scissor lifecycle is the caller's responsibility (see scroll.h).
// Conventions:
// - vbar = vertical scrollbar (the only kind currently drawn).
// - "track" = the full-height channel the thumb slides in.
// - "thumb" = the draggable element inside the track.
// - All rects are in pixel coords with a top-left origin.

#include "types.h"
#include "gui.h"
#include "scene.h"
#include "renderer.h"
#include "scroll.h"
#include "clib/stdlib.h"

// Visual defaults (used when style leaves the field at zero). Chosen
// to be readable against dark themes and still grabbable on a touch
// device.
static const float _SCROLL_INTERNAL__DEFAULT_BAR_SIZE  = 12.0f;
static const float _SCROLL_INTERNAL__DEFAULT_THUMB_MIN = 20.0f; // thumbs smaller than this are too tiny to drag.
// scroll-end-effect: wobble. The spring's response time when the style sets
// no scroll-smooth, and the longest time one call advances (a frame that
// follows a pause must not run the spring for the whole pause).
static const float _SCROLL_INTERNAL__WOBBLE_RESPONSE_MS = 160.0f;
static const float _SCROLL_INTERNAL__WOBBLE_STEP_MS     = 4.0f;
static const float _SCROLL_INTERNAL__WOBBLE_LONGEST_MS  = 100.0f;

static gui_rect_t _scroll_internal__thumb_for(gui_rect_t track, float view_h, float content_h, float scroll_y);
static gui_rect_t _scroll_internal__track_in(gui_node_t* node, gui_rect_t view, float scale);

/**
 * @brief fades a whole scroll item by its centre's proximity to the viewport edges
 * @param gui_node_t* node -> direct child whose descendants inherit this opacity
 * @param float scale -> logical-to-device pixel scale
 * @return float -> opacity between zero and one; one when fading is disabled
 */
float scroll__child_fade_alpha(gui_node_t* node, float scale)
{
    gui_node_t* parent = node != NULL_POINTER ? node->parent : NULL_POINTER;
    float distance = 0.0f;
    float centre = 0.0f;
    float fade = 0.0f;
    float alpha = 1.0f;

    if (parent == NULL_POINTER || parent->resolved.scroll_fade_px <= 0.0f || (parent->resolved.overflow_y != GUI_OVERFLOW__SCROLL && parent->resolved.overflow_y != GUI_OVERFLOW__AUTO))
    {
        return 1.0f;
    }
    fade = parent->resolved.scroll_fade_px * scale;
    if (fade <= 0.0f)
    {
        return 1.0f;
    }
    centre = node->bounds.y + node->bounds.h * 0.5f;
    distance = stdlib__fminf(centre - parent->bounds.y, parent->bounds.y + parent->bounds.h - centre);
    alpha = stdlib__fminf(1.0f, stdlib__fmaxf(0.0f, distance / fade));
    // Smooth endpoints while keeping the ramp entirely inside its requested band.
    return alpha * alpha * (3.0f - 2.0f * alpha);
}

boole scroll__smooth_step(float* position, float target, float response_ms, float ms_delta)
{
    float delta = target - *position;
    float elapsed = stdlib__fminf(100.0f, stdlib__fmaxf(0.0f, ms_delta));

    if (response_ms <= 0.0f || stdlib__fabsf(delta) < 0.5f)
    {
        *position = target;
        return FALSE;
    }
    *position += delta * (1.0f - stdlib__expf(-elapsed / response_ms));
    return TRUE;
}

// Test whether a particular axis should scroll given the user's
// overflow-mode declaration and the actual content-vs-bounds ratio.
static boole _scroll_internal__is_scroll_axis(gui_overflow_e mode, boole content_exceeds)
{
    if (mode == GUI_OVERFLOW__SCROLL)
    {
        return TRUE;
    }
    if (mode == GUI_OVERFLOW__AUTO)
    {
        return content_exceeds;
    }
    return FALSE;
}

/**
 * @brief resolve the scrollbar thickness in pixels, applying the style override if present and otherwise the toolkit default (~12 px scaled).
 *
 * @param gui_node_t* node -> The container node.
 * @param float scale -> Global UI scale factor.
 *
 * @return float -> Bar thickness in logical pixels.
 */
float scroll__bar_size(gui_node_t* node, float scale)
{
    gui_style_t* s = &node->resolved;
    return (s->scrollbar_size > 0.0f) ? s->scrollbar_size * scale : _SCROLL_INTERNAL__DEFAULT_BAR_SIZE * scale;
}

/**
 * @brief vertical scrollbar visible? Encodes the overflow_y rules: VISIBLE / HIDDEN  -> never visible. SCROLL            -> always visible. AUTO              -> visible iff content_h exceeds bounds.h.
 *
 * @param gui_node_t* node -> The container node (must have content_h + bounds populated by layout).
 *
 * @return boole -> TRUE if a vertical scrollbar should be drawn.
 */
boole scroll__vbar_visible(gui_node_t* node)
{
    boole exceeds = (node->content_h > node->bounds.h + 0.5f);
    return _scroll_internal__is_scroll_axis(node->resolved.overflow_y, exceeds);
}

/**
 * @brief horizontal scrollbar visible? Mirror of scroll__vbar_visible for the x axis. Encodes overflow_x and content_w vs bounds.w instead of the y equivalents. Both bars coexist when the container overflows on both axes; the bottom-right corner where they'd meet is occupied by the vertical bar (horizontal ends before it).
 *
 * @param gui_node_t* node -> The container node.
 *
 * @return boole -> TRUE if a horizontal scrollbar should be drawn.
 */
boole scroll__hbar_visible(gui_node_t* node)
{
    boole exceeds = (node->content_w > node->bounds.w + 0.5f);
    return _scroll_internal__is_scroll_axis(node->resolved.overflow_x, exceeds);
}

// Track rect: rightmost `bar_size` pixels of the container, full height.
// File-local because external callers should go through scroll__draw_vbar.
// If the HORIZONTAL bar is ALSO visible the vertical track's bottom end
// is shortened by bar_size so the corner where the two bars would meet
// stays clear (the h-bar gets the corner by symmetry with the visual
// convention).
static gui_rect_t _scroll_internal__vbar_track(gui_node_t* node, float bar_size)
{
    gui_rect_t r;
    r.x = node->bounds.x + node->bounds.w - bar_size;
    r.y = node->bounds.y;
    r.w = bar_size;
    r.h = node->bounds.h;
    if (scroll__hbar_visible(node))
    {
        r.h -= bar_size;
        if (r.h < 0.0f)
        {
            r.h = 0.0f;
        }
    }
    return r;
}

// Track rect for the horizontal bar. Bottom-most `bar_size` pixels, full
// width; if the vertical bar is also visible, the right end shortens by
// bar_size so the corner is left to the vbar.
static gui_rect_t _scroll_internal__hbar_track(gui_node_t* node, float bar_size)
{
    gui_rect_t r;
    r.x = node->bounds.x;
    r.y = node->bounds.y + node->bounds.h - bar_size;
    r.w = node->bounds.w;
    r.h = bar_size;
    if (scroll__vbar_visible(node))
    {
        r.w -= bar_size;
        if (r.w < 0.0f)
        {
            r.w = 0.0f;
        }
    }
    return r;
}

// Thumb rect within the track. Size = track_h * (view_h / content_h),
// clamped to [DEFAULT_THUMB_MIN, track_h]. Position scales with
// scroll_y / (content_h - view_h).
static gui_rect_t _scroll_internal__vbar_thumb(gui_node_t* node, gui_rect_t track)
{
    return _scroll_internal__thumb_for(track, node->bounds.h, node->content_h, node->scroll_y);
}

// The same thumb from explicit extents: a view of view_h showing content
// of content_h scrolled by scroll_y. A scroll_y outside [0, content_h -
// view_h] (content pulled past an edge) keeps the thumb at that end.
static gui_rect_t _scroll_internal__thumb_for(gui_rect_t track, float view_h, float content_h, float scroll_y)
{
    // Content fits entirely. We only get here when overflow_y == SCROLL
    // (AUTO would have skipped the bar via vbar_visible). Show a
    // full-height thumb so the bar still reads as a scrollbar even
    // though there's nothing to scroll.
    if (content_h <= view_h)
    {
        return track;
    }

    float ratio   = view_h / content_h;
    float thumb_h = track.h * ratio;
    if (thumb_h < _SCROLL_INTERNAL__DEFAULT_THUMB_MIN)
    {
        thumb_h = _SCROLL_INTERNAL__DEFAULT_THUMB_MIN;
    }
    if (thumb_h > track.h)
    {
        thumb_h = track.h;
    }

    float scroll_max = content_h - view_h;
    float travel     = track.h - thumb_h;
    float t          = (scroll_max > 0.0f) ? (scroll_y / scroll_max) : 0.0f;
    if (t < 0.0f)
    {
        t = 0.0f;
    }
    if (t > 1.0f)
    {
        t = 1.0f;
    }

    gui_rect_t r;
    r.x = track.x;
    r.y = track.y + travel * t;
    r.w = track.w;
    r.h = thumb_h;
    return r;
}

/**
 * @brief clamp scroll_y into [0, max(0, content_h - bounds.h)]. Called at the end of layout (so a content-shrink resets stale offsets) and during mid-drag updates (so the thumb can't fall off the end of the track).
 *
 * @param gui_node_t* node -> The container node.
 *
 * @return void
 */
void scroll__clamp(gui_node_t* node)
{
    // Intrinsic probes do not know the final viewport yet.
    if (scene__layout_is_measuring() == TRUE)
    {
        return;
    }
    float max_y = node->content_h - node->bounds.h;
    if (max_y < 0.0f)
    {
        max_y = 0.0f;
    }
    if (node->scroll_y < 0.0f)
    {
        node->scroll_y = 0.0f;
    }
    if (node->scroll_y > max_y)
    {
        node->scroll_y = max_y;
    }
    // Clamp the target too. content_h can shrink between frames (a
    // :disappear animation, a hot reload with fewer list items, a
    // display:none toggle on a child) which would leave
    // scroll_y_target pointing off the end if we didn't clamp here.
    // The animator would then eternally chase an out-of-range target
    // and the user would see scroll_y stuck at max_y instead of
    // rebounding to a valid position.
    if (node->scroll_y_target < 0.0f)
    {
        node->scroll_y_target = 0.0f;
    }
    if (node->scroll_y_target > max_y)
    {
        node->scroll_y_target = max_y;
    }

    // Same clamp for the x axis. content_w shrinking (hot reload drops
    // wide child, etc.) can leave scroll_x pointing off the right end.
    float max_x = node->content_w - node->bounds.w;
    if (max_x < 0.0f)
    {
        max_x = 0.0f;
    }
    if (node->scroll_x < 0.0f)
    {
        node->scroll_x = 0.0f;
    }
    if (node->scroll_x > max_x)
    {
        node->scroll_x = max_x;
    }
    if (node->scroll_x_target < 0.0f)
    {
        node->scroll_x_target = 0.0f;
    }
    if (node->scroll_x_target > max_x)
    {
        node->scroll_x_target = max_x;
    }
}

// Auto-hide alpha multiplier for the scrollbar. Returns 1.0 while
// the bar should be fully visible, 0.0 once it should be invisible,
// and a linear ramp between for the fade window. With overflow: auto
// scrollbars only appear when the user is interacting (or the
// smooth-scroll animator is mid-flight) and disappear after a short
// idle, the overlay-scrollbar convention; overflow: scroll asks for a
// permanent bar, as it does in CSS, so it stays at 1.0.
// Tuning rationale: HOLD_MS at 700 lets the user finish a flick
// and read where they landed before the bar vanishes. FADE_MS at
// 400 is short enough to feel snappy but long enough to read as
// a fade rather than a snap. Together they produce a 1.1 s "you
// can still grab me" window after the last activity.
static float _scroll_internal__bar_alpha(gui_node_t* node, gui_overflow_e mode)
{
    int64 now  = (int64)scene__frame_time_ms();
    int64 last = node->scroll_last_active_ms;
    if (mode == GUI_OVERFLOW__SCROLL)
    {
        return 1.0f;
    }
    if (last == 0)
    {
        return 0.0f;
    }
    int64 elapsed = now - last;
    if (elapsed < 0)
    {
        return 1.0f;
    }
    const int64 HOLD_MS = 700;
    const int64 FADE_MS = 400;
    if (elapsed < HOLD_MS)
    {
        return 1.0f;
    }
    int64 fade_elapsed = elapsed - HOLD_MS;
    if (fade_elapsed >= FADE_MS)
    {
        return 0.0f;
    }
    return 1.0f - ((float)fade_elapsed / (float)FADE_MS);
}

/**
 * @brief submit the vertical scrollbar's track + thumb rects via renderer__submit_rect. Caller is responsible for checking scroll__vbar_visible first and for managing scissor state (the bar itself sits inside the container's bounds so it's safe to draw inside or outside the scissor region).
 *
 * @param gui_node_t* node -> The container node.
 * @param float scale -> Global UI scale factor.
 *
 * @return void
 */
void scroll__draw_vbar(gui_node_t* node, float scale)
{
    gui_style_t* s = &node->resolved;
    float bar_size   = scroll__bar_size(node, scale);
    float bar_radius = (s->scrollbar_radius > 0.0f) ? s->scrollbar_radius * scale : bar_size * 0.5f;

    gui_rect_t track = _scroll_internal__vbar_track(node, bar_size);
    gui_rect_t thumb = _scroll_internal__vbar_thumb(node, track);

    // Defaults read against the standard dark theme: dim white track,
    // brighter white thumb. A .style override is the right move for any
    // real app that doesn't use the default theme.
    gui_color_t track_c = s->has_scrollbar_track ? s->scrollbar_track : scene__rgba(1.0f, 1.0f, 1.0f, 0.06f);
    gui_color_t thumb_c = s->has_scrollbar_thumb ? s->scrollbar_thumb : scene__rgba(1.0f, 1.0f, 1.0f, 0.30f);

    // Auto-hide: scale both rect alphas by the elapsed-since-last-
    // activity fade. alpha == 0 means "skip rendering entirely" so
    // we don't spend submit_rect calls on invisible quads.
    float bar_alpha = _scroll_internal__bar_alpha(node, node->resolved.overflow_y);
    if (bar_alpha <= 0.0f)
    {
        return;
    }
    track_c.a *= bar_alpha;
    thumb_c.a *= bar_alpha;

    renderer__submit_rect(track, track_c, bar_radius);
    renderer__submit_rect(thumb, thumb_c, bar_radius);
}

// Thumb rect for the horizontal bar. Size = track_w * (view_w / content_w);
// position scales with scroll_x / (content_w - view_w).
static gui_rect_t _scroll_internal__hbar_thumb(gui_node_t* node, gui_rect_t track)
{
    float content_w = node->content_w;
    float view_w    = node->bounds.w;
    // corner shared with vbar.
    if (scroll__vbar_visible(node))
    {
        view_w -= track.h;
    }

    if (content_w <= view_w)
    {
        return track;
    }

    float ratio   = view_w / content_w;
    float thumb_w = track.w * ratio;
    if (thumb_w < _SCROLL_INTERNAL__DEFAULT_THUMB_MIN)
    {
        thumb_w = _SCROLL_INTERNAL__DEFAULT_THUMB_MIN;
    }
    if (thumb_w > track.w)
    {
        thumb_w = track.w;
    }

    float scroll_max = content_w - view_w;
    float travel     = track.w - thumb_w;
    float t          = (scroll_max > 0.0f) ? (node->scroll_x / scroll_max) : 0.0f;
    if (t < 0.0f)
    {
        t = 0.0f;
    }
    if (t > 1.0f)
    {
        t = 1.0f;
    }

    gui_rect_t r;
    r.x = track.x + travel * t;
    r.y = track.y;
    r.w = thumb_w;
    r.h = track.h;
    return r;
}

/**
 * @brief submit the horizontal scrollbar's track + thumb rects. Mirror of scroll__draw_vbar for the x axis. If the vertical bar is ALSO visible, the horizontal bar's right end is shortened by scroll__bar_size to leave the corner clear for the vbar.
 *
 * @param gui_node_t* node -> The container node.
 * @param float scale -> Global UI scale factor.
 *
 * @return void
 */
void scroll__draw_hbar(gui_node_t* node, float scale)
{
    gui_style_t* s = &node->resolved;
    float bar_size   = scroll__bar_size(node, scale);
    float bar_radius = (s->scrollbar_radius > 0.0f) ? s->scrollbar_radius * scale : bar_size * 0.5f;

    gui_rect_t track = _scroll_internal__hbar_track(node, bar_size);
    gui_rect_t thumb = _scroll_internal__hbar_thumb(node, track);

    gui_color_t track_c = s->has_scrollbar_track ? s->scrollbar_track : scene__rgba(1.0f, 1.0f, 1.0f, 0.06f);
    gui_color_t thumb_c = s->has_scrollbar_thumb ? s->scrollbar_thumb : scene__rgba(1.0f, 1.0f, 1.0f, 0.30f);

    // Auto-hide -- same shared helper the vertical bar uses. Both
    // axes fade in lockstep with the SAME last-active timestamp,
    // which is the correct behaviour: any scroll input on either
    // axis lights up both bars (they're a single visual unit).
    float bar_alpha = _scroll_internal__bar_alpha(node, node->resolved.overflow_x);
    if (bar_alpha <= 0.0f)
    {
        return;
    }
    track_c.a *= bar_alpha;
    thumb_c.a *= bar_alpha;

    renderer__submit_rect(track, track_c, bar_radius);
    renderer__submit_rect(thumb, thumb_c, bar_radius);
}

// Hit-test the cursor against the vertical or horizontal thumb. File-
// local; on_mouse_down is the public entry point.
static boole _scroll_internal__hit_vthumb(gui_node_t* node, int64 x, int64 y, float scale)
{
    if (scroll__vbar_visible(node) == FALSE)
    {
        return FALSE;
    }
    float bar_size = scroll__bar_size(node, scale);
    gui_rect_t track = _scroll_internal__vbar_track(node, bar_size);
    gui_rect_t thumb = _scroll_internal__vbar_thumb(node, track);
    float fx = (float)x;
    float fy = (float)y;
    return (boole)(fx >= thumb.x && fx < thumb.x + thumb.w && fy >= thumb.y && fy < thumb.y + thumb.h);
}

static boole _scroll_internal__hit_hthumb(gui_node_t* node, int64 x, int64 y, float scale)
{
    if (scroll__hbar_visible(node) == FALSE)
    {
        return FALSE;
    }
    float bar_size = scroll__bar_size(node, scale);
    gui_rect_t track = _scroll_internal__hbar_track(node, bar_size);
    gui_rect_t thumb = _scroll_internal__hbar_thumb(node, track);
    float fx = (float)x;
    float fy = (float)y;
    return (boole)(fx >= thumb.x && fx < thumb.x + thumb.w && fy >= thumb.y && fy < thumb.y + thumb.h);
}

/**
 * @brief mouse-down hook for a scrollable container. If the cursor lands on the scrollbar thumb, records drag-start state on the node so a subsequent on_mouse_drag converts mouse delta into scroll delta.
 *
 * @param gui_node_t* node -> The container node.
 * @param int64 x -> Cursor x.
 * @param int64 y -> Cursor y.
 * @param float scale -> Global UI scale factor.
 *
 * @return void
 */
void scroll__on_mouse_down(gui_node_t* node, int64 x, int64 y, float scale)
{
    // Vertical thumb wins if both happen to overlap (corner overlap is
    // prevented by the cross-aware track sizes above, but the thumbs
    // are the part the user aims at). Two drag directions share one
    // axis field, so we pick one.
    if (_scroll_internal__hit_vthumb(node, x, y, scale))
    {
        node->scroll_drag_axis         = 1; // y.
        node->scroll_drag_mouse_start  = (float)y;
        node->scroll_drag_scroll_start = node->scroll_y;
    }
    else if (_scroll_internal__hit_hthumb(node, x, y, scale))
    {
        node->scroll_drag_axis         = 2; // x.
        node->scroll_drag_mouse_start  = (float)x;
        node->scroll_drag_scroll_start = node->scroll_x;
    }
    else
    {
        // Click landed elsewhere in the container. Ensure no stale drag
        // state survives -- on_mouse_drag checks scroll_drag_axis and
        // no-ops when zero.
        node->scroll_drag_axis = 0;
    }
}

/**
 * @brief mouse-drag hook. If a thumb drag is in progress, applies the delta to scroll_y, clamps, and returns TRUE so the caller swallows the event (no hover updates while scrolling). Otherwise returns FALSE so normal hover semantics continue.
 *
 * @param gui_node_t* node -> The container node.
 * @param int64 y -> Cursor y at the drag event.
 * @param float scale -> Global UI scale factor.
 *
 * @return boole -> TRUE if the event was consumed by an active scrollbar drag.
 */
boole scroll__on_mouse_drag(gui_node_t* node, int64 y, float scale)
{
    // Caller passes both x and y through node->scroll_drag_mouse_start
    // vs current (x or y). The function signature only has a single
    // integer so we interpret it based on scroll_drag_axis: 1 = y, 2 =
    // x. That matches widget_div's delegate which passes `y` for y-axis
    // drags; we rely on the x-axis case being uncommon enough that the
    // caller can wrap via a helper if needed. See scroll__on_mouse_drag_xy
    // for an explicit-x variant.
    if (node->scroll_drag_axis == 1)
    {
        float bar_size = scroll__bar_size(node, scale);
        gui_rect_t track = _scroll_internal__vbar_track(node, bar_size);
        gui_rect_t thumb = _scroll_internal__vbar_thumb(node, track);

        float scroll_max = node->content_h - node->bounds.h;
        float travel     = track.h - thumb.h;
        if (scroll_max <= 0.0f || travel <= 0.0f)
        {
            return TRUE;
        }

        float mouse_delta  = (float)y - node->scroll_drag_mouse_start;
        float scroll_delta = mouse_delta * (scroll_max / travel);

        // Drag is a direct-manipulation gesture -- the thumb must track
        // the cursor 1:1, no lerp. So we write BOTH scroll_y AND
        // scroll_y_target, which keeps the animator's smooth-scroll
        // interpolator at rest (delta between them is zero) while the
        // drag is in progress. Wheel input is the opposite: it writes
        // ONLY scroll_y_target and lets the animator catch up. That split
        // matches CSS's `scroll-behavior: smooth` semantics -- programmatic
        // scrolls smooth; direct-manipulation gestures don't.
        node->scroll_y        = node->scroll_drag_scroll_start + scroll_delta;
        node->scroll_y_target = node->scroll_y;
        node->scroll_last_active_ms = (int64)scene__frame_time_ms();
        scroll__clamp(node);
        return TRUE;
    }
    if (node->scroll_drag_axis == 2)
    {
        // Horizontal drag. The single-arg `y` signature is a mild lie
        // here -- the caller actually forwards the cursor's X. Widget
        // hosts that want to support both axes route the drag through
        // on_mouse_drag with the right coordinate based on the active
        // axis (see widget_div.c's forward).
        float bar_size = scroll__bar_size(node, scale);
        gui_rect_t track = _scroll_internal__hbar_track(node, bar_size);
        gui_rect_t thumb = _scroll_internal__hbar_thumb(node, track);

        float scroll_max = node->content_w - node->bounds.w;
        float travel     = track.w - thumb.w;
        if (scroll_max <= 0.0f || travel <= 0.0f)
        {
            return TRUE;
        }

        float mouse_delta  = (float)y - node->scroll_drag_mouse_start;
        float scroll_delta = mouse_delta * (scroll_max / travel);

        node->scroll_x        = node->scroll_drag_scroll_start + scroll_delta;
        node->scroll_x_target = node->scroll_x;
        node->scroll_last_active_ms = (int64)scene__frame_time_ms();
        scroll__clamp(node);
        return TRUE;
    }
    return FALSE;
}

/**
 * @brief mouse-up hook. Clears any active drag state on the node.
 *
 * @param gui_node_t* node -> The container node.
 *
 * @return void
 */
void scroll__on_mouse_up(gui_node_t* node)
{
    node->scroll_drag_axis = 0;
}

/**
 * @brief whether the vertical rubber band is on (overscroll-behavior: auto or contain on the container)
 * @param gui_node_t* node -> the container
 * @return boole -> TRUE when the content may be pulled past its edges
 */
boole scroll__overscroll_enabled_y(gui_node_t* node)
{
    return node->resolved.overscroll_y;
}

/**
 * @brief the farthest the content can be pulled past an edge (overscroll-limit; half the box by default)
 * @param gui_node_t* node -> the container
 * @return float -> device pixels, at least one
 */
float scroll__overscroll_limit_y(gui_node_t* node)
{
    const gui_style_t* r = &node->resolved;
    float limit = 0.0f;

    if (r->overscroll_limit_pct > 0.0f)
    {
        limit = node->bounds.h * r->overscroll_limit_pct / 100.0f;
    }
    else if (r->overscroll_limit_px > 0.0f)
    {
        limit = r->overscroll_limit_px * scene__scale();
    }
    else
    {
        limit = node->bounds.h * 0.5f;
    }
    return (limit < 1.0f) ? 1.0f : limit;
}

/**
 * @brief pull the content past an edge: a movement outward counts with the resistance diminishing to nothing at the limit, a movement back with the plain resistance (overscroll-resistance)
 * @param gui_node_t* node -> the container
 * @param float excess -> the movement in device pixels, negative past the start, positive past the end
 * @return void
 */
void scroll__overscroll_push_y(gui_node_t* node, float excess)
{
    float limit = scroll__overscroll_limit_y(node);
    float resistance = (node->resolved.overscroll_resistance > 0.0f) ? node->resolved.overscroll_resistance : 0.55f;
    float fraction = stdlib__fabsf(node->overscroll_y) / limit;
    float factor = resistance;
    boole outward = (boole)(node->overscroll_y == 0.0f || (node->overscroll_y > 0.0f) == (excess > 0.0f));

    if (resistance > 1.0f)
    {
        resistance = 1.0f;
    }
    if (outward == TRUE)
    {
        factor = resistance * (1.0f - ((fraction > 1.0f) ? 1.0f : fraction));
    }
    node->overscroll_y += excess * factor;
    if (node->overscroll_y > limit)
    {
        node->overscroll_y = limit;
    }
    if (node->overscroll_y < -limit)
    {
        node->overscroll_y = -limit;
    }
    node->overscroll_y_returning = FALSE;
    node->scroll_last_active_ms = (int64)scene__frame_time_ms();
}

/**
 * @brief let go of the content: the spring back to the edge starts (overscroll-duration, overscroll-timing-function); nothing happens at the edge itself
 * @param gui_node_t* node -> the container
 * @return void
 */
void scroll__overscroll_release_y(gui_node_t* node)
{
    node->overscroll_y_held = FALSE;
    node->overscroll_y_fling = FALSE;
    if (node->overscroll_y != 0.0f)
    {
        node->overscroll_y_from = node->overscroll_y;
        node->overscroll_y_age_ms = 0.0f;
        node->overscroll_y_returning = TRUE;
        // no fling out of a pulled state: the spring owns the motion
        node->scroll_y_velocity = 0.0f;
        node->scroll_last_active_ms = (int64)scene__frame_time_ms();
    }
}

// ===== a scrollbar for a view that is not the node's own box ================

// Track of such a view: its rightmost bar_size pixels, full height.
static gui_rect_t _scroll_internal__track_in(gui_node_t* node, gui_rect_t view, float scale)
{
    gui_rect_t track;
    float bar_size = 0.0f;

    stdlib__null_memory(&track, sizeof(track));
    bar_size = scroll__bar_size(node, scale);
    if (bar_size > view.w)
    {
        bar_size = view.w;
    }
    track.x = view.x + view.w - bar_size;
    track.y = view.y;
    track.w = bar_size;
    track.h = view.h;
    return track;
}

/**
 * @brief the thumb of the scrollbar along the right edge of a view
 * @param gui_node_t* node -> the node whose scrollbar-* style applies
 * @param gui_rect_t view -> the visible rectangle, device pixels
 * @param float content_h -> height of all the content
 * @param float scroll_y -> how far the content is scrolled
 * @param float scale -> global UI scale
 * @return gui_rect_t -> the thumb, device pixels
 */
gui_rect_t scroll__vbar_thumb_in(gui_node_t* node, gui_rect_t view, float content_h, float scroll_y, float scale)
{
    return _scroll_internal__thumb_for(_scroll_internal__track_in(node, view, scale), view.h, content_h, scroll_y);
}

/**
 * @brief how far the content moves for one device pixel of thumb travel
 * @param gui_node_t* node -> the node whose scrollbar-* style applies
 * @param gui_rect_t view -> the visible rectangle
 * @param float content_h -> height of all the content
 * @param float scale -> global UI scale
 * @return float -> content pixels per thumb pixel, 0 when nothing scrolls
 */
float scroll__scroll_per_pixel_in(gui_node_t* node, gui_rect_t view, float content_h, float scale)
{
    gui_rect_t track;
    gui_rect_t thumb;
    float travel = 0.0f;

    stdlib__null_memory(&track, sizeof(track));
    stdlib__null_memory(&thumb, sizeof(thumb));
    if (content_h <= view.h)
    {
        return 0.0f;
    }
    track = _scroll_internal__track_in(node, view, scale);
    thumb = _scroll_internal__thumb_for(track, view.h, content_h, 0.0f);
    travel = track.h - thumb.h;
    if (travel <= 0.0f)
    {
        return 0.0f;
    }
    return (content_h - view.h) / travel;
}

/**
 * @brief draw the track and the thumb of such a view with the node's scrollbar style
 * @param gui_node_t* node -> the node whose scrollbar-* style applies
 * @param gui_rect_t view -> the visible rectangle
 * @param float content_h -> height of all the content
 * @param float scroll_y -> how far the content is scrolled
 * @param float alpha -> multiplies the alpha of both colours
 * @param float scale -> global UI scale
 * @return void
 */
void scroll__draw_vbar_in(gui_node_t* node, gui_rect_t view, float content_h, float scroll_y, float alpha, float scale)
{
    gui_style_t* s = &node->resolved;
    gui_rect_t track;
    gui_rect_t thumb;
    gui_color_t track_color;
    gui_color_t thumb_color;
    float radius = 0.0f;

    stdlib__null_memory(&track, sizeof(track));
    stdlib__null_memory(&thumb, sizeof(thumb));
    stdlib__null_memory(&track_color, sizeof(track_color));
    stdlib__null_memory(&thumb_color, sizeof(thumb_color));
    if (alpha <= 0.0f || content_h <= view.h)
    {
        return;
    }
    track = _scroll_internal__track_in(node, view, scale);
    thumb = _scroll_internal__thumb_for(track, view.h, content_h, scroll_y);
    radius = (s->scrollbar_radius > 0.0f) ? s->scrollbar_radius * scale : track.w * 0.5f;
    // the same defaults as scroll__draw_vbar
    track_color = (s->has_scrollbar_track == TRUE) ? s->scrollbar_track : scene__rgba(1.0f, 1.0f, 1.0f, 0.06f);
    thumb_color = (s->has_scrollbar_thumb == TRUE) ? s->scrollbar_thumb : scene__rgba(1.0f, 1.0f, 1.0f, 0.30f);
    track_color.a *= alpha;
    thumb_color.a *= alpha;
    renderer__submit_rect(track, track_color, radius);
    renderer__submit_rect(thumb, thumb_color, radius);
}

// ===== scroll-end-effect: wobble =================================================

/**
 * @brief whether the style asks for the wobble with an amount above zero
 * @param const gui_style_t* style -> resolved style of the scrolling node
 * @return boole -> TRUE when the spring moves the content
 */
boole scroll__wobble_enabled(const gui_style_t* style)
{
    return (boole)(style->scroll_end_effect == GUI_SCROLL_END_EFFECT__WOBBLE && style->scroll_end_effect_amount > 0.0f);
}

/**
 * @brief whether the scroll position moves to its target over time
 * @param const gui_style_t* style -> resolved style of the scrolling node
 * @return boole -> TRUE with scroll-smooth or the wobble; input then writes the target only
 */
boole scroll__animated(const gui_style_t* style)
{
    return (boole)(style->scroll_smooth_ms > 0.0f || scroll__wobble_enabled(style) == TRUE);
}

/**
 * @brief advance the spring that moves a scroll position to its target
 *
 * The spring is x'' = -w^2 (x - target) - 2 z w x'. w is 2 / response, with
 * the response time taken from scroll-smooth (160 ms when unset). z, the
 * damping ratio, is 1 / (1 + 3 * amount): 1 at amount 0 (the position does
 * not pass the target), 0.4 at 0.5, 0.25 at 1 (it passes the target by about
 * 44 % of the step). Integrated in steps of at most 4 ms, velocity first,
 * which keeps the energy bounded at these step sizes.
 *
 * @param const gui_style_t* style -> resolved style of the scrolling node
 * @param float* position -> scroll position, device pixels
 * @param float* velocity -> its speed, device pixels per millisecond
 * @param float target -> where the position comes to rest
 * @param float ms_delta -> time to advance
 * @return boole -> TRUE while the spring has not come to rest
 */
boole scroll__wobble_step(const gui_style_t* style, float* position, float* velocity, float target, float ms_delta)
{
    float response = 0.0f;
    float omega = 0.0f;
    float zeta = 0.0f;
    float remaining = 0.0f;
    float step = 0.0f;
    float acceleration = 0.0f;

    response = (style->scroll_smooth_ms > 0.0f) ? style->scroll_smooth_ms : _SCROLL_INTERNAL__WOBBLE_RESPONSE_MS;
    omega = 2.0f / response;
    zeta = 1.0f / (1.0f + 3.0f * style->scroll_end_effect_amount);
    remaining = (ms_delta > _SCROLL_INTERNAL__WOBBLE_LONGEST_MS) ? _SCROLL_INTERNAL__WOBBLE_LONGEST_MS : ms_delta;
    while (remaining > 0.0f)
    {
        step = (remaining > _SCROLL_INTERNAL__WOBBLE_STEP_MS) ? _SCROLL_INTERNAL__WOBBLE_STEP_MS : remaining;
        acceleration = -omega * omega * (*position - target) - 2.0f * zeta * omega * (*velocity);
        *velocity += acceleration * step;
        *position += (*velocity) * step;
        remaining -= step;
    }
    if (stdlib__fabsf(*position - target) < 0.25f && stdlib__fabsf(*velocity) < 0.005f)
    {
        *position = target;
        *velocity = 0.0f;
        return FALSE;
    }
    return TRUE;
}
