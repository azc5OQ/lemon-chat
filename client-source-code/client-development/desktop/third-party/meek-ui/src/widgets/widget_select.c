// widget_select.c - the select control in its styles (select-style).
//
// STYLES
//   list, wheel, sheet, page, menu, dialog and search open a panel over the
//   page (the scene's overlay slot: drawn after the page, pointer input
//   before it). segmented, chips and carousel draw the options inside the
//   control and open nothing. docs/select-styles.md describes every style
//   and the properties it reads.
//
// COST PER FRAME
//   Every frame and every input event fills one metrics record
//   (_widget_select_internal__metrics: one walk over the options to count
//   them, one more for the styles that measure or filter their text) and
//   then works on the rows that can be visible. No function walks the option
//   list once per row, so a list of 300 options costs the same per frame as
//   a list of 16.
//
// REVEAL CLOCK
//   widget_anim runs from 0 to 1 over total = popup-duration + (rows that
//   fit into the list - 1) * popup-stagger. Options that do not fit take no
//   time: they are outside the list while it opens. The backdrop (blur, dim)
//   and the panel fade with widget_anim, so they take the same time as the
//   rows that are visible. The reveal order counts from the first row in
//   view at the time of opening, closing freezes row positions and fades the whole popup over popup-duration.
//
// SCROLLING
//   The mouse wheel, a drag, the scrollbar and the keys write scroll_target;
//   the shown offset follows at once, over scroll-smooth, or on the spring
//   of scroll-end-effect: wobble. wheel, carousel and the mark of a segmented
//   control move over 120 ms when the style sets neither.
//
// VALUE
//   A style changes the value and calls on_change from an input handler
//   only, never while drawing: list, sheet, page, menu and search when a row
//   is clicked, dialog when its confirm button is, wheel and carousel when
//   the input decides which option the turn ends on (the turn itself is
//   drawn afterwards), segmented and chips when an option is clicked.
#include "widget_select.h"
#include "../motion.h"

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "scroll.h"
#include "renderer.h"
#include "font.h"
#include "gradient.h"
#include "clib/stdlib.h"
#include "clib/memory_manager.h"
#include "memory_types.h"

// Logical pixels unless the name says otherwise.
#define _WIDGET_SELECT_INTERNAL__WHEEL_TICK_ROWS 3.0f    // rows one wheel tick moves a list by (the three lines of a desktop list)
#define _WIDGET_SELECT_INTERNAL__CHIPS_TICK 120.0f       // what one wheel tick moves a row of chips by
#define _WIDGET_SELECT_INTERNAL__VISIBLE_CAP 256         // rows drawn in one frame at most
#define _WIDGET_SELECT_INTERNAL__TEXT_CAP 64             // title, button and placeholder text
#define _WIDGET_SELECT_INTERNAL__QUERY_CAP 64            // the text of a search
#define _WIDGET_SELECT_INTERNAL__WHEEL_END 1.2217f       // radians (70 degrees): the angle at which a wheel's row leaves the view
#define _WIDGET_SELECT_INTERNAL__HALF_PI 1.5707963f
#define _WIDGET_SELECT_INTERNAL__BAR_H 48.0f             // title bar of page and dialog, button strip of dialog
#define _WIDGET_SELECT_INTERNAL__HANDLE_H 20.0f          // the strip with the handle of a sheet
#define _WIDGET_SELECT_INTERNAL__SHEET_TITLE_H 36.0f
#define _WIDGET_SELECT_INTERNAL__SHEET_PART 0.6f         // the most of the viewport's height the rows of a sheet take
#define _WIDGET_SELECT_INTERNAL__SHEET_CLOSE 0.25f       // a sheet pulled down by this part of its height closes on release
#define _WIDGET_SELECT_INTERNAL__DIALOG_W 300.0f
#define _WIDGET_SELECT_INTERNAL__DIALOG_MARGIN 24.0f     // free space kept around a dialog
#define _WIDGET_SELECT_INTERNAL__DIALOG_TOP 12.0f        // above the rows of a dialog without a title
#define _WIDGET_SELECT_INTERNAL__BUTTON_H 36.0f
#define _WIDGET_SELECT_INTERNAL__BUTTON_PAD 16.0f
#define _WIDGET_SELECT_INTERNAL__BUTTON_GAP 8.0f
#define _WIDGET_SELECT_INTERNAL__MARK 16.0f              // check mark, radio mark
#define _WIDGET_SELECT_INTERNAL__MARK_GAP 10.0f          // between a mark and the text
#define _WIDGET_SELECT_INTERNAL__DRAG_START 6.0f         // a press that moves farther than this is a drag, not a click
#define _WIDGET_SELECT_INTERNAL__TURN_MS 120.0f          // wheel, carousel, segmented mark: time to the target when the style sets no scroll-smooth
#define _WIDGET_SELECT_INTERNAL__FLING_MS 180.0f         // a released drag carries on for its speed times this
#define _WIDGET_SELECT_INTERNAL__CHIP_GAP 8.0f

// Runtime state of one select; the appearance comes from the style.
typedef struct _widget_select_internal__state_t
{
    // the offset along the style's axis, device pixels: down a list, around a wheel, along chips and a carousel, the mark of a segmented control
    float scroll;          // shown; outside [0, max] while a spring swings past an edge
    float scroll_target;   // where it comes to rest, inside [0, max]
    float scroll_velocity; // device pixels per millisecond
    int64 scroll_ms;       // frame time of the last step, without float uptime rounding
    int64 reveal_ms;       // frame time of the last popup fade step
    float scroll_far;      // first-order follower: stretch relaxes without oscillation
    float stretch_anchor;  // pointer along the scroll axis, in device pixels
    boole stretch_anchored;
    float presence;        // closing fades the whole popup, without reversing its reveal
    int64 stagger_first;   // the row the reveal order counts from
    // the scrollbar
    boole thumb_held;      // the thumb follows the pointer
    float thumb_pointer;   // pointer y when the thumb was taken
    float thumb_scroll;    // scroll_target when the thumb was taken
    boole press_on_bar;    // the press began on the scrollbar: its release selects nothing
    // a drag of the content
    boole drag_held;       // the press began on the options
    boole drag_moved;      // and moved farther than DRAG_START: its release selects nothing
    float drag_pointer;    // pointer coordinate at the press
    float drag_scroll;     // scroll_target at the press
    float drag_last;       // pointer coordinate of the last movement
    int64 drag_last_ms;    // and its frame time
    float drag_speed;      // smoothed speed of the pointer, device pixels per millisecond
    // a sheet pulled down by its handle
    boole sheet_held;
    float sheet_pointer;   // pointer y when the handle was taken
    float sheet_pull;      // how far the sheet is pulled below its place, device pixels
    // the options the input points at
    int64 pending;         // dialog: the option marked before the confirm button takes it
    int64 highlight;       // the row the arrow keys are on, -1 for none
    // search
    char  query[_WIDGET_SELECT_INTERNAL__QUERY_CAP];
    int64 query_len;
    int*  matches;         // the options that contain the query, ascending
    int64 match_count;
    int64 match_capacity;
    // text from the node's attributes
    char  title[_WIDGET_SELECT_INTERNAL__TEXT_CAP];
    char  confirm[_WIDGET_SELECT_INTERNAL__TEXT_CAP];
    char  cancel[_WIDGET_SELECT_INTERNAL__TEXT_CAP];
    char  placeholder[_WIDGET_SELECT_INTERNAL__TEXT_CAP];
    boole blur_prepared;   // one pixel was blurred in a frame of the closed control
} _widget_select_internal__state_t;

// Diagnostic: the option nodes this file's loops have stepped over. The
// checks read it to show that the work of a frame grows with the number of
// options and not with its square (scene__select_walked).
static int64 _widget_select_internal__walked = 0;

// Everything the options are placed with, derived once per frame or event.
typedef struct _widget_select_internal__metrics_t
{
    gui_select_style_e style;
    gui_rect_t rest;        // the panel at rest, after its entrance
    gui_rect_t panel;       // the panel as drawn now
    gui_rect_t rows;        // where the options are drawn and clipped
    gui_rect_t header;      // zero height when the style has none
    gui_rect_t footer;
    gui_rect_t confirm;     // dialog
    gui_rect_t cancel;      // dialog
    gui_rect_t back;        // page
    gui_rect_t bar;         // the scrollbar's track; zero width when the rows do not scroll
    float scale;
    float view_w;
    float view_h;
    float row_height;       // one row of a list or a wheel; one item of a carousel or a segmented control along x
    float row_width;        // the rows' width without the scrollbar
    float content;          // all the rows along the axis
    float max_scroll;
    float scroll;           // shown offset
    float velocity;
    float duration;         // popup-duration, ms
    float stagger;          // popup-stagger, ms; 0 for popup-reveal: fade
    float total;            // ms of the whole reveal
    float clock;            // ms into the reveal
    float enter;            // eased 0..1: the entrance of the panel
    float travel;           // how far a row is from its place before it reveals: negative above, positive below
    float stretch_lag;
    float stretch_anchor;
    float stretch_reach;
    float presence;
    float reach;            // the farthest a row is drawn from its resting place
    float wheel_step;       // wheel: the angle between two rows, radians
    float wheel_radius;     // wheel: device pixels
    int64 count;            // options
    int64 rows_count;       // rows: the options, or the matches of a search
    int64 visible;          // wheel, carousel: select-rows as an odd number from 3
    int64 stagger_rows;     // rows that take part in the reveal order
    int64 stagger_first;
    gui_popup_reveal_e reveal;
    boole open;
    boole filtered;         // the rows are the matches of a search
} _widget_select_internal__metrics_t;

static _widget_select_internal__state_t* _widget_select_internal__state(gui_node_t* n);
static float       _widget_select_internal__stretch_point(const _widget_select_internal__metrics_t* m, float point, boole inverse);
static void        _widget_select_internal__stretch_rect(const _widget_select_internal__metrics_t* m, gui_rect_t* rect);
static void        _widget_select_internal__stretch_metrics(gui_node_t* n, _widget_select_internal__metrics_t* m);
static boole       _widget_select_internal__stretch_step(gui_node_t* n, const _widget_select_internal__metrics_t* m, float elapsed);
static boole       _widget_select_internal__is_inline(gui_select_style_e style);
static boole       _widget_select_internal__is_turning(gui_select_style_e style);
static boole       _widget_select_internal__is_horizontal(gui_select_style_e style);
static float       _widget_select_internal__row_height(gui_node_t* n, float scale);
static float       _widget_select_internal__duration(gui_node_t* n);
static float       _widget_select_internal__smooth(gui_node_t* n);
static boole       _widget_select_internal__contains(gui_rect_t r, float x, float y);
static float       _widget_select_internal__clamp(float value, float low, float high);
static float       _widget_select_internal__round(float value);
static boole       _widget_select_internal__text_contains(const char* text, const char* query);
static void        _widget_select_internal__copy_text(char* out, const char* text);
static gui_color_t _widget_select_internal__text_color(gui_style_t* s);
static gui_color_t _widget_select_internal__accent_color(gui_style_t* s);
static gui_color_t _widget_select_internal__separator_color(gui_style_t* s);
static gui_color_t _widget_select_internal__panel_color(gui_style_t* s);
static gui_color_t _widget_select_internal__mix(gui_color_t a, gui_color_t b, float t);
static gui_font_t* _widget_select_internal__font(gui_node_t* n, float scale);
static int64       _widget_select_internal__option_count(gui_node_t* select);
static gui_node_t* _widget_select_internal__option_at(gui_node_t* select, int64 index);
static float       _widget_select_internal__widest_option(gui_node_t* n, gui_font_t* font);
static void        _widget_select_internal__filter(gui_node_t* n, _widget_select_internal__state_t* state, int64 count);
static int64       _widget_select_internal__option_of_row(const _widget_select_internal__metrics_t* m, const _widget_select_internal__state_t* state, int64 row);
static int64       _widget_select_internal__row_of_option(const _widget_select_internal__metrics_t* m, const _widget_select_internal__state_t* state, int64 option);
static void        _widget_select_internal__options_of_rows(gui_node_t* n, const _widget_select_internal__metrics_t* m, const _widget_select_internal__state_t* state, int64 first, int64 last, gui_node_t** out);
static void        _widget_select_internal__settle(gui_node_t* n, _widget_select_internal__state_t* state, float max_scroll);
static gui_rect_t  _widget_select_internal__place(gui_node_t* n, float w, float h, float scale);
static void        _widget_select_internal__metrics_stacked(gui_node_t* n, _widget_select_internal__metrics_t* m);
static void        _widget_select_internal__metrics_wheel(gui_node_t* n, _widget_select_internal__metrics_t* m);
static void        _widget_select_internal__metrics_inline(gui_node_t* n, _widget_select_internal__metrics_t* m);
static void        _widget_select_internal__metrics(gui_node_t* n, _widget_select_internal__metrics_t* m);
static boole       _widget_select_internal__row(const _widget_select_internal__metrics_t* m, int64 row, gui_rect_t* bounds, float* alpha);
static boole       _widget_select_internal__wheel_row(const _widget_select_internal__metrics_t* m, int64 row, gui_rect_t* bounds, float* alpha, float* nearness);
static boole       _widget_select_internal__carousel_item(gui_node_t* n, const _widget_select_internal__metrics_t* m, int64 index, gui_rect_t* bounds, float* alpha, float* nearness);
static boole       _widget_select_internal__chip(gui_node_t* n, const _widget_select_internal__metrics_t* m, gui_font_t* font, int64 index, gui_rect_t* bounds);
static int64       _widget_select_internal__row_at(const _widget_select_internal__metrics_t* m, float x, float y);
static int64       _widget_select_internal__inline_at(gui_node_t* n, const _widget_select_internal__metrics_t* m, float x, float y);
static void        _widget_select_internal__rows_in_view(const _widget_select_internal__metrics_t* m, int64* first, int64* last);
static int64       _widget_select_internal__first_in_view(const _widget_select_internal__metrics_t* m, float scroll);
static boole       _widget_select_internal__tick(gui_node_t* n, _widget_select_internal__metrics_t* m);
static boole       _widget_select_internal__scroll_step(gui_node_t* n, _widget_select_internal__metrics_t* m);
static void        _widget_select_internal__scroll_by(gui_node_t* n, const _widget_select_internal__metrics_t* m, float pixels);
static void        _widget_select_internal__scroll_to_row(gui_node_t* n, const _widget_select_internal__metrics_t* m, int64 row);
static void        _widget_select_internal__show_current(gui_node_t* n);
static void        _widget_select_internal__commit(gui_node_t* n, int64 option, boole close);
static void        _widget_select_internal__turn_to(gui_node_t* n, const _widget_select_internal__metrics_t* m, int64 option);
static void        _widget_select_internal__close_pulled_sheet(gui_node_t* n, const _widget_select_internal__metrics_t* m);
static void        _widget_select_internal__sync_label(gui_node_t* n);
static void        _widget_select_internal__draw_text(gui_node_t* n, gui_font_t* font, float scale, float x, float y, float h, gui_color_t color, const char* text);
static void        _widget_select_internal__draw_check(float x, float y, float size, float thickness, gui_color_t color);
static void        _widget_select_internal__draw_header(gui_node_t* n, const _widget_select_internal__metrics_t* m, gui_font_t* font, float alpha);
static void        _widget_select_internal__draw_footer(gui_node_t* n, const _widget_select_internal__metrics_t* m, gui_font_t* font, float alpha);
static void        _widget_select_internal__option_states(gui_node_t* n, const _widget_select_internal__metrics_t* m);
static void        _widget_select_internal__option_border(gui_node_t* option, gui_rect_t row, float scale);
static void        _widget_select_internal__border_segment(gui_vec2_t a, gui_vec2_t b, gui_vec2_t c, gui_vec2_t d, gui_rect_t row, gui_color_t color, const gradient_sampler_t* gradient, float opacity);
static void        _widget_select_internal__option_bg(gui_node_t* n, gui_node_t* option, gui_rect_t row, float alpha, gui_color_t fallback, float scale);
static gui_color_t _widget_select_internal__option_color(gui_node_t* n, gui_node_t* option, gui_color_t fallback, float alpha);
static void        _widget_select_internal__draw_rows(gui_node_t* n, const _widget_select_internal__metrics_t* m, gui_font_t* font);
static void        _widget_select_internal__draw_wheel(gui_node_t* n, const _widget_select_internal__metrics_t* m, gui_font_t* font);
static void        _widget_select_internal__draw_popup(gui_node_t* n, float scale);
static void        _widget_select_internal__draw_inline(gui_node_t* n, float scale);
static void        _widget_select_internal__draw_closed(gui_node_t* n, float scale);
static void        _widget_select_internal__init_defaults(gui_node_t* n);
static boole       _widget_select_internal__apply_attribute(gui_node_t* n, char* name, char* value);
static void        _widget_select_internal__layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale);
static void        _widget_select_internal__emit_draws(gui_node_t* n, float scale);
static void        _widget_select_internal__on_mouse_down(gui_node_t* n, int64 x, int64 y, int64 button);
static boole       _widget_select_internal__on_mouse_drag(gui_node_t* n, int64 x, int64 y);
static void        _widget_select_internal__on_mouse_up(gui_node_t* n, int64 x, int64 y, int64 button);
static boole       _widget_select_internal__on_mouse_wheel(gui_node_t* n, int64 x, int64 y, float delta);
static void        _widget_select_internal__on_char(gui_node_t* n, uint codepoint);
static void        _widget_select_internal__on_key(gui_node_t* n, int64 vk, boole down);
static void        _widget_select_internal__on_destroy(gui_node_t* n);

// ===== small helpers =========================================================

/**
 * @brief the select's state, created on the first call with the texts and marks a new select starts with
 */
static _widget_select_internal__state_t* _widget_select_internal__state(gui_node_t* n)
{
    _widget_select_internal__state_t* state = NULL_POINTER;
    boole created = (boole)(n != NULL_POINTER && n->user_data == NULL_POINTER);

    state = (_widget_select_internal__state_t*)scene__widget_state(n);
    if (created == TRUE && state != NULL_POINTER)
    {
        _widget_select_internal__copy_text(state->confirm, "OK");
        _widget_select_internal__copy_text(state->cancel, "Cancel");
        _widget_select_internal__copy_text(state->placeholder, "Search");
        state->highlight = -1;
        state->pending = -1;
    }
    return state;
}

static boole _widget_select_internal__is_inline(gui_select_style_e style)
{
    return (boole)(style == GUI_SELECT_STYLE__SEGMENTED || style == GUI_SELECT_STYLE__CHIPS || style == GUI_SELECT_STYLE__CAROUSEL);
}

// the option in the middle is the value, and the content stops on an option
static boole _widget_select_internal__is_turning(gui_select_style_e style)
{
    return (boole)(style == GUI_SELECT_STYLE__WHEEL || style == GUI_SELECT_STYLE__CAROUSEL);
}

static boole _widget_select_internal__is_horizontal(gui_select_style_e style)
{
    return _widget_select_internal__is_inline(style);
}

static float _widget_select_internal__row_height(gui_node_t* n, float scale)
{
    return ((n->resolved.option_height > 0.0f) ? n->resolved.option_height : 32.0f) * scale;
}

static float _widget_select_internal__duration(gui_node_t* n)
{
    if (motion__reduced_motion())
    {
        return 0.0f;
    }
    return (n->resolved.popup_set & GUI_POPUP_SET__DURATION) ? n->resolved.popup_duration_ms : 150.0f;
}

/**
 * @brief the time the offset takes to its target when no spring moves it: scroll-smooth, else 120 ms for the styles that turn or slide a mark, else 0 (at once)
 */
static float _widget_select_internal__smooth(gui_node_t* n)
{
    gui_select_style_e style = n->resolved.select_style;

    if (motion__reduced_motion())
    {
        return 0.0f;
    }
    if (n->resolved.scroll_smooth_ms > 0.0f)
    {
        return n->resolved.scroll_smooth_ms;
    }
    if (_widget_select_internal__is_turning(style) == TRUE || style == GUI_SELECT_STYLE__SEGMENTED)
    {
        return _WIDGET_SELECT_INTERNAL__TURN_MS;
    }
    return 0.0f;
}

static boole _widget_select_internal__contains(gui_rect_t r, float x, float y)
{
    return (boole)(x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h);
}

static float _widget_select_internal__clamp(float value, float low, float high)
{
    if (value < low)
    {
        return low;
    }
    if (value > high)
    {
        return high;
    }
    return value;
}

static float _widget_select_internal__round(float value)
{
    return stdlib__floorf(value + 0.5f);
}

/**
 * @brief whether text contains query, letters compared without case; an empty query is contained in every text
 */
static boole _widget_select_internal__text_contains(const char* text, const char* query)
{
    const char* from = NULL_POINTER;
    const char* a = NULL_POINTER;
    const char* b = NULL_POINTER;

    if (query[0] == 0)
    {
        return TRUE;
    }
    for (from = text; *from != 0; from++)
    {
        a = from;
        b = query;
        while (*a != 0 && *b != 0 && stdlib__tolower((ubyte)*a) == stdlib__tolower((ubyte)*b))
        {
            a++;
            b++;
        }
        if (*b == 0)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static void _widget_select_internal__copy_text(char* out, const char* text)
{
    stdlib__strcpy_safe(out, _WIDGET_SELECT_INTERNAL__TEXT_CAP, text);
}

static gui_color_t _widget_select_internal__text_color(gui_style_t* s)
{
    if (s->has_option_color)
    {
        return s->option_color;
    }
    return s->has_font_color ? s->font_color : scene__rgb(0.96f, 0.96f, 0.98f);
}

static gui_color_t _widget_select_internal__accent_color(gui_style_t* s)
{
    return s->has_accent_color ? s->accent_color : scene__rgb(0.39f, 0.40f, 0.95f);
}

static gui_color_t _widget_select_internal__separator_color(gui_style_t* s)
{
    gui_color_t color;

    if (s->has_option_separator)
    {
        return s->option_separator;
    }
    color = _widget_select_internal__text_color(s);
    color.a *= 0.2f;
    return color;
}

static gui_color_t _widget_select_internal__panel_color(gui_style_t* s)
{
    gui_color_t panel;

    if (s->has_popup_background)
    {
        return s->popup_background;
    }
    if (s->has_background_color)
    {
        panel = s->background_color;
        panel.r *= 0.85f;
        panel.g *= 0.85f;
        panel.b *= 0.85f;
        return panel;
    }
    return scene__rgb(0.09f, 0.10f, 0.13f);
}

static gui_color_t _widget_select_internal__mix(gui_color_t a, gui_color_t b, float t)
{
    gui_color_t c;

    c.r = a.r + (b.r - a.r) * t;
    c.g = a.g + (b.g - a.g) * t;
    c.b = a.b + (b.b - a.b) * t;
    c.a = a.a + (b.a - a.a) * t;
    return c;
}

static gui_font_t* _widget_select_internal__font(gui_node_t* n, float scale)
{
    return font__at(n->resolved.font_family, ((n->resolved.font_size > 0.0f) ? n->resolved.font_size : 14.0f) * scale);
}

// ===== the options ===========================================================

/**
 * @brief count the option children; one walk over the children
 */
static int64 _widget_select_internal__option_count(gui_node_t* select)
{
    gui_node_t* c = NULL_POINTER;
    int64 count = 0;

    for (c = select->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        _widget_select_internal__walked++;
        if (c->type == GUI_NODE_TYPE__OPTION)
        {
            count++;
        }
    }
    return count;
}

/**
 * @brief the option with this index, NULL when out of range; walks index + 1 children
 */
static gui_node_t* _widget_select_internal__option_at(gui_node_t* select, int64 index)
{
    gui_node_t* c = NULL_POINTER;
    int64 i = 0;

    if (index < 0)
    {
        return NULL_POINTER;
    }
    for (c = select->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        _widget_select_internal__walked++;
        if (c->type == GUI_NODE_TYPE__OPTION)
        {
            if (i == index)
            {
                return c;
            }
            i++;
        }
    }
    return NULL_POINTER;
}

/**
 * @brief the width of the longest option's text; one walk, each text measured through the font's width cache
 */
static float _widget_select_internal__widest_option(gui_node_t* n, gui_font_t* font)
{
    gui_node_t* c = NULL_POINTER;
    float widest = 0.0f;
    float w = 0.0f;

    if (font == NULL_POINTER)
    {
        return 0.0f;
    }
    for (c = n->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        _widget_select_internal__walked++;
        if (c->type == GUI_NODE_TYPE__OPTION && c->text[0] != 0)
        {
            w = font__measure(font, c->text);
            if (w > widest)
            {
                widest = w;
            }
        }
    }
    return widest;
}

/**
 * @brief search: collect the options whose text contains the query; one walk
 */
static void _widget_select_internal__filter(gui_node_t* n, _widget_select_internal__state_t* state, int64 count)
{
    gui_node_t* c = NULL_POINTER;
    int* grown = NULL_POINTER;
    int64 index = 0;

    state->match_count = 0;
    if (count > state->match_capacity)
    {
        grown = (int*)GUI_MALLOC_T((size_t)count * sizeof(int), MM_TYPE__GENERIC);
        if (grown == NULL_POINTER)
        {
            return;
        }
        GUI_FREE(state->matches);
        state->matches = grown;
        state->match_capacity = count;
    }
    for (c = n->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        _widget_select_internal__walked++;
        if (c->type != GUI_NODE_TYPE__OPTION)
        {
            continue;
        }
        if (index < state->match_capacity && _widget_select_internal__text_contains(c->text, state->query) == TRUE)
        {
            state->matches[state->match_count] = (int)index;
            state->match_count++;
        }
        index++;
    }
}

static int64 _widget_select_internal__option_of_row(const _widget_select_internal__metrics_t* m, const _widget_select_internal__state_t* state, int64 row)
{
    if (row < 0 || row >= m->rows_count)
    {
        return -1;
    }
    if (m->filtered == TRUE)
    {
        return (state != NULL_POINTER && row < state->match_count) ? (int64)state->matches[row] : -1;
    }
    return row;
}

static int64 _widget_select_internal__row_of_option(const _widget_select_internal__metrics_t* m, const _widget_select_internal__state_t* state, int64 option)
{
    int64 low = 0;
    int64 high = 0;
    int64 middle = 0;

    if (m->filtered == FALSE)
    {
        return (option >= 0 && option < m->rows_count) ? option : -1;
    }
    if (state == NULL_POINTER)
    {
        return -1;
    }
    // the matches ascend
    high = state->match_count - 1;
    while (low <= high)
    {
        middle = (low + high) / 2;
        if ((int64)state->matches[middle] == option)
        {
            return middle;
        }
        if ((int64)state->matches[middle] < option)
        {
            low = middle + 1;
        }
        else
        {
            high = middle - 1;
        }
    }
    return -1;
}

/**
 * @brief the option nodes of the rows first..last, out[row - first]; one walk over the children, because rows and options ascend together
 */
static void _widget_select_internal__options_of_rows(gui_node_t* n, const _widget_select_internal__metrics_t* m, const _widget_select_internal__state_t* state, int64 first, int64 last, gui_node_t** out)
{
    gui_node_t* c = NULL_POINTER;
    int64 index = 0;
    int64 row = first;
    int64 wanted = 0;
    int64 i = 0;

    for (i = 0; i <= last - first; i++)
    {
        out[i] = NULL_POINTER;
    }
    if (last < first)
    {
        return;
    }
    wanted = _widget_select_internal__option_of_row(m, state, row);
    for (c = n->first_child; c != NULL_POINTER && row <= last; c = c->next_sibling)
    {
        _widget_select_internal__walked++;
        if (c->type != GUI_NODE_TYPE__OPTION)
        {
            continue;
        }
        if (index == wanted)
        {
            out[row - first] = c;
            row++;
            wanted = _widget_select_internal__option_of_row(m, state, row);
        }
        index++;
    }
}

// ===== geometry ==============================================================

/**
 * @brief keep the shown offset valid between two drawn frames: on its target when the style moves nothing over time (so a value the host wrote, or a wheel tick, is in place for the next read), inside the content when no spring may carry it past an edge
 */
static void _widget_select_internal__settle(gui_node_t* n, _widget_select_internal__state_t* state, float max_scroll)
{
    boole spring = (boole)(scroll__wobble_enabled(&n->resolved) == TRUE && motion__reduced_motion() == FALSE);
    boole held = (boole)(state->thumb_held == TRUE || (state->drag_held == TRUE && state->drag_moved == TRUE));

    if (held == TRUE || spring == TRUE)
    {
        return;
    }
    if (_widget_select_internal__smooth(n) <= 0.0f)
    {
        state->scroll = state->scroll_target;
        state->scroll_velocity = 0.0f;
        return;
    }
    state->scroll = _widget_select_internal__clamp(state->scroll, 0.0f, max_scroll);
}

/**
 * @brief place a panel of w x h at the control or in the centre of the viewport (popup-align, popup-placement, popup-gap), kept inside the viewport
 */
static gui_rect_t _widget_select_internal__place(gui_node_t* n, float w, float h, float scale)
{
    gui_style_t* s = &n->resolved;
    gui_rect_t r;
    int64 vw = 0;
    int64 vh = 0;
    float gap = 0.0f;

    stdlib__null_memory(&r, sizeof(r));
    gap = ((s->popup_set & GUI_POPUP_SET__GAP) ? s->popup_gap : 4.0f) * scale;
    scene__viewport(&vw, &vh);
    r.w = w;
    r.h = h;
    if (r.w > (float)vw)
    {
        r.w = (float)vw;
    }
    if (r.h > (float)vh)
    {
        r.h = (float)vh;
    }
    r.x = n->bounds.x;
    r.y = n->bounds.y + n->bounds.h + gap;
    if (s->popup_centered)
    {
        r.x = ((float)vw - r.w) * 0.5f;
        r.y = ((float)vh - r.h) * 0.5f;
    }
    else if (s->popup_placement == GUI_POPUP_PLACEMENT__ABOVE || (s->popup_placement == GUI_POPUP_PLACEMENT__AUTO && r.y + r.h > (float)vh && n->bounds.y > (float)vh - r.y))
    {
        r.y = n->bounds.y - gap - r.h;
    }
    r.x = stdlib__fmaxf(0.0f, stdlib__fminf(r.x, (float)vw - r.w));
    r.y = stdlib__fmaxf(0.0f, stdlib__fminf(r.y, (float)vh - r.h));
    return r;
}

/**
 * @brief the panel, its parts and the reveal clock of the styles that stack their rows: list, sheet, page, menu, dialog, search
 */
static void _widget_select_internal__metrics_stacked(gui_node_t* n, _widget_select_internal__metrics_t* m)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    gui_style_t* s = &n->resolved;
    gui_font_t* font = NULL_POINTER;
    float rows_h = 0.0f;
    float header_h = 0.0f;
    float footer_h = 0.0f;
    float room = 0.0f;
    float w = 0.0f;
    float pad = 0.0f;
    float progress = 0.0f;
    float offset = 0.0f;
    float bar_size = 0.0f;
    float button_w = 0.0f;
    float button_h = 0.0f;
    int64 fit = 0;
    boole titled = (boole)(state != NULL_POINTER && state->title[0] != 0);

    pad = ((s->popup_set & GUI_POPUP_SET__PADDING) ? s->option_padding : 12.0f) * m->scale;
    rows_h = (float)m->rows_count * m->row_height;
    if (s->select_rows >= 1.0f)
    {
        rows_h = stdlib__fminf(rows_h, stdlib__floorf(s->select_rows) * m->row_height);
    }
    room = m->view_h;
    w = (s->popup_width > 0.0f) ? s->popup_width * m->scale : n->bounds.w;
    if (m->style == GUI_SELECT_STYLE__SEARCH)
    {
        header_h = m->row_height;
    }
    else if (m->style == GUI_SELECT_STYLE__SHEET)
    {
        header_h = (_WIDGET_SELECT_INTERNAL__HANDLE_H + ((titled == TRUE) ? _WIDGET_SELECT_INTERNAL__SHEET_TITLE_H : 0.0f)) * m->scale;
        room = m->view_h * _WIDGET_SELECT_INTERNAL__SHEET_PART + header_h;
        w = (s->popup_width > 0.0f) ? s->popup_width * m->scale : m->view_w;
    }
    else if (m->style == GUI_SELECT_STYLE__PAGE)
    {
        header_h = _WIDGET_SELECT_INTERNAL__BAR_H * m->scale;
        w = m->view_w;
    }
    else if (m->style == GUI_SELECT_STYLE__DIALOG)
    {
        header_h = ((titled == TRUE) ? _WIDGET_SELECT_INTERNAL__BAR_H : _WIDGET_SELECT_INTERNAL__DIALOG_TOP) * m->scale;
        footer_h = _WIDGET_SELECT_INTERNAL__BAR_H * m->scale;
        room = m->view_h - 2.0f * _WIDGET_SELECT_INTERNAL__DIALOG_MARGIN * m->scale;
        w = (s->popup_width > 0.0f) ? s->popup_width * m->scale : _WIDGET_SELECT_INTERNAL__DIALOG_W * m->scale;
        w = stdlib__fminf(w, m->view_w - 2.0f * _WIDGET_SELECT_INTERNAL__DIALOG_MARGIN * m->scale);
    }
    else if (m->style == GUI_SELECT_STYLE__MENU && s->popup_width <= 0.0f)
    {
        // as wide as the longest option, with the column of the check mark
        font = _widget_select_internal__font(n, m->scale);
        w = _widget_select_internal__widest_option(n, font) + 2.0f * pad + (_WIDGET_SELECT_INTERNAL__MARK + _WIDGET_SELECT_INTERNAL__MARK_GAP) * m->scale;
    }
    if (w > m->view_w)
    {
        w = m->view_w;
    }
    if (w < 0.0f)
    {
        w = 0.0f;
    }
    rows_h = _widget_select_internal__clamp(rows_h, 0.0f, stdlib__fmaxf(0.0f, room - header_h - footer_h));

    // the panel at rest
    if (m->style == GUI_SELECT_STYLE__PAGE)
    {
        m->rest.w = m->view_w;
        m->rest.h = m->view_h;
        rows_h = stdlib__fmaxf(0.0f, m->view_h - header_h);
    }
    else if (m->style == GUI_SELECT_STYLE__SHEET)
    {
        m->rest.w = w;
        m->rest.h = header_h + rows_h;
        m->rest.x = (m->view_w - w) * 0.5f;
        m->rest.y = m->view_h - m->rest.h;
    }
    else if (m->style == GUI_SELECT_STYLE__DIALOG)
    {
        m->rest.w = w;
        m->rest.h = header_h + rows_h + footer_h;
        m->rest.x = (m->view_w - w) * 0.5f;
        m->rest.y = (m->view_h - m->rest.h) * 0.5f;
    }
    else
    {
        m->rest = _widget_select_internal__place(n, w, header_h + rows_h, m->scale);
        // a panel cut to the viewport takes the cut from its rows
        rows_h = stdlib__fmaxf(0.0f, m->rest.h - header_h);
    }

    // the reveal clock: only the rows that fit take part in the stagger
    fit = (m->row_height > 0.0f) ? (int64)stdlib__ceilf(rows_h / m->row_height) : 1;
    if (fit < 1)
    {
        fit = 1;
    }
    m->stagger_rows = (m->rows_count < fit) ? m->rows_count : fit;
    m->total = (m->stagger_rows > 0) ? m->duration + (float)(m->stagger_rows - 1) * m->stagger : m->duration;
    m->clock = n->widget_anim * m->total;
    progress = (m->duration > 0.0f) ? _widget_select_internal__clamp(m->clock / m->duration, 0.0f, 1.0f) : ((m->open == TRUE) ? 1.0f : 0.0f);
    m->enter = 1.0f - (1.0f - progress) * (1.0f - progress) * (1.0f - progress);

    // the panel as drawn: each style's entrance
    m->panel = m->rest;
    if (m->style == GUI_SELECT_STYLE__SHEET)
    {
        m->panel.y += (1.0f - m->enter) * m->rest.h;
        if (state != NULL_POINTER)
        {
            m->panel.y += state->sheet_pull;
        }
    }
    else if (m->style == GUI_SELECT_STYLE__PAGE)
    {
        m->panel.x += (1.0f - m->enter) * m->view_w;
    }
    else if (m->style == GUI_SELECT_STYLE__DIALOG)
    {
        m->panel.y += (1.0f - m->enter) * 8.0f * m->scale;
    }
    else if (m->style == GUI_SELECT_STYLE__MENU)
    {
        // grows from the edge that faces the control
        m->panel.h = m->rest.h * m->enter;
        if (m->rest.y < n->bounds.y)
        {
            m->panel.y = m->rest.y + m->rest.h - m->panel.h;
        }
    }
    m->header.x = m->panel.x;
    m->header.y = m->panel.y;
    m->header.w = m->panel.w;
    m->header.h = header_h;
    m->rows.x = m->panel.x;
    m->rows.y = m->panel.y + header_h;
    m->rows.w = m->panel.w;
    m->rows.h = rows_h;
    if (m->style == GUI_SELECT_STYLE__MENU)
    {
        m->rows.h = stdlib__fminf(rows_h, stdlib__fmaxf(0.0f, m->panel.h - header_h));
    }
    m->footer.x = m->panel.x;
    m->footer.y = m->rows.y + rows_h;
    m->footer.w = m->panel.w;
    m->footer.h = footer_h;
    if (m->style == GUI_SELECT_STYLE__PAGE)
    {
        m->back.x = m->header.x;
        m->back.y = m->header.y;
        m->back.w = header_h;
        m->back.h = header_h;
    }
    if (m->style == GUI_SELECT_STYLE__DIALOG && state != NULL_POINTER)
    {
        font = _widget_select_internal__font(n, m->scale);
        button_h = _WIDGET_SELECT_INTERNAL__BUTTON_H * m->scale;
        button_w = ((font != NULL_POINTER) ? font__measure(font, state->confirm) : 0.0f) + 2.0f * _WIDGET_SELECT_INTERNAL__BUTTON_PAD * m->scale;
        m->confirm.w = button_w;
        m->confirm.h = button_h;
        m->confirm.x = m->footer.x + m->footer.w - pad - button_w;
        m->confirm.y = m->footer.y + (m->footer.h - button_h) * 0.5f;
        button_w = ((font != NULL_POINTER) ? font__measure(font, state->cancel) : 0.0f) + 2.0f * _WIDGET_SELECT_INTERNAL__BUTTON_PAD * m->scale;
        m->cancel.w = button_w;
        m->cancel.h = button_h;
        m->cancel.x = m->confirm.x - _WIDGET_SELECT_INTERNAL__BUTTON_GAP * m->scale - button_w;
        m->cancel.y = m->confirm.y;
    }

    // scrolling and the scrollbar
    m->content = (float)m->rows_count * m->row_height;
    m->max_scroll = stdlib__fmaxf(0.0f, m->content - rows_h);
    m->row_width = m->rows.w;
    if (m->max_scroll > 0.0f)
    {
        bar_size = stdlib__fminf(scroll__bar_size(n, m->scale), m->rows.w);
        m->bar.x = m->rows.x + m->rows.w - bar_size;
        m->bar.y = m->rows.y;
        m->bar.w = bar_size;
        m->bar.h = m->rows.h;
        m->row_width = m->rows.w - bar_size;
    }
    if (state != NULL_POINTER)
    {
        // options were removed, the query changed, or the viewport grew: the resting place stays inside the rows
        state->scroll_target = _widget_select_internal__clamp(state->scroll_target, 0.0f, m->max_scroll);
        _widget_select_internal__settle(n, state, m->max_scroll);
        m->scroll = state->scroll;
        m->velocity = state->scroll_velocity;
        m->stagger_first = state->stagger_first;
    }
    if (m->stagger_first > m->rows_count - m->stagger_rows)
    {
        m->stagger_first = m->rows_count - m->stagger_rows;
    }
    if (m->stagger_first < 0)
    {
        m->stagger_first = 0;
    }
    offset = ((s->popup_set & GUI_POPUP_SET__OFFSET) ? s->popup_offset : 10.0f) * m->scale;
    if (m->reveal == GUI_POPUP_REVEAL__BOTTOM_UP)
    {
        m->travel = offset;
    }
    else if (m->reveal == GUI_POPUP_REVEAL__TOP_DOWN)
    {
        m->travel = -offset;
    }
    m->reach = stdlib__fabsf(m->travel);
}

/**
 * @brief the wheel: select-rows rows on the visible part of a cylinder, the row in the middle at full height
 *
 * A row at the distance k (in rows) from the middle is at the angle
 * k * step, where step = WHEEL_END / (rows / 2): the rows at the two ends
 * of the view are at WHEEL_END. The radius makes the row in the middle
 * option-height high: radius = option-height / (2 sin(step / 2)). The
 * panel's height is the chord between the two ends, 2 radius sin(WHEEL_END).
 */
static void _widget_select_internal__metrics_wheel(gui_node_t* n, _widget_select_internal__metrics_t* m)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    gui_style_t* s = &n->resolved;
    float progress = 0.0f;
    float w = 0.0f;

    m->visible = (s->select_rows >= 3.0f) ? ((int64)s->select_rows | 1) : 5;
    m->wheel_step = _WIDGET_SELECT_INTERNAL__WHEEL_END / ((float)m->visible * 0.5f);
    m->wheel_radius = m->row_height / (2.0f * stdlib__sinf(m->wheel_step * 0.5f));
    w = (s->popup_width > 0.0f) ? s->popup_width * m->scale : n->bounds.w;
    m->rest = _widget_select_internal__place(n, w, 2.0f * m->wheel_radius * stdlib__sinf(_WIDGET_SELECT_INTERNAL__WHEEL_END), m->scale);
    m->panel = m->rest;
    m->rows = m->rest;
    m->row_width = m->rows.w;
    // the rows appear together: no stagger, no travel
    m->stagger = 0.0f;
    m->stagger_rows = 1;
    m->total = m->duration;
    m->clock = n->widget_anim * m->total;
    progress = (m->duration > 0.0f) ? _widget_select_internal__clamp(m->clock / m->duration, 0.0f, 1.0f) : ((m->open == TRUE) ? 1.0f : 0.0f);
    m->enter = 1.0f - (1.0f - progress) * (1.0f - progress) * (1.0f - progress);
    m->content = (float)m->rows_count * m->row_height;
    m->max_scroll = (m->rows_count > 0) ? (float)(m->rows_count - 1) * m->row_height : 0.0f;
    if (state != NULL_POINTER)
    {
        state->scroll_target = _widget_select_internal__clamp(state->scroll_target, 0.0f, m->max_scroll);
        _widget_select_internal__settle(n, state, m->max_scroll);
        m->scroll = state->scroll;
        m->velocity = state->scroll_velocity;
    }
}

/**
 * @brief segmented, chips, carousel: the options inside the control's own rectangle
 */
static void _widget_select_internal__metrics_inline(gui_node_t* n, _widget_select_internal__metrics_t* m)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    gui_style_t* s = &n->resolved;
    gui_node_t* c = NULL_POINTER;
    gui_font_t* font = NULL_POINTER;
    float pad = 0.0f;
    float gap = 0.0f;

    m->rest = n->bounds;
    m->panel = n->bounds;
    m->rows = n->bounds;
    m->row_width = n->bounds.w;
    m->enter = 1.0f;
    m->open = TRUE;
    if (m->style == GUI_SELECT_STYLE__SEGMENTED)
    {
        m->row_height = (m->count > 0) ? n->bounds.w / (float)m->count : n->bounds.w;
        m->max_scroll = (m->count > 0) ? (float)(m->count - 1) * m->row_height : 0.0f;
    }
    else if (m->style == GUI_SELECT_STYLE__CAROUSEL)
    {
        m->visible = (s->select_rows >= 3.0f) ? ((int64)s->select_rows | 1) : 5;
        m->row_height = n->bounds.w / (float)m->visible;
        m->max_scroll = (m->count > 0) ? (float)(m->count - 1) * m->row_height : 0.0f;
    }
    else
    {
        // chips: each as wide as its text
        font = _widget_select_internal__font(n, m->scale);
        pad = ((s->popup_set & GUI_POPUP_SET__PADDING) ? s->option_padding : 12.0f) * m->scale;
        gap = ((s->popup_set & GUI_POPUP_SET__GAP) ? s->popup_gap : _WIDGET_SELECT_INTERNAL__CHIP_GAP) * m->scale;
        for (c = n->first_child; c != NULL_POINTER; c = c->next_sibling)
        {
            _widget_select_internal__walked++;
            if (c->type == GUI_NODE_TYPE__OPTION)
            {
                m->content += ((font != NULL_POINTER) ? font__measure(font, c->text) : 0.0f) + 2.0f * pad + gap;
            }
        }
        if (m->content > 0.0f)
        {
            m->content -= gap;
        }
        m->max_scroll = stdlib__fmaxf(0.0f, m->content - n->bounds.w);
    }
    if (state != NULL_POINTER)
    {
        // a carousel and the mark of a segmented control head for the value, also when the host wrote it
        if (m->style != GUI_SELECT_STYLE__CHIPS && state->drag_held == FALSE)
        {
            state->scroll_target = (float)(int64)n->value * m->row_height;
        }
        state->scroll_target = _widget_select_internal__clamp(state->scroll_target, 0.0f, m->max_scroll);
        _widget_select_internal__settle(n, state, m->max_scroll);
        m->scroll = state->scroll;
        m->velocity = state->scroll_velocity;
    }
}

/**
 * @brief fill the record the options are placed with
 */
static void _widget_select_internal__metrics(gui_node_t* n, _widget_select_internal__metrics_t* m)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    gui_style_t* s = &n->resolved;
    int64 vw = 0;
    int64 vh = 0;

    stdlib__null_memory(m, sizeof(*m));
    scene__viewport(&vw, &vh);
    m->style = s->select_style;
    m->scale = scene__scale();
    m->view_w = (float)vw;
    m->view_h = (float)vh;
    m->count = _widget_select_internal__option_count(n);
    m->rows_count = m->count;
    m->row_height = _widget_select_internal__row_height(n, m->scale);
    m->open = n->is_open;
    m->reveal = s->popup_reveal;
    m->duration = _widget_select_internal__duration(n);
    m->stagger = (m->reveal == GUI_POPUP_REVEAL__FADE) ? 0.0f : s->popup_stagger_ms;
    if (m->style == GUI_SELECT_STYLE__SEARCH && state != NULL_POINTER && state->query_len > 0)
    {
        _widget_select_internal__filter(n, state, m->count);
        m->filtered = TRUE;
        m->rows_count = state->match_count;
    }
    if (_widget_select_internal__is_inline(m->style) == TRUE)
    {
        _widget_select_internal__metrics_inline(n, m);
    }
    else if (m->style == GUI_SELECT_STYLE__WHEEL)
    {
        _widget_select_internal__metrics_wheel(n, m);
    }
    else
    {
        _widget_select_internal__metrics_stacked(n, m);
    }
    m->presence = (_widget_select_internal__is_inline(m->style) == TRUE || state == NULL_POINTER) ? 1.0f : state->presence;
    m->enter *= m->presence;
    _widget_select_internal__stretch_metrics(n, m);
}

// Continuous deformation of both row edges keeps adjacent rows joined. Its
// slope is at least 0.5, so the inverse remains well-defined for hit testing.
static float _widget_select_internal__stretch_point(const _widget_select_internal__metrics_t* m, float point, boole inverse)
{
    float lag = m->stretch_lag;
    float reach = m->stretch_reach;
    float anchor = m->stretch_anchor;
    float ratio = 0.0f;
    if (lag == 0.0f || reach <= 0.0f)
    {
        return point;
    }
    if (inverse == FALSE)
    {
        return point + lag * stdlib__fminf(stdlib__fabsf(point - anchor) / reach, 1.0f);
    }
    if (point <= anchor - reach + lag || point >= anchor + reach + lag)
    {
        return point - lag;
    }
    ratio = lag / reach;
    return point < anchor ? (point - ratio * anchor) / (1.0f - ratio) : (point + ratio * anchor) / (1.0f + ratio);
}

static void _widget_select_internal__stretch_rect(const _widget_select_internal__metrics_t* m, gui_rect_t* rect)
{
    float center = 0.0f;
    if (_widget_select_internal__is_horizontal(m->style) == TRUE)
    {
        center = rect->x + rect->w * 0.5f;
        rect->x = _widget_select_internal__stretch_point(m, center, FALSE) - rect->w * 0.5f;
    }
    else
    {
        center = rect->y + rect->h * 0.5f;
        rect->y = _widget_select_internal__stretch_point(m, center, FALSE) - rect->h * 0.5f;
    }
}

static void _widget_select_internal__stretch_metrics(gui_node_t* n, _widget_select_internal__metrics_t* m)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    float extent = _widget_select_internal__is_horizontal(m->style) ? m->rows.w : m->rows.h;
    float amount = n->resolved.scroll_start_effect_amount;
    m->stretch_lag = 0.0f;
    if (state == NULL_POINTER || n->resolved.scroll_start_effect != GUI_SCROLL_START_EFFECT__STRETCH || amount <= 0.0f || motion__reduced_motion() == TRUE || state->thumb_held == TRUE || m->style == GUI_SELECT_STYLE__SEGMENTED)
    {
        return;
    }
    m->stretch_reach = stdlib__fmaxf(extent * 0.5f, 2.0f * m->row_height);
    m->stretch_anchor = state->stretch_anchor;
    // Content moves against the scroll offset: expand spacing ahead of that motion.
    m->stretch_lag = _widget_select_internal__clamp(state->scroll_far - state->scroll, -amount * m->stretch_reach * 0.5f, amount * m->stretch_reach * 0.5f);
    m->reach += stdlib__fabsf(m->stretch_lag);
}

static boole _widget_select_internal__stretch_step(gui_node_t* n, const _widget_select_internal__metrics_t* m, float elapsed)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    float amount = n->resolved.scroll_start_effect_amount;
    float lag = 0.0f;
    int64 mx = 0;
    int64 my = 0;
    boole horizontal = _widget_select_internal__is_horizontal(m->style);
    if (state == NULL_POINTER)
    {
        return FALSE;
    }
    if (n->resolved.scroll_start_effect != GUI_SCROLL_START_EFFECT__STRETCH || amount <= 0.0f || motion__reduced_motion() == TRUE || state->thumb_held == TRUE || m->style == GUI_SELECT_STYLE__SEGMENTED)
    {
        state->scroll_far = state->scroll;
        state->stretch_anchored = FALSE;
        return FALSE;
    }
    lag = state->scroll - state->scroll_far;
    if (stdlib__fabsf(lag) < 0.05f)
    {
        state->scroll_far = state->scroll;
        state->stretch_anchored = FALSE;
        return FALSE;
    }
    if (state->stretch_anchored == FALSE)
    {
        state->stretch_anchor = horizontal ? m->rows.x + m->rows.w * 0.5f : m->rows.y + m->rows.h * 0.5f;
        state->stretch_anchored = TRUE;
    }
    if (scene__pointer_position(&mx, &my) == TRUE && _widget_select_internal__contains(m->rows, (float)mx, (float)my) == TRUE)
    {
        state->stretch_anchor = horizontal ? (float)mx : (float)my;
    }
    state->scroll_far += lag * (1.0f - stdlib__expf(-elapsed / (amount * 150.0f)));
    return TRUE;
}

/**
 * @brief the rectangle and the opacity a row of a stacked style is drawn with; no walk over the options
 * @return boole -> FALSE for a row out of range and for a row that is not visible
 */
static boole _widget_select_internal__row(const _widget_select_internal__metrics_t* m, int64 row, gui_rect_t* bounds, float* alpha)
{
    int64 rank = 0;
    float progress = 0.0f;
    float ease = 0.0f;

    if (alpha != NULL_POINTER)
    {
        *alpha = 0.0f;
    }
    if (row < 0 || row >= m->rows_count)
    {
        return FALSE;
    }
    rank = (m->reveal == GUI_POPUP_REVEAL__BOTTOM_UP) ? (m->stagger_first + m->stagger_rows - 1) - row : row - m->stagger_first;
    if (rank > m->stagger_rows - 1)
    {
        rank = m->stagger_rows - 1;
    }
    if (rank < 0)
    {
        rank = 0;
    }
    if (m->duration > 0.0f)
    {
        progress = (m->clock - (float)rank * m->stagger) / m->duration;
    }
    else
    {
        progress = (m->open == TRUE) ? 1.0f : 0.0f;
    }
    progress = _widget_select_internal__clamp(progress, 0.0f, 1.0f);
    ease = 1.0f - (1.0f - progress) * (1.0f - progress) * (1.0f - progress);
    bounds->x = m->rows.x;
    bounds->w = m->row_width;
    bounds->h = m->row_height;
    bounds->y = m->rows.y + (float)row * m->row_height - m->scroll + m->travel * (1.0f - ease);
    _widget_select_internal__stretch_rect(m, bounds);
    if (alpha != NULL_POINTER)
    {
        *alpha = ease * m->presence;
    }
    return (boole)(ease > 0.001f && bounds->y < m->rows.y + m->rows.h && bounds->y + bounds->h > m->rows.y);
}

/**
 * @brief the rectangle and the opacity of a wheel's row
 * @param float* nearness -> optional: 1 for the row in the middle, falling to 0 one row away from it
 * @return boole -> FALSE for a row outside the visible part of the wheel
 */
static boole _widget_select_internal__wheel_row(const _widget_select_internal__metrics_t* m, int64 row, gui_rect_t* bounds, float* alpha, float* nearness)
{
    float distance = 0.0f;
    float angle = 0.0f;
    float from = 0.0f;
    float to = 0.0f;
    float middle = 0.0f;

    if (alpha != NULL_POINTER)
    {
        *alpha = 0.0f;
    }
    if (nearness != NULL_POINTER)
    {
        *nearness = 0.0f;
    }
    if (row < 0 || row >= m->rows_count || m->row_height <= 0.0f)
    {
        return FALSE;
    }
    // A row covers the angles from half a step before its middle to half a
    // step after it. Its rectangle is the projection of that arc, so the
    // rows at the two ends of the view end at the panel's edges, and a row
    // that turns out of the view gets flatter until it is gone.
    distance = (float)row - m->scroll / m->row_height;
    from = (distance - 0.5f) * m->wheel_step;
    to = (distance + 0.5f) * m->wheel_step;
    if (from >= _WIDGET_SELECT_INTERNAL__WHEEL_END || to <= -_WIDGET_SELECT_INTERNAL__WHEEL_END)
    {
        return FALSE;
    }
    from = _widget_select_internal__clamp(from, -_WIDGET_SELECT_INTERNAL__WHEEL_END, _WIDGET_SELECT_INTERNAL__WHEEL_END);
    to = _widget_select_internal__clamp(to, -_WIDGET_SELECT_INTERNAL__WHEEL_END, _WIDGET_SELECT_INTERNAL__WHEEL_END);
    angle = _widget_select_internal__clamp(distance * m->wheel_step, -_WIDGET_SELECT_INTERNAL__WHEEL_END, _WIDGET_SELECT_INTERNAL__WHEEL_END);
    middle = m->rows.y + m->rows.h * 0.5f;
    bounds->x = m->rows.x;
    bounds->w = m->rows.w;
    bounds->y = middle + m->wheel_radius * stdlib__sinf(from);
    bounds->h = m->wheel_radius * (stdlib__sinf(to) - stdlib__sinf(from));
    _widget_select_internal__stretch_rect(m, bounds);
    if (alpha != NULL_POINTER)
    {
        *alpha = stdlib__cosf(stdlib__fabsf(angle) / _WIDGET_SELECT_INTERNAL__WHEEL_END * _WIDGET_SELECT_INTERNAL__HALF_PI) * m->enter;
    }
    if (nearness != NULL_POINTER)
    {
        *nearness = _widget_select_internal__clamp(1.0f - stdlib__fabsf(distance), 0.0f, 1.0f);
    }
    return TRUE;
}

/**
 * @brief the rectangle and the opacity of a carousel's item: the wheel's fade along x, without the cylinder
 */
static boole _widget_select_internal__carousel_item(gui_node_t* n, const _widget_select_internal__metrics_t* m, int64 index, gui_rect_t* bounds, float* alpha, float* nearness)
{
    float distance = 0.0f;
    float reach = (float)m->visible * 0.5f;

    if (alpha != NULL_POINTER)
    {
        *alpha = 0.0f;
    }
    if (nearness != NULL_POINTER)
    {
        *nearness = 0.0f;
    }
    if (index < 0 || index >= m->count || m->row_height <= 0.0f)
    {
        return FALSE;
    }
    distance = (float)index - m->scroll / m->row_height;
    if (stdlib__fabsf(distance) >= reach + 0.5f)
    {
        return FALSE;
    }
    bounds->w = m->row_height;
    bounds->h = n->bounds.h;
    bounds->x = n->bounds.x + n->bounds.w * 0.5f + distance * m->row_height - m->row_height * 0.5f;
    bounds->y = n->bounds.y;
    _widget_select_internal__stretch_rect(m, bounds);
    if (alpha != NULL_POINTER)
    {
        *alpha = stdlib__cosf(_widget_select_internal__clamp(stdlib__fabsf(distance) / reach, 0.0f, 1.0f) * _WIDGET_SELECT_INTERNAL__HALF_PI);
    }
    if (nearness != NULL_POINTER)
    {
        *nearness = _widget_select_internal__clamp(1.0f - stdlib__fabsf(distance), 0.0f, 1.0f);
    }
    return TRUE;
}

/**
 * @brief the rectangle of a chip; walks the options before it, so a loop over all chips keeps its own running x instead of calling this
 */
static boole _widget_select_internal__chip(gui_node_t* n, const _widget_select_internal__metrics_t* m, gui_font_t* font, int64 index, gui_rect_t* bounds)
{
    gui_style_t* s = &n->resolved;
    gui_node_t* c = NULL_POINTER;
    float pad = ((s->popup_set & GUI_POPUP_SET__PADDING) ? s->option_padding : 12.0f) * m->scale;
    float gap = ((s->popup_set & GUI_POPUP_SET__GAP) ? s->popup_gap : _WIDGET_SELECT_INTERNAL__CHIP_GAP) * m->scale;
    float x = n->bounds.x - m->scroll;
    float w = 0.0f;
    int64 i = 0;

    for (c = n->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        _widget_select_internal__walked++;
        if (c->type != GUI_NODE_TYPE__OPTION)
        {
            continue;
        }
        w = ((font != NULL_POINTER) ? font__measure(font, c->text) : 0.0f) + 2.0f * pad;
        if (i == index)
        {
            bounds->x = x;
            bounds->y = n->bounds.y;
            bounds->w = w;
            bounds->h = n->bounds.h;
            _widget_select_internal__stretch_rect(m, bounds);
            return TRUE;
        }
        x += w + gap;
        i++;
    }
    return FALSE;
}

/**
 * @brief the rows whose drawn rectangle can touch the rows' area, by arithmetic on the scroll offset
 */
static void _widget_select_internal__rows_in_view(const _widget_select_internal__metrics_t* m, int64* first, int64* last)
{
    float reach = m->reach;

    *first = 0;
    *last = -1;
    if (m->rows_count <= 0 || m->row_height <= 0.0f)
    {
        return;
    }
    if (m->style == GUI_SELECT_STYLE__WHEEL)
    {
        // the rows on the visible part of the cylinder
        reach = (float)(m->visible / 2 + 1) * m->row_height;
        *first = (int64)stdlib__floorf((m->scroll - reach) / m->row_height);
        *last = (int64)stdlib__ceilf((m->scroll + reach) / m->row_height);
    }
    else
    {
        *first = (int64)stdlib__floorf((m->scroll - reach) / m->row_height) - 1;
        *last = (int64)stdlib__floorf((m->scroll + m->rows.h + reach) / m->row_height) + 1;
    }
    if (*first < 0)
    {
        *first = 0;
    }
    if (*last > m->rows_count - 1)
    {
        *last = m->rows_count - 1;
    }
    if (*last - *first + 1 > _WIDGET_SELECT_INTERNAL__VISIBLE_CAP)
    {
        *last = *first + _WIDGET_SELECT_INTERNAL__VISIBLE_CAP - 1;
    }
}

/**
 * @brief the first row that is in view when the rows are scrolled by `scroll`, kept so that stagger_rows rows follow it
 */
static int64 _widget_select_internal__first_in_view(const _widget_select_internal__metrics_t* m, float scroll)
{
    int64 first = 0;

    if (m->row_height <= 0.0f)
    {
        return 0;
    }
    first = (int64)stdlib__floorf(_widget_select_internal__clamp(scroll, 0.0f, m->max_scroll) / m->row_height);
    if (first > m->rows_count - m->stagger_rows)
    {
        first = m->rows_count - m->stagger_rows;
    }
    if (first < 0)
    {
        first = 0;
    }
    return first;
}

/**
 * @brief the row drawn under a point of an open select, -1 for none. The drawn position counts, not the resting one; of two rows that overlap while they move, the one painted later
 */
static int64 _widget_select_internal__row_at(const _widget_select_internal__metrics_t* m, float x, float y)
{
    gui_rect_t row;
    float sine = 0.0f;
    float limit = 0.0f;
    int64 first = 0;
    int64 last = 0;
    int64 i = 0;

    stdlib__null_memory(&row, sizeof(row));
    if (_widget_select_internal__contains(m->rows, x, y) == FALSE || m->rows_count <= 0 || m->row_height <= 0.0f)
    {
        return -1;
    }
    if (m->bar.w > 0.0f && _widget_select_internal__contains(m->bar, x, y) == TRUE)
    {
        return -1;
    }
    if (m->style == GUI_SELECT_STYLE__WHEEL)
    {
        // from the height on the cylinder back to the angle, then to the row
        y = _widget_select_internal__stretch_point(m, y, TRUE);
        sine = (y - (m->rows.y + m->rows.h * 0.5f)) / m->wheel_radius;
        limit = stdlib__sinf(_WIDGET_SELECT_INTERNAL__WHEEL_END);
        if (sine <= -limit || sine >= limit)
        {
            return -1;
        }
        i = (int64)_widget_select_internal__round(m->scroll / m->row_height + stdlib__atan2f(sine, stdlib__sqrtf(1.0f - sine * sine)) / m->wheel_step);
        return (i >= 0 && i < m->rows_count) ? i : -1;
    }
    // only the rows within `reach` of the point can be drawn under it
    first = (int64)stdlib__floorf((y - m->rows.y + m->scroll - m->reach) / m->row_height) - 1;
    last = (int64)stdlib__floorf((y - m->rows.y + m->scroll + m->reach) / m->row_height) + 1;
    if (first < 0)
    {
        first = 0;
    }
    if (last > m->rows_count - 1)
    {
        last = m->rows_count - 1;
    }
    for (i = last; i >= first; i--)
    {
        if (_widget_select_internal__row(m, i, &row, NULL_POINTER) == TRUE && _widget_select_internal__contains(row, x, y) == TRUE)
        {
            return i;
        }
    }
    return -1;
}

/**
 * @brief the option under a point of a segmented control, a row of chips or a carousel, -1 for none
 */
static int64 _widget_select_internal__inline_at(gui_node_t* n, const _widget_select_internal__metrics_t* m, float x, float y)
{
    gui_style_t* s = &n->resolved;
    gui_node_t* c = NULL_POINTER;
    gui_font_t* font = NULL_POINTER;
    float pad = 0.0f;
    float gap = 0.0f;
    float from = 0.0f;
    float w = 0.0f;
    int64 i = 0;

    if (_widget_select_internal__contains(n->bounds, x, y) == FALSE || m->count <= 0)
    {
        return -1;
    }
    if (m->style == GUI_SELECT_STYLE__SEGMENTED)
    {
        i = (m->row_height > 0.0f) ? (int64)stdlib__floorf((x - n->bounds.x) / m->row_height) : 0;
        return (i >= 0 && i < m->count) ? i : -1;
    }
    if (m->style == GUI_SELECT_STYLE__CAROUSEL)
    {
        x = _widget_select_internal__stretch_point(m, x, TRUE);
        i = (m->row_height > 0.0f) ? (int64)_widget_select_internal__round(m->scroll / m->row_height + (x - (n->bounds.x + n->bounds.w * 0.5f)) / m->row_height) : 0;
        return (i >= 0 && i < m->count) ? i : -1;
    }
    font = _widget_select_internal__font(n, m->scale);
    pad = ((s->popup_set & GUI_POPUP_SET__PADDING) ? s->option_padding : 12.0f) * m->scale;
    gap = ((s->popup_set & GUI_POPUP_SET__GAP) ? s->popup_gap : _WIDGET_SELECT_INTERNAL__CHIP_GAP) * m->scale;
    from = n->bounds.x - m->scroll;
    x = _widget_select_internal__stretch_point(m, x, TRUE);
    for (c = n->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        _widget_select_internal__walked++;
        if (c->type != GUI_NODE_TYPE__OPTION)
        {
            continue;
        }
        w = ((font != NULL_POINTER) ? font__measure(font, c->text) : 0.0f) + 2.0f * pad;
        if (x >= from && x < from + w)
        {
            return i;
        }
        from += w + gap;
        i++;
    }
    return -1;
}

gui_rect_t scene__select_popup_bounds(gui_node_t* n)
{
    _widget_select_internal__metrics_t m;
    gui_rect_t none;

    stdlib__null_memory(&none, sizeof(none));
    if (n == NULL_POINTER || n->type != GUI_NODE_TYPE__SELECT)
    {
        return none;
    }
    _widget_select_internal__metrics(n, &m);
    return m.rest;
}

boole scene__select_option_bounds(gui_node_t* n, int64 index, gui_rect_t* bounds, float* alpha)
{
    _widget_select_internal__metrics_t m;
    boole shown = FALSE;

    if (alpha != NULL_POINTER)
    {
        *alpha = 0.0f;
    }
    if (n == NULL_POINTER || n->type != GUI_NODE_TYPE__SELECT || bounds == NULL_POINTER)
    {
        return FALSE;
    }
    _widget_select_internal__metrics(n, &m);
    if (index < 0 || index >= m.count)
    {
        return FALSE;
    }
    if (m.style == GUI_SELECT_STYLE__WHEEL)
    {
        return _widget_select_internal__wheel_row(&m, index, bounds, alpha, NULL_POINTER);
    }
    if (m.style == GUI_SELECT_STYLE__CAROUSEL)
    {
        return _widget_select_internal__carousel_item(n, &m, index, bounds, alpha, NULL_POINTER);
    }
    if (m.style == GUI_SELECT_STYLE__SEGMENTED)
    {
        bounds->x = n->bounds.x + (float)index * m.row_height;
        bounds->y = n->bounds.y;
        bounds->w = m.row_height;
        bounds->h = n->bounds.h;
        shown = TRUE;
    }
    else if (m.style == GUI_SELECT_STYLE__CHIPS)
    {
        shown = _widget_select_internal__chip(n, &m, _widget_select_internal__font(n, m.scale), index, bounds);
        shown = (boole)(shown == TRUE && bounds->x < n->bounds.x + n->bounds.w && bounds->x + bounds->w > n->bounds.x);
    }
    if (_widget_select_internal__is_inline(m.style) == TRUE)
    {
        if (alpha != NULL_POINTER && shown == TRUE)
        {
            *alpha = 1.0f;
        }
        return shown;
    }
    return _widget_select_internal__row(&m, _widget_select_internal__row_of_option(&m, _widget_select_internal__state(n), index), bounds, alpha);
}

gui_rect_t scene__select_part_bounds(gui_node_t* n, gui_select_part_e part)
{
    _widget_select_internal__metrics_t m;
    gui_rect_t none;

    stdlib__null_memory(&none, sizeof(none));
    if (n == NULL_POINTER || n->type != GUI_NODE_TYPE__SELECT)
    {
        return none;
    }
    _widget_select_internal__metrics(n, &m);
    switch (part)
    {
        case GUI_SELECT_PART__PANEL:   return m.panel;
        case GUI_SELECT_PART__ROWS:    return m.rows;
        case GUI_SELECT_PART__HEADER:  return (m.header.h > 0.0f) ? m.header : none;
        case GUI_SELECT_PART__FOOTER:  return (m.footer.h > 0.0f) ? m.footer : none;
        case GUI_SELECT_PART__CONFIRM: return m.confirm;
        case GUI_SELECT_PART__CANCEL:  return m.cancel;
        case GUI_SELECT_PART__BACK:    return m.back;
        case GUI_SELECT_PART__BAR:     return m.bar;
        default:                       return none;
    }
}

const char* scene__select_query(gui_node_t* n)
{
    _widget_select_internal__state_t* state = NULL_POINTER;

    if (n == NULL_POINTER || n->type != GUI_NODE_TYPE__SELECT || n->resolved.select_style != GUI_SELECT_STYLE__SEARCH)
    {
        return "";
    }
    state = _widget_select_internal__state(n);
    return (state != NULL_POINTER) ? state->query : "";
}

int64 scene__select_match_count(gui_node_t* n)
{
    _widget_select_internal__metrics_t m;

    if (n == NULL_POINTER || n->type != GUI_NODE_TYPE__SELECT)
    {
        return 0;
    }
    _widget_select_internal__metrics(n, &m);
    return m.rows_count;
}

int64 scene__select_walked(boole reset)
{
    int64 walked = _widget_select_internal__walked;

    if (reset == TRUE)
    {
        _widget_select_internal__walked = 0;
    }
    return walked;
}

int64 scene__select_pending(gui_node_t* n)
{
    _widget_select_internal__state_t* state = NULL_POINTER;

    if (n == NULL_POINTER || n->type != GUI_NODE_TYPE__SELECT || n->resolved.select_style != GUI_SELECT_STYLE__DIALOG || n->is_open == FALSE)
    {
        return -1;
    }
    state = _widget_select_internal__state(n);
    return (state != NULL_POINTER) ? state->pending : -1;
}

// ===== time ==================================================================

/**
 * @brief advance the reveal clock by the frame time
 * @return boole -> TRUE while the entrance or whole-popup fade runs
 */
static boole _widget_select_internal__tick(gui_node_t* n, _widget_select_internal__metrics_t* m)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    int64 now = scene__frame_time_ms();
    float elapsed = (state != NULL_POINTER && now > state->reveal_ms) ? (float)(now - state->reveal_ms) : 0.0f;
    float step = 1.0f;

    if (m->duration > 0.0f && m->total > 0.0f)
    {
        step = elapsed / m->total;
    }
    if (n->is_open == TRUE)
    {
        n->widget_anim += step;
    }
    if (state != NULL_POINTER)
    {
        state->reveal_ms = now;
        step = m->duration > 0.0f ? elapsed / m->duration : 1.0f;
        state->presence = _widget_select_internal__clamp(state->presence + (n->is_open ? step : -step), 0.0f, 1.0f);
        if (!n->is_open && state->presence <= 0.0f)
        {
            n->widget_anim = 0.0f;
        }
    }
    n->widget_anim = _widget_select_internal__clamp(n->widget_anim, 0.0f, 1.0f);
    return (boole)((n->is_open == TRUE) ? n->widget_anim < 1.0f || (state != NULL_POINTER && state->presence < 1.0f) : n->widget_anim > 0.0f);
}

/**
 * @brief move the shown offset toward scroll_target by the frame time, and a released sheet back to its place
 * @return boole -> TRUE while something has not come to rest
 */
static boole _widget_select_internal__scroll_step(gui_node_t* n, _widget_select_internal__metrics_t* m)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    gui_style_t* s = &n->resolved;
    int64 now = scene__frame_time_ms();
    float smooth = _widget_select_internal__smooth(n);
    float elapsed = 0.0f;
    float delta = 0.0f;
    boole moving = FALSE;

    if (state == NULL_POINTER)
    {
        return FALSE;
    }
    elapsed = (now > state->scroll_ms) ? (float)(now - state->scroll_ms) : 0.0f;
    state->scroll_ms = now;
    delta = state->scroll_target - state->scroll;
    if (state->thumb_held == TRUE || (state->drag_held == TRUE && state->drag_moved == TRUE))
    {
        // the thumb or the content is held: the input places the offset
        moving = FALSE;
    }
    else if (scroll__wobble_enabled(s) == TRUE && motion__reduced_motion() == FALSE)
    {
        if (delta != 0.0f || state->scroll_velocity != 0.0f)
        {
            moving = scroll__wobble_step(s, &state->scroll, &state->scroll_velocity, state->scroll_target, elapsed);
        }
    }
    else
    {
        moving = scroll__smooth_step(&state->scroll, state->scroll_target, smooth, elapsed);
        if (moving == FALSE)
        {
            state->scroll_velocity = 0.0f;
        }
    }
    if (state->sheet_held == FALSE && state->sheet_pull > 0.0f)
    {
        state->sheet_pull -= state->sheet_pull * _widget_select_internal__clamp(elapsed / 80.0f, 0.0f, 1.0f);
        if (state->sheet_pull < 0.5f)
        {
            state->sheet_pull = 0.0f;
        }
        moving = TRUE;
    }
    m->scroll = state->scroll;
    m->velocity = state->scroll_velocity;
    moving = (boole)(_widget_select_internal__stretch_step(n, m, elapsed) || moving);
    _widget_select_internal__stretch_metrics(n, m);
    return moving;
}

/**
 * @brief move the resting place by a number of device pixels; with the wobble, the part past an edge pushes the content over that edge
 */
static void _widget_select_internal__scroll_by(gui_node_t* n, const _widget_select_internal__metrics_t* m, float pixels)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    gui_style_t* s = &n->resolved;
    float wanted = 0.0f;
    float excess = 0.0f;
    float response = 0.0f;

    if (state == NULL_POINTER)
    {
        return;
    }
    wanted = state->scroll_target + pixels;
    state->scroll_target = _widget_select_internal__clamp(wanted, 0.0f, m->max_scroll);
    excess = wanted - state->scroll_target;
    if (excess != 0.0f && scroll__wobble_enabled(s) == TRUE && motion__reduced_motion() == FALSE)
    {
        // the spring's rate 2 / response turns a distance into the speed that carries the content about that far
        response = (s->scroll_smooth_ms > 0.0f) ? s->scroll_smooth_ms : 160.0f;
        state->scroll_velocity += excess * (2.0f / response) * s->scroll_end_effect_amount;
    }
    if (scroll__wobble_enabled(s) == FALSE && _widget_select_internal__smooth(n) <= 0.0f)
    {
        state->scroll = state->scroll_target;
    }
    n->widget_animating = TRUE;
}

/**
 * @brief scroll a stacked list just far enough that the row is in view
 */
static void _widget_select_internal__scroll_to_row(gui_node_t* n, const _widget_select_internal__metrics_t* m, int64 row)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    float top = (float)row * m->row_height;

    if (state == NULL_POINTER || row < 0)
    {
        return;
    }
    if (top < state->scroll_target)
    {
        _widget_select_internal__scroll_by(n, m, top - state->scroll_target);
    }
    else if (top + m->row_height > state->scroll_target + m->rows.h)
    {
        _widget_select_internal__scroll_by(n, m, top + m->row_height - m->rows.h - state->scroll_target);
    }
}

/**
 * @brief place the content for the current value without movement: a list with the current row in the middle of the view, a wheel or a carousel on it, the mark of a segmented control under it
 */
static void _widget_select_internal__show_current(gui_node_t* n)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    _widget_select_internal__metrics_t m;
    int64 row = 0;

    if (state == NULL_POINTER)
    {
        return;
    }
    _widget_select_internal__metrics(n, &m);
    if (_widget_select_internal__is_turning(m.style) == TRUE || m.style == GUI_SELECT_STYLE__SEGMENTED)
    {
        state->scroll_target = _widget_select_internal__clamp((float)(int64)n->value * m.row_height, 0.0f, m.max_scroll);
    }
    else if (m.style == GUI_SELECT_STYLE__CHIPS)
    {
        state->scroll_target = _widget_select_internal__clamp(state->scroll_target, 0.0f, m.max_scroll);
    }
    else
    {
        row = _widget_select_internal__row_of_option(&m, state, (int64)n->value);
        state->scroll_target = _widget_select_internal__clamp(((float)((row >= 0) ? row : 0) + 0.5f) * m.row_height - m.rows.h * 0.5f, 0.0f, m.max_scroll);
    }
    state->scroll = state->scroll_target;
    state->scroll_far = state->scroll;
    state->stretch_anchored = FALSE;
    state->scroll_velocity = 0.0f;
    state->scroll_ms = scene__frame_time_ms();
    state->thumb_held = FALSE;
    state->press_on_bar = FALSE;
    state->drag_held = FALSE;
    state->drag_moved = FALSE;
    state->sheet_held = FALSE;
    state->sheet_pull = 0.0f;
    m.scroll = state->scroll;
    state->stagger_first = _widget_select_internal__first_in_view(&m, state->scroll);
}

/**
 * @brief take an option as the value; on_change is called when it differs. The handler is allowed to destroy n: nothing reads n after this call
 */
static void _widget_select_internal__commit(gui_node_t* n, int64 option, boole close)
{
    boole changed = (boole)((int64)n->value != option);

    n->value = (float)option;
    _widget_select_internal__sync_label(n);
    if (close == TRUE)
    {
        scene__select_open(n, FALSE);
    }
    if (changed == TRUE)
    {
        scene__dispatch_change(n, (float)option);
    }
}

/**
 * @brief wheel, carousel, segmented: head for an option and take it as the value; the movement is drawn in the frames that follow
 */
static void _widget_select_internal__turn_to(gui_node_t* n, const _widget_select_internal__metrics_t* m, int64 option)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);

    if (state == NULL_POINTER || m->count <= 0)
    {
        return;
    }
    if (option < 0)
    {
        option = 0;
    }
    if (option > m->count - 1)
    {
        option = m->count - 1;
    }
    state->scroll_target = (float)option * m->row_height;
    if (scroll__wobble_enabled(&n->resolved) == FALSE && _widget_select_internal__smooth(n) <= 0.0f)
    {
        state->scroll = state->scroll_target;
    }
    n->widget_animating = TRUE;
    _widget_select_internal__commit(n, option, FALSE);
}

/**
 * @brief close a sheet from where it was pulled to: the reveal clock is set to the place the sheet is at, so the slide out continues from there
 */
static void _widget_select_internal__close_pulled_sheet(gui_node_t *n, const _widget_select_internal__metrics_t *m)
{
    (void)m;
    // Keep the released position throughout the whole-popup fade.
    scene__select_open(n, FALSE);
}

void scene__select_open(gui_node_t *n, boole open)
{
    _widget_select_internal__state_t *state = NULL_POINTER;
    _widget_select_internal__metrics_t m;
    gui_node_t *previous = NULL_POINTER;
    gui_rect_t full = { 0 };
    int64 vw = 0;
    int64 vh = 0;
    boole fresh = FALSE;

    if (n == NULL_POINTER || n->type != GUI_NODE_TYPE__SELECT || _widget_select_internal__is_inline(n->resolved.select_style))
    {
        return;
    }
    if (n->is_open == open)
    {
        return;
    }
    state = _widget_select_internal__state(n);
    fresh = (boole)(n->widget_anim <= 0.0f);
    n->is_open = open;
    if (state != NULL_POINTER)
    {
        state->reveal_ms = scene__frame_time_ms();
    }
    n->widget_animating = TRUE;
    if (open)
    {
        previous = scene__overlay_node();
        if (previous != NULL_POINTER && previous != n)
        {
            previous->is_open = FALSE;
            previous->widget_anim = 0.0f;
            previous->widget_animating = FALSE;
        }
        if (state != NULL_POINTER)
        {
            state->pending = (int64)n->value;
            state->highlight = -1;
            if (fresh)
            {
                state->presence = 1.0f;
                state->query[0] = 0;
                state->query_len = 0;
                _widget_select_internal__show_current(n);
            }
            state->scroll_ms = scene__frame_time_ms();
        }
        scene__set_focus(n);
        scene__viewport(&vw, &vh);
        full.w = (float)vw;
        full.h = (float)vh;
        scene__set_overlay(n, full, _widget_select_internal__draw_popup);
    }
    else if (state != NULL_POINTER)
    {
        state->thumb_held = FALSE;
        state->press_on_bar = FALSE;
        state->drag_held = FALSE;
        state->drag_moved = FALSE;
        state->sheet_held = FALSE;
    }
    if (!open && n->resolved.select_style == GUI_SELECT_STYLE__SEARCH && scene__focus() == n)
    {
        scene__set_focus(NULL_POINTER);
    }
    _widget_select_internal__metrics(n, &m);
    if (m.duration <= 0.0f)
    {
        n->widget_anim = open ? 1.0f : 0.0f;
        if (state != NULL_POINTER)
        {
            state->presence = n->widget_anim;
        }
        n->widget_animating = FALSE;
        if (!open && scene__overlay_node() == n)
        {
            scene__set_overlay(NULL_POINTER, full, NULL_POINTER);
        }
    }
}

/**
 * @brief copy the current option's text into the control's own text, so drawing the closed control walks no children
 */
static void _widget_select_internal__sync_label(gui_node_t* n)
{
    gui_node_t* option = _widget_select_internal__option_at(n, (int64)n->value);
    int64 len = 0;

    if (option == NULL_POINTER)
    {
        n->text[0] = 0;
        n->text_len = 0;
        return;
    }
    len = (int64)stdlib__strlen(option->text);
    if (scene__set_text(n, option->text, len) == FALSE)
    {
        // Never display the previously selected option after a failed copy.
        scene__set_text(n, NULL_POINTER, 0);
    }
}

/**
 * @brief one line of text, centred between y and y + h
 */
static void _widget_select_internal__draw_text(gui_node_t* n, gui_font_t* font, float scale, float x, float y, float h, gui_color_t color, const char* text)
{
    float line = 0.0f;

    if (font == NULL_POINTER || text == NULL_POINTER || text[0] == 0 || color.a <= 0.001f)
    {
        return;
    }
    line = font__line_height_for(font, &n->resolved, scale);
    font__draw(font, x, y + (h - line) * 0.5f + font__baseline_for(font, &n->resolved, scale), color, (char*)text);
}

/**
 * @brief a check mark inside the square of `size` at x, y
 */
static void _widget_select_internal__draw_check(float x, float y, float size, float thickness, gui_color_t color)
{
    gui_vec2_t a;
    gui_vec2_t b;
    gui_vec2_t c;

    a.x = x + size * 0.10f;
    a.y = y + size * 0.52f;
    b.x = x + size * 0.40f;
    b.y = y + size * 0.82f;
    c.x = x + size * 0.92f;
    c.y = y + size * 0.20f;
    renderer__submit_line(a, b, thickness, color);
    renderer__submit_line(b, c, thickness, color);
}

/**
 * @brief what stands above the rows: the handle and title of a sheet, the title bar of a page or a dialog, the text field of a search
 */
static void _widget_select_internal__draw_header(gui_node_t* n, const _widget_select_internal__metrics_t* m, gui_font_t* font, float alpha)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    gui_style_t* s = &n->resolved;
    gui_color_t color = _widget_select_internal__text_color(s);
    gui_color_t line = _widget_select_internal__separator_color(s);
    gui_color_t faint;
    gui_rect_t rect;
    gui_vec2_t a;
    gui_vec2_t b;
    gui_vec2_t c;
    float pad = ((s->popup_set & GUI_POPUP_SET__PADDING) ? s->option_padding : 12.0f) * m->scale;
    float handle = _WIDGET_SELECT_INTERNAL__HANDLE_H * m->scale;
    float text_w = 0.0f;

    stdlib__null_memory(&rect, sizeof(rect));
    if (m->header.h <= 0.0f || state == NULL_POINTER)
    {
        return;
    }
    color.a *= alpha;
    line.a *= alpha;
    faint = color;
    faint.a *= 0.45f;
    if (m->style == GUI_SELECT_STYLE__SHEET)
    {
        // the handle: a short bar in the middle of its strip
        rect.w = 36.0f * m->scale;
        rect.h = 4.0f * m->scale;
        rect.x = m->header.x + (m->header.w - rect.w) * 0.5f;
        rect.y = m->header.y + (handle - rect.h) * 0.5f;
        renderer__submit_rect(rect, faint, rect.h * 0.5f);
        if (state->title[0] != 0 && font != NULL_POINTER)
        {
            text_w = font__measure(font, state->title);
            _widget_select_internal__draw_text(n, font, m->scale, m->header.x + (m->header.w - text_w) * 0.5f, m->header.y + handle, m->header.h - handle, color, state->title);
        }
        return;
    }
    if (m->style == GUI_SELECT_STYLE__PAGE)
    {
        // the arrow back: two lines that meet on the left
        a.x = m->back.x + m->back.w * 0.58f;
        a.y = m->back.y + m->back.h * 0.30f;
        b.x = m->back.x + m->back.w * 0.38f;
        b.y = m->back.y + m->back.h * 0.50f;
        c.x = a.x;
        c.y = m->back.y + m->back.h * 0.70f;
        renderer__submit_line(a, b, 2.0f * m->scale, color);
        renderer__submit_line(b, c, 2.0f * m->scale, color);
        _widget_select_internal__draw_text(n, font, m->scale, m->back.x + m->back.w, m->header.y, m->header.h, color, state->title);
    }
    else if (m->style == GUI_SELECT_STYLE__DIALOG)
    {
        _widget_select_internal__draw_text(n, font, m->scale, m->header.x + pad + 4.0f * m->scale, m->header.y, m->header.h, color, state->title);
        return;
    }
    else if (m->style == GUI_SELECT_STYLE__SEARCH)
    {
        if (state->query_len > 0)
        {
            _widget_select_internal__draw_text(n, font, m->scale, m->header.x + pad, m->header.y, m->header.h, color, state->query);
            text_w = (font != NULL_POINTER) ? font__measure(font, state->query) : 0.0f;
        }
        else
        {
            _widget_select_internal__draw_text(n, font, m->scale, m->header.x + pad, m->header.y, m->header.h, faint, state->placeholder);
        }
        if (scene__focus() == n && font != NULL_POINTER)
        {
            // the caret, after the text
            rect.w = 2.0f * m->scale;
            rect.h = font__line_height_for(font, s, m->scale);
            rect.x = m->header.x + pad + text_w;
            rect.y = m->header.y + (m->header.h - rect.h) * 0.5f;
            renderer__submit_rect(rect, color, 0.0f);
        }
    }
    // the line under a title bar and under the text field
    rect.x = m->header.x;
    rect.y = m->header.y + m->header.h - 1.0f * m->scale;
    rect.w = m->header.w;
    rect.h = 1.0f * m->scale;
    renderer__submit_rect(rect, line, 0.0f);
}

/**
 * @brief the two buttons of a dialog
 */
static void _widget_select_internal__draw_footer(gui_node_t* n, const _widget_select_internal__metrics_t* m, gui_font_t* font, float alpha)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    gui_style_t* s = &n->resolved;
    gui_color_t color = _widget_select_internal__accent_color(s);
    gui_color_t hover;
    int64 mx = 0;
    int64 my = 0;
    boole pointer = FALSE;

    if (m->footer.h <= 0.0f || state == NULL_POINTER)
    {
        return;
    }
    hover = s->has_option_hover_background ? s->option_hover_background : scene__rgba(1.0f, 1.0f, 1.0f, 0.08f);
    color.a *= alpha;
    hover.a *= alpha;
    pointer = scene__pointer_position(&mx, &my);
    if (pointer == TRUE && _widget_select_internal__contains(m->cancel, (float)mx, (float)my) == TRUE)
    {
        renderer__submit_rect(m->cancel, hover, s->radius * m->scale);
    }
    if (pointer == TRUE && _widget_select_internal__contains(m->confirm, (float)mx, (float)my) == TRUE)
    {
        renderer__submit_rect(m->confirm, hover, s->radius * m->scale);
    }
    _widget_select_internal__draw_text(n, font, m->scale, m->cancel.x + _WIDGET_SELECT_INTERNAL__BUTTON_PAD * m->scale, m->cancel.y, m->cancel.h, color, state->cancel);
    _widget_select_internal__draw_text(n, font, m->scale, m->confirm.x + _WIDGET_SELECT_INTERNAL__BUTTON_PAD * m->scale, m->confirm.y, m->confirm.h, color, state->confirm);
}

// Option nodes keep the ordinary cascade and transitions; the select owns geometry.
static void _widget_select_internal__option_states(gui_node_t* n, const _widget_select_internal__metrics_t* m)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    gui_node_t* c = NULL_POINTER;
    gui_node_state_e next = GUI_NODE_STATE__DEFAULT;
    int64 mx = 0;
    int64 my = 0;
    int64 hover = -1;
    int64 index = 0;
    boole live = (boole)(n->is_open == TRUE || _widget_select_internal__is_inline(m->style) == TRUE);

    if (live == FALSE && n->widget_anim > 0.0f)
    {
        return;
    }
    if (live == TRUE && state != NULL_POINTER && state->drag_moved == FALSE && state->thumb_held == FALSE && state->sheet_held == FALSE)
    {
        if (scene__pointer_position(&mx, &my) == TRUE)
        {
            hover = _widget_select_internal__is_inline(m->style) ? _widget_select_internal__inline_at(n, m, (float)mx, (float)my) : _widget_select_internal__option_of_row(m, state, _widget_select_internal__row_at(m, (float)mx, (float)my));
        }
        if (hover < 0 && state->highlight >= 0)
        {
            hover = _widget_select_internal__option_of_row(m, state, state->highlight);
        }
    }
    for (c = n->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        _widget_select_internal__walked++;
        if (c->type != GUI_NODE_TYPE__OPTION)
        {
            continue;
        }
        c->value = (index == (int64)n->value) ? 1.0f : 0.0f;
        next = GUI_NODE_STATE__DEFAULT;
        if (index == hover)
        {
            next = GUI_NODE_STATE__HOVER;
            if (state != NULL_POINTER && state->drag_held == TRUE)
            {
                next = GUI_NODE_STATE__PRESSED;
            }
        }
        if (c->state != GUI_NODE_STATE__DISABLED && c->state != next)
        {
            c->state = next;
            n->widget_animating = TRUE;
        }
        index++;
    }
}

// A ring made of non-overlapping quads leaves the backdrop visible through
// the middle. The general background helper uses an opaque inset instead.
static void _widget_select_internal__border_segment(gui_vec2_t a, gui_vec2_t b, gui_vec2_t c, gui_vec2_t d, gui_rect_t row, gui_color_t color, const gradient_sampler_t* gradient, float opacity)
{
    if (gradient != NULL_POINTER)
    {
        color = gradient__sample(gradient, (a.x + b.x + c.x + d.x) * 0.25f - row.x, (a.y + b.y + c.y + d.y) * 0.25f - row.y);
    }
    color.a *= opacity;
    renderer__submit_triangle(a, b, c, color);
    renderer__submit_triangle(a, c, d, color);
}

static void _widget_select_internal__option_border(gui_node_t* option, gui_rect_t row, float scale)
{
    gui_style_t* s = &option->resolved;
    gradient_sampler_t sampler;
    const gradient_sampler_t* gradient = NULL_POINTER;
    gui_vec2_t outer[36];
    gui_vec2_t inner[36];
    gui_vec2_t a;
    gui_vec2_t b;
    gui_vec2_t c;
    gui_vec2_t d;
    float width = s->border_width * scale;
    float radius = 0.0f;
    float inset_radius = 0.0f;
    float angle = 0.0f;
    float ox = 0.0f;
    float oy = 0.0f;
    float ix = 0.0f;
    float iy = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    int corner = 0;
    int step = 0;
    int i = 0;
    int next = 0;
    int piece = 0;
    int pieces = 1;

    if (width <= 0.0f || s->border_style == GUI_BORDER_STYLE__NONE || s->has_border_color == FALSE || row.w <= 0.0f || row.h <= 0.0f)
    {
        return;
    }
    width = stdlib__fminf(width, stdlib__fminf(row.w, row.h) * 0.5f);
    radius = _widget_select_internal__clamp(s->radius * scale, 0.0f, stdlib__fminf(row.w, row.h) * 0.5f);
    inset_radius = stdlib__fmaxf(0.0f, radius - width);
    if (s->has_border_gradient == TRUE)
    {
        gradient__prepare(&s->border_gradient, row.w, row.h, scale, &sampler);
        gradient = &sampler;
    }
    for (corner = 0; corner < 4; corner++)
    {
        ox = (corner == 0 || corner == 3) ? row.x + row.w - radius : row.x + radius;
        oy = corner < 2 ? row.y + row.h - radius : row.y + radius;
        ix = (corner == 0 || corner == 3) ? row.x + row.w - width - inset_radius : row.x + width + inset_radius;
        iy = corner < 2 ? row.y + row.h - width - inset_radius : row.y + width + inset_radius;
        for (step = 0; step <= 8; step++)
        {
            angle = ((float)corner + (float)step / 8.0f) * _WIDGET_SELECT_INTERNAL__HALF_PI;
            i = corner * 9 + step;
            outer[i].x = ox + stdlib__cosf(angle) * radius;
            outer[i].y = oy + stdlib__sinf(angle) * radius;
            inner[i].x = ix + stdlib__cosf(angle) * inset_radius;
            inner[i].y = iy + stdlib__sinf(angle) * inset_radius;
        }
    }
    for (i = 0; i < 36; i++)
    {
        next = (i + 1) % 36;
        pieces = gradient != NULL_POINTER ? (int)(stdlib__fmaxf(stdlib__fabsf(outer[next].x - outer[i].x), stdlib__fabsf(outer[next].y - outer[i].y)) / 4.0f) + 1 : 1;
        for (piece = 0; piece < pieces; piece++)
        {
            u = (float)piece / (float)pieces;
            v = (float)(piece + 1) / (float)pieces;
            a.x = outer[i].x + (outer[next].x - outer[i].x) * u;
            a.y = outer[i].y + (outer[next].y - outer[i].y) * u;
            b.x = outer[i].x + (outer[next].x - outer[i].x) * v;
            b.y = outer[i].y + (outer[next].y - outer[i].y) * v;
            c.x = inner[i].x + (inner[next].x - inner[i].x) * v;
            c.y = inner[i].y + (inner[next].y - inner[i].y) * v;
            d.x = inner[i].x + (inner[next].x - inner[i].x) * u;
            d.y = inner[i].y + (inner[next].y - inner[i].y) * u;
            _widget_select_internal__border_segment(a, b, c, d, row, s->border_color, gradient, option->effective_opacity);
        }
    }
}

static void _widget_select_internal__option_bg(gui_node_t* n, gui_node_t* option, gui_rect_t row, float alpha, gui_color_t fallback, float scale)
{
    gui_rect_t saved;
    float opacity = 0.0f;
    float option_opacity = 1.0f;
    gui_border_style_e border_style = GUI_BORDER_STYLE__NONE;
    gui_style_t* s = (option != NULL_POINTER) ? &option->resolved : NULL_POINTER;

    option_opacity = (s != NULL_POINTER && s->has_opacity) ? s->opacity : 1.0f;
    if (s == NULL_POINTER || (s->has_background_color == FALSE && s->has_bg_gradient == FALSE && s->has_background_image == FALSE))
    {
        fallback.a *= alpha * n->effective_opacity * option_opacity;
        if (fallback.a > 0.001f)
        {
            renderer__submit_rect(row, fallback, n->resolved.radius * scale);
        }
    }
    if (option == NULL_POINTER)
    {
        return;
    }
    saved = option->bounds;
    opacity = option->effective_opacity;
    option->bounds = row;
    option->effective_opacity = alpha * n->effective_opacity * option_opacity;
    border_style = s->border_style;
    s->border_style = GUI_BORDER_STYLE__NONE;
    scene__emit_default_bg(option, scale);
    s->border_style = border_style;
    _widget_select_internal__option_border(option, row, scale);
    option->bounds = saved;
    option->effective_opacity = opacity;
}

static gui_color_t _widget_select_internal__option_color(gui_node_t* n, gui_node_t* option, gui_color_t fallback, float alpha)
{
    if (option != NULL_POINTER)
    {
        if (option->style[GUI_NODE_STATE__DEFAULT].has_font_color || option->style[option->state].has_font_color)
        {
            fallback = option->resolved.font_color;
        }
        if (option->resolved.has_opacity)
        {
            alpha *= option->resolved.opacity;
        }
    }
    fallback.a *= alpha * n->effective_opacity;
    return fallback;
}

/**
 * @brief the rows of a stacked style, with the mark the style puts on the current option
 */
static void _widget_select_internal__draw_rows(gui_node_t* n, const _widget_select_internal__metrics_t* m, gui_font_t* font)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    gui_style_t* s = &n->resolved;
    gui_node_t* options[_WIDGET_SELECT_INTERNAL__VISIBLE_CAP];
    gui_rect_t row;
    gui_color_t background;
    gui_color_t color;
    gui_color_t accent;
    gui_color_t inside;
    gui_vec2_t centre;
    int64 mx = 0;
    int64 my = 0;
    int64 hover = -1;
    int64 marked = -1;
    int64 first = 0;
    int64 last = -1;
    int64 option = 0;
    int64 i = 0;
    float alpha = 0.0f;
    float pad = ((s->popup_set & GUI_POPUP_SET__PADDING) ? s->option_padding : 12.0f) * m->scale;
    float mark = _WIDGET_SELECT_INTERNAL__MARK * m->scale;
    float text_x = 0.0f;
    boole overlapping = FALSE;
    boole held = (boole)(state != NULL_POINTER && (state->thumb_held == TRUE || state->drag_moved == TRUE || state->sheet_held == TRUE));

    stdlib__null_memory(&row, sizeof(row));
    stdlib__null_memory(&background, sizeof(background));
    stdlib__null_memory(&color, sizeof(color));
    if (held == FALSE && scene__pointer_position(&mx, &my) == TRUE)
    {
        hover = _widget_select_internal__row_at(m, (float)mx, (float)my);
    }
    // the option that carries the style's mark: the value, or what a dialog has marked so far
    marked = (m->style == GUI_SELECT_STYLE__DIALOG && state != NULL_POINTER) ? state->pending : (int64)n->value;
    // rows can overlap while they move into place or trail a scroll
    overlapping = (boole)(m->clock < m->total);
    _widget_select_internal__rows_in_view(m, &first, &last);
    _widget_select_internal__options_of_rows(n, m, state, first, last, options);
    for (i = first; i <= last; i++)
    {
        if (_widget_select_internal__row(m, i, &row, &alpha) == FALSE)
        {
            continue;
        }
        option = _widget_select_internal__option_of_row(m, state, i);
        stdlib__null_memory(&background, sizeof(background));
        color = _widget_select_internal__text_color(s);
        accent = _widget_select_internal__accent_color(s);
        if (option == (int64)n->value)
        {
            // dialog, page and menu show the current option by their mark; the others by this fill
            if (m->style != GUI_SELECT_STYLE__DIALOG && m->style != GUI_SELECT_STYLE__PAGE && m->style != GUI_SELECT_STYLE__MENU)
            {
                background = s->has_option_current_background ? s->option_current_background : accent;
                if (!s->has_option_current_background)
                {
                    background.a = 0.25f;
                }
            }
            else if (s->has_option_current_background)
            {
                background = s->option_current_background;
            }
            if (s->has_option_current_color)
            {
                color = s->option_current_color;
            }
        }
        if (i == hover || (state != NULL_POINTER && i == state->highlight))
        {
            if (s->has_option_hover_background)
            {
                background = s->option_hover_background;
            }
            else if (m->style != GUI_SELECT_STYLE__LIST)
            {
                background = scene__rgba(1.0f, 1.0f, 1.0f, 0.06f);
            }
            if (s->has_option_hover_color)
            {
                color = s->option_hover_color;
            }
        }
        color = _widget_select_internal__option_color(n, options[i - first], color, alpha);
        accent.a *= alpha;
        if (overlapping == TRUE)
        {
            renderer__flush_pending_draws();
        }
        _widget_select_internal__option_bg(n, options[i - first], row, alpha, background, m->scale);
        text_x = row.x + pad;
        if (m->style == GUI_SELECT_STYLE__MENU)
        {
            if (option == marked)
            {
                _widget_select_internal__draw_check(row.x + pad, row.y + (row.h - mark) * 0.5f, mark, 2.0f * m->scale, accent);
            }
            text_x += mark + _WIDGET_SELECT_INTERNAL__MARK_GAP * m->scale;
        }
        else if (m->style == GUI_SELECT_STYLE__PAGE)
        {
            if (option == marked)
            {
                _widget_select_internal__draw_check(row.x + row.w - pad - mark, row.y + (row.h - mark) * 0.5f, mark, 2.0f * m->scale, accent);
            }
        }
        else if (m->style == GUI_SELECT_STYLE__DIALOG)
        {
            // a ring, filled in the middle for the marked option
            centre.x = row.x + pad + mark * 0.5f;
            centre.y = row.y + row.h * 0.5f;
            inside = _widget_select_internal__panel_color(s);
            inside.a *= alpha;
            renderer__submit_circle(centre, mark * 0.5f, (option == marked) ? accent : color);
            renderer__submit_circle(centre, mark * 0.5f - 2.0f * m->scale, inside);
            if (option == marked)
            {
                renderer__submit_circle(centre, mark * 0.25f, accent);
            }
            text_x += mark + _WIDGET_SELECT_INTERNAL__MARK_GAP * m->scale;
        }
        if (options[i - first] != NULL_POINTER)
        {
            _widget_select_internal__draw_text(options[i - first], _widget_select_internal__font(options[i - first], m->scale), m->scale, text_x, row.y, row.h, color, options[i - first]->text);
        }
    }
    if (m->bar.w > 0.0f)
    {
        // over the rows: the rows' batch is drawn first
        renderer__flush_pending_draws();
        scroll__draw_vbar_in(n, m->rows, m->content, m->scroll, n->widget_anim * m->presence, m->scale);
    }
}

/**
 * @brief the rows of a wheel: the band of the middle row, then the rows on the cylinder
 */
static void _widget_select_internal__draw_wheel(gui_node_t* n, const _widget_select_internal__metrics_t* m, gui_font_t* font)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    gui_style_t* s = &n->resolved;
    gui_node_t* options[_WIDGET_SELECT_INTERNAL__VISIBLE_CAP];
    gui_rect_t row;
    gui_rect_t band;
    gui_rect_t line;
    gui_color_t color;
    gui_color_t current;
    gui_color_t fill;
    gui_color_t separator = _widget_select_internal__separator_color(s);
    int64 first = 0;
    int64 last = -1;
    int64 i = 0;
    float alpha = 0.0f;
    float nearness = 0.0f;
    float pad = ((s->popup_set & GUI_POPUP_SET__PADDING) ? s->option_padding : 12.0f) * m->scale;
    float text_x = 0.0f;

    stdlib__null_memory(&row, sizeof(row));
    stdlib__null_memory(&band, sizeof(band));
    stdlib__null_memory(&line, sizeof(line));
    stdlib__null_memory(&fill, sizeof(fill));
    band.x = m->rows.x;
    band.w = m->rows.w;
    band.h = m->row_height;
    band.y = m->rows.y + (m->rows.h - band.h) * 0.5f;
    if (s->has_option_current_background)
    {
        fill = s->option_current_background;
        fill.a *= m->enter;
        renderer__submit_rect(band, fill, s->radius * m->scale);
    }
    separator.a *= m->enter;
    line = band;
    line.h = 1.0f * m->scale;
    renderer__submit_rect(line, separator, 0.0f);
    line.y = band.y + band.h - line.h;
    renderer__submit_rect(line, separator, 0.0f);

    current = s->has_option_current_color ? s->option_current_color : _widget_select_internal__text_color(s);
    _widget_select_internal__rows_in_view(m, &first, &last);
    _widget_select_internal__options_of_rows(n, m, state, first, last, options);
    for (i = first; i <= last; i++)
    {
        if (_widget_select_internal__wheel_row(m, i, &row, &alpha, &nearness) == FALSE || options[i - first] == NULL_POINTER)
        {
            continue;
        }
        // the colour passes from the rows' to the middle row's as a row comes to the middle
        color = _widget_select_internal__mix(_widget_select_internal__text_color(s), current, nearness);
        stdlib__null_memory(&fill, sizeof(fill));
        if (options[i - first]->state == GUI_NODE_STATE__HOVER || options[i - first]->state == GUI_NODE_STATE__PRESSED)
        {
            fill = s->has_option_hover_background ? s->option_hover_background : scene__rgba(1.0f, 1.0f, 1.0f, 0.06f);
            if (s->has_option_hover_color == TRUE)
            {
                color = s->option_hover_color;
            }
        }
        _widget_select_internal__option_bg(n, options[i - first], row, alpha, fill, m->scale);
        color = _widget_select_internal__option_color(n, options[i - first], color, alpha);
        font = _widget_select_internal__font(options[i - first], m->scale);
        text_x = row.x + pad;
        if (s->halign == GUI_HALIGN__CENTER && font != NULL_POINTER)
        {
            text_x = row.x + (row.w - font__measure(font, options[i - first]->text)) * 0.5f;
        }
        else if (s->halign == GUI_HALIGN__RIGHT && font != NULL_POINTER)
        {
            text_x = row.x + row.w - pad - font__measure(font, options[i - first]->text);
        }
        _widget_select_internal__draw_text(options[i - first], _widget_select_internal__font(options[i - first], m->scale), m->scale, text_x, row.y, row.h, color, options[i - first]->text);
    }
}

// ===== drawing: the open select ==============================================

static void _widget_select_internal__draw_popup(gui_node_t* n, float scale)
{
    _widget_select_internal__metrics_t m;
    gui_style_t* s = &n->resolved;
    gui_font_t* font = NULL_POINTER;
    gui_rect_t full;
    gui_rect_t none;
    gui_rect_t body;
    gui_color_t dim;
    gui_color_t panel;
    gui_color_t shadow;
    float fade = 0.0f;
    boole revealing = FALSE;
    boole scrolling = FALSE;

    stdlib__null_memory(&full, sizeof(full));
    stdlib__null_memory(&none, sizeof(none));
    stdlib__null_memory(&body, sizeof(body));
    stdlib__null_memory(&dim, sizeof(dim));
    stdlib__null_memory(&shadow, sizeof(shadow));

    // the clock first: the panel's place this frame depends on it
    _widget_select_internal__metrics(n, &m);
    revealing = _widget_select_internal__tick(n, &m);
    if (n->is_open == FALSE && n->widget_anim <= 0.0f)
    {
        n->widget_animating = FALSE;
        if (scene__overlay_node() == n)
        {
            scene__set_overlay(NULL_POINTER, none, NULL_POINTER);
        }
        return;
    }
    scrolling = n->is_open ? _widget_select_internal__scroll_step(n, &m) : FALSE;
    // again, with the clock, the offset and a sheet's pull of this frame
    _widget_select_internal__metrics(n, &m);
    n->widget_animating = (boole)(revealing == TRUE || scrolling == TRUE);
    _widget_select_internal__option_states(n, &m);

    // the overlay covers the viewport of this frame: input and drawing use the same geometry after a resize
    full.w = m.view_w;
    full.h = m.view_h;
    scene__set_overlay(n, full, _widget_select_internal__draw_popup);
    fade = n->widget_anim * m.presence;

    // the scissor flushes the page, so the blur reads the finished backdrop
    renderer__push_scissor(full);
    if (s->popup_blur > 0.0f && fade > 0.0f)
    {
        renderer__blur_region(full, s->popup_blur * scale * fade);
    }
    if (s->has_popup_dim)
    {
        dim = s->popup_dim;
        dim.a *= fade;
        renderer__submit_rect(full, dim, 0.0f);
    }
    panel = _widget_select_internal__panel_color(s);
    panel.a *= fade;
    body = m.panel;
    if (m.style == GUI_SELECT_STYLE__SHEET)
    {
        // round at the top only: the lower corners are below the viewport
        body.h += s->radius * scale + 1.0f;
    }
    if (panel.a > 0.001f && body.h > 0.0f)
    {
        if (m.style == GUI_SELECT_STYLE__MENU || m.style == GUI_SELECT_STYLE__DIALOG || m.style == GUI_SELECT_STYLE__SHEET)
        {
            shadow = scene__rgba(0.0f, 0.0f, 0.0f, 0.35f * fade);
            body.y += 6.0f * scale;
            renderer__submit_shadow(body, shadow, s->radius * scale, 24.0f * scale);
            body.y -= 6.0f * scale;
        }
        renderer__submit_rect(body, panel, (m.style == GUI_SELECT_STYLE__PAGE) ? 0.0f : s->radius * scale);
    }
    font = _widget_select_internal__font(n, scale);
    _widget_select_internal__draw_header(n, &m, font, m.enter);
    if (m.rows.h > 0.0f)
    {
        renderer__push_scissor(m.rows);
        if (m.style == GUI_SELECT_STYLE__WHEEL)
        {
            _widget_select_internal__draw_wheel(n, &m, font);
        }
        else
        {
            _widget_select_internal__draw_rows(n, &m, font);
        }
        renderer__pop_scissor();
    }
    _widget_select_internal__draw_footer(n, &m, font, m.enter);
    renderer__pop_scissor();
}

// ===== drawing: the control ==================================================

/**
 * @brief segmented, chips, carousel: the options inside the control
 */
static void _widget_select_internal__draw_inline(gui_node_t* n, float scale)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    _widget_select_internal__metrics_t m;
    gui_style_t* s = &n->resolved;
    gui_font_t* font = _widget_select_internal__font(n, scale);
    gui_node_t* c = NULL_POINTER;
    gui_rect_t item;
    gui_rect_t mark;
    gui_rect_t line;
    gui_color_t color;
    gui_color_t current;
    gui_color_t fill;
    gui_color_t empty = {0};
    gui_color_t separator = _widget_select_internal__separator_color(s);
    int64 mx = 0;
    int64 my = 0;
    int64 hover = -1;
    int64 i = 0;
    float alpha = 0.0f;
    float nearness = 0.0f;
    float pad = ((s->popup_set & GUI_POPUP_SET__PADDING) ? s->option_padding : 12.0f) * scale;
    float gap = ((s->popup_set & GUI_POPUP_SET__GAP) ? s->popup_gap : _WIDGET_SELECT_INTERNAL__CHIP_GAP) * scale;
    float from = 0.0f;
    float text_w = 0.0f;
    boole shown = FALSE;

    stdlib__null_memory(&item, sizeof(item));
    stdlib__null_memory(&mark, sizeof(mark));
    stdlib__null_memory(&line, sizeof(line));
    _widget_select_internal__metrics(n, &m);
    n->widget_animating = _widget_select_internal__scroll_step(n, &m);
    _widget_select_internal__option_states(n, &m);
    if (scene__pointer_position(&mx, &my) == TRUE && (state == NULL_POINTER || state->drag_moved == FALSE))
    {
        hover = _widget_select_internal__inline_at(n, &m, (float)mx, (float)my);
    }
    current = s->has_option_current_color ? s->option_current_color : _widget_select_internal__text_color(s);
    fill = s->has_option_current_background ? s->option_current_background : _widget_select_internal__accent_color(s);
    if (!s->has_option_current_background)
    {
        fill.a = 0.25f;
    }

    // chips stand on the page; the two others on the control's own background
    if (m.style != GUI_SELECT_STYLE__CHIPS)
    {
        if (s->has_background_color || s->has_bg_gradient || s->has_shadow || s->blur_px > 0.0f)
        {
            scene__emit_default_bg(n, scale);
        }
        else
        {
            renderer__submit_rect(n->bounds, scene__rgb(0.12f, 0.13f, 0.16f), s->radius * scale);
            scene__emit_border(n, n->bounds, scale);
        }
    }
    renderer__push_scissor(n->bounds);
    if (m.style == GUI_SELECT_STYLE__SEGMENTED)
    {
        // the mark slides under the current option
        mark.x = n->bounds.x + m.scroll;
        mark.y = n->bounds.y;
        mark.w = m.row_height;
        mark.h = n->bounds.h;
        if (m.count > 0)
        {
            renderer__submit_rect(mark, fill, s->radius * scale);
        }
        line.y = n->bounds.y + n->bounds.h * 0.25f;
        line.w = 1.0f * scale;
        line.h = n->bounds.h * 0.5f;
        for (c = n->first_child; c != NULL_POINTER; c = c->next_sibling)
        {
            _widget_select_internal__walked++;
            if (c->type != GUI_NODE_TYPE__OPTION)
            {
                continue;
            }
            item.x = n->bounds.x + (float)i * m.row_height;
            item.y = n->bounds.y;
            item.w = m.row_height;
            item.h = n->bounds.h;
            if (i == hover && i != (int64)n->value && s->has_option_hover_background)
            {
                renderer__submit_rect(item, s->option_hover_background, s->radius * scale);
            }
            // a line between two options, not beside the mark
            if (i > 0 && i != (int64)n->value && i - 1 != (int64)n->value)
            {
                line.x = item.x;
                renderer__submit_rect(line, separator, 0.0f);
            }
            color = (i == (int64)n->value) ? current : _widget_select_internal__text_color(s);
            if (i == hover && s->has_option_hover_color)
            {
                color = s->option_hover_color;
            }
            _widget_select_internal__option_bg(n, c, item, 1.0f, empty, scale);
            color = _widget_select_internal__option_color(n, c, color, 1.0f);
            font = _widget_select_internal__font(c, scale);
            text_w = (font != NULL_POINTER) ? font__measure(font, c->text) : 0.0f;
            _widget_select_internal__draw_text(c, font, scale, item.x + (item.w - text_w) * 0.5f, item.y, item.h, color, c->text);
            i++;
        }
    }
    else if (m.style == GUI_SELECT_STYLE__CAROUSEL)
    {
        // the place of the value: the middle of the control
        mark.w = m.row_height;
        mark.h = n->bounds.h;
        mark.x = n->bounds.x + (n->bounds.w - mark.w) * 0.5f;
        mark.y = n->bounds.y;
        if (s->has_option_current_background)
        {
            renderer__submit_rect(mark, s->option_current_background, s->radius * scale);
        }
        line.y = n->bounds.y + n->bounds.h * 0.2f;
        line.w = 1.0f * scale;
        line.h = n->bounds.h * 0.6f;
        line.x = mark.x;
        renderer__submit_rect(line, separator, 0.0f);
        line.x = mark.x + mark.w - line.w;
        renderer__submit_rect(line, separator, 0.0f);
        for (c = n->first_child; c != NULL_POINTER; c = c->next_sibling)
        {
            _widget_select_internal__walked++;
            if (c->type != GUI_NODE_TYPE__OPTION)
            {
                continue;
            }
            if (_widget_select_internal__carousel_item(n, &m, i, &item, &alpha, &nearness) == TRUE)
            {
                color = _widget_select_internal__mix(_widget_select_internal__text_color(s), current, nearness);
                _widget_select_internal__option_bg(n, c, item, alpha, empty, scale);
                color = _widget_select_internal__option_color(n, c, color, alpha);
                font = _widget_select_internal__font(c, scale);
                text_w = (font != NULL_POINTER) ? font__measure(font, c->text) : 0.0f;
                _widget_select_internal__draw_text(c, font, scale, item.x + (item.w - text_w) * 0.5f, item.y, item.h, color, c->text);
            }
            i++;
        }
    }
    else
    {
        // chips: one pass with a running x
        from = n->bounds.x - m.scroll;
        for (c = n->first_child; c != NULL_POINTER; c = c->next_sibling)
        {
            _widget_select_internal__walked++;
            if (c->type != GUI_NODE_TYPE__OPTION)
            {
                continue;
            }
            text_w = (font != NULL_POINTER) ? font__measure(font, c->text) : 0.0f;
            item.x = from;
            item.y = n->bounds.y;
            item.w = text_w + 2.0f * pad;
            item.h = n->bounds.h;
            from += item.w + gap;
            _widget_select_internal__stretch_rect(&m, &item);
            shown = (boole)(item.x < n->bounds.x + n->bounds.w && item.x + item.w > n->bounds.x);
            if (shown == TRUE)
            {
                color = _widget_select_internal__text_color(s);
                empty = s->has_popup_background ? s->popup_background : scene__rgba(1.0f, 1.0f, 1.0f, 0.08f);
                if (i == (int64)n->value)
                {
                    empty = fill;
                    color = current;
                }
                else if (i == hover && s->has_option_hover_background)
                {
                    empty = s->option_hover_background;
                }
                if (i == hover && s->has_option_hover_color)
                {
                    color = s->option_hover_color;
                }
                _widget_select_internal__option_bg(n, c, item, 1.0f, empty, scale);
                color = _widget_select_internal__option_color(n, c, color, 1.0f);
                _widget_select_internal__draw_text(c, _widget_select_internal__font(c, scale), scale, item.x + pad, item.y, item.h, color, c->text);
            }
            i++;
        }
    }
    renderer__pop_scissor();
}

/**
 * @brief the control of a style that opens: the current option's text and the chevron
 */
static void _widget_select_internal__draw_closed(gui_node_t* n, float scale)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    gui_style_t* s = &n->resolved;
    gui_font_t* f = NULL_POINTER;
    gui_color_t background;
    gui_color_t color;
    gui_color_t chevron;
    gui_rect_t pixel;
    gui_vec2_t left;
    gui_vec2_t right;
    gui_vec2_t tip;
    float side = 10.0f * scale;
    float direction = n->is_open == TRUE ? -1.0f : 1.0f;
    float cx = n->bounds.x + n->bounds.w - 16.0f * scale;
    float cy = n->bounds.y + n->bounds.h * 0.5f;

    stdlib__null_memory(&background, sizeof(background));
    stdlib__null_memory(&color, sizeof(color));
    stdlib__null_memory(&chevron, sizeof(chevron));
    stdlib__null_memory(&pixel, sizeof(pixel));
    stdlib__null_memory(&left, sizeof(left));
    stdlib__null_memory(&right, sizeof(right));

    // A renderer builds its blur filter the first time a blur runs, which
    // takes tens of milliseconds (Direct3D 11 compiles two shaders: 55 ms
    // measured). One pixel under the control is blurred here, in a frame of
    // the closed control, so that the first frame of the opening select
    // does not pay for it. A blur of one pixel changes no pixel, and the
    // control's background is drawn over it next.
    if (state != NULL_POINTER && state->blur_prepared == FALSE && s->popup_blur > 0.0f && n->bounds.w >= 1.0f && n->bounds.h >= 1.0f)
    {
        pixel.x = n->bounds.x;
        pixel.y = n->bounds.y;
        pixel.w = 1.0f;
        pixel.h = 1.0f;
        renderer__blur_region(pixel, 1.0f);
        state->blur_prepared = TRUE;
    }

    // the whole background bundle when the style sets one; else a dark panel, so an unstyled select reads as a control
    if (s->has_background_color || s->has_bg_gradient || s->has_shadow || s->blur_px > 0.0f)
    {
        scene__emit_default_bg(n, scale);
    }
    else
    {
        background = scene__rgb(0.12f, 0.13f, 0.16f);
        renderer__submit_rect(n->bounds, background, s->radius * scale);
        scene__emit_border(n, n->bounds, scale);
    }

    // the label: left, 12 px in, centred on the line; the right side belongs to the chevron
    f = _widget_select_internal__font(n, scale);
    color = s->has_font_color ? s->font_color : scene__rgb(0.96f, 0.96f, 0.98f);
    _widget_select_internal__draw_text(n, f, scale, n->bounds.x + 12.0f * scale, n->bounds.y, n->bounds.h, color, n->text);

    // Down when closed, up when open; use the same scaled accent as before.
    chevron = s->has_accent_color ? s->accent_color : scene__rgb(0.72f, 0.74f, 0.80f);
    left.x = cx - side * 0.5f;
    left.y = cy - direction * side * 0.25f;
    right.x = cx + side * 0.5f;
    right.y = left.y;
    tip.x = cx;
    tip.y = cy + direction * side * 0.25f;
    renderer__submit_triangle(left, right, tip, chevron);
}

static void _widget_select_internal__emit_draws(gui_node_t* n, float scale)
{
    if (_widget_select_internal__is_inline(n->resolved.select_style) == TRUE)
    {
        _widget_select_internal__draw_inline(n, scale);
        return;
    }
    _widget_select_internal__draw_closed(n, scale);
}

// ===== the node ==============================================================

static void _widget_select_internal__init_defaults(gui_node_t* n)
{
    // the first option; the layout clamps the value once the options exist
    n->value = 0.0f;
}

static boole _widget_select_internal__apply_attribute(gui_node_t* n, char* name, char* value)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    char* p = NULL_POINTER;
    int64 index = 0;

    if (stdlib__strcmp(name, "value") == 0)
    {
        // a plain integer index; the layout clamps it to the option count
        for (p = value; *p != 0; p++)
        {
            if (*p >= '0' && *p <= '9')
            {
                index = index * 10 + (*p - '0');
            }
        }
        n->value = (float)index;
        return TRUE;
    }
    if (state == NULL_POINTER)
    {
        return FALSE;
    }
    // title: sheet, page, dialog. confirm, cancel: the buttons of a dialog. placeholder: the empty field of a search
    if (stdlib__strcmp(name, "title") == 0)
    {
        _widget_select_internal__copy_text(state->title, value);
        return TRUE;
    }
    if (stdlib__strcmp(name, "confirm") == 0)
    {
        _widget_select_internal__copy_text(state->confirm, value);
        return TRUE;
    }
    if (stdlib__strcmp(name, "cancel") == 0)
    {
        _widget_select_internal__copy_text(state->cancel, value);
        return TRUE;
    }
    if (stdlib__strcmp(name, "placeholder") == 0)
    {
        _widget_select_internal__copy_text(state->placeholder, value);
        return TRUE;
    }
    return FALSE;
}

static void _widget_select_internal__layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    gui_style_t* s = &n->resolved;
    gui_node_t* c = NULL_POINTER;
    int64 count = 0;
    float was = n->bounds.w;

    (void)avail_h;
    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = scene__layout_width(n, avail_w, scale);
    n->bounds.h = (s->size_h > 0.0f) ? s->size_h * scale : 36.0f * scale;

    // the options carry data and are not drawn as nodes: zero size at the control's place
    for (c = n->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        _widget_select_internal__walked++;
        c->bounds.x = x;
        c->bounds.y = y;
        c->bounds.w = 0.0f;
        c->bounds.h = 0.0f;
        if (c->type == GUI_NODE_TYPE__OPTION)
        {
            c->value = (count == (int64)n->value) ? 1.0f : 0.0f;
            if (n->is_open == FALSE && n->widget_anim <= 0.0f && _widget_select_internal__is_inline(s->select_style) == FALSE && c->state != GUI_NODE_STATE__DISABLED)
            {
                c->state = GUI_NODE_STATE__DEFAULT;
            }
            count++;
        }
    }
    if (count > 0)
    {
        if (n->value < 0.0f)
        {
            n->value = 0.0f;
        }
        if ((int64)n->value >= count)
        {
            n->value = (float)(count - 1);
        }
    }
    _widget_select_internal__sync_label(n);
    // a carousel and the mark of a segmented control stand on the value; their item width follows the control's width
    if (state != NULL_POINTER && (s->select_style == GUI_SELECT_STYLE__CAROUSEL || s->select_style == GUI_SELECT_STYLE__SEGMENTED) && state->drag_held == FALSE && was != n->bounds.w)
    {
        _widget_select_internal__show_current(n);
    }
}

// ===== input =================================================================

static void _widget_select_internal__on_mouse_down(gui_node_t* n, int64 x, int64 y, int64 button)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    _widget_select_internal__metrics_t m;
    gui_rect_t thumb;
    float pointer = 0.0f;

    stdlib__null_memory(&thumb, sizeof(thumb));
    if (state == NULL_POINTER)
    {
        return;
    }
    state->press_on_bar = FALSE;
    state->thumb_held = FALSE;
    state->drag_held = FALSE;
    state->drag_moved = FALSE;
    state->sheet_held = FALSE;
    if (button != 0)
    {
        return;
    }
    _widget_select_internal__metrics(n, &m);
    pointer = (_widget_select_internal__is_horizontal(m.style) == TRUE) ? (float)x : (float)y;
    if (_widget_select_internal__is_inline(m.style) == TRUE)
    {
        // chips and a carousel are moved by a drag
        if (m.style != GUI_SELECT_STYLE__SEGMENTED && _widget_select_internal__contains(n->bounds, (float)x, (float)y) == TRUE)
        {
            state->drag_held = TRUE;
            state->drag_pointer = pointer;
            state->drag_scroll = state->scroll_target;
            state->drag_last = pointer;
            state->drag_last_ms = scene__frame_time_ms();
            state->drag_speed = 0.0f;
        }
        return;
    }
    if (n->is_open == FALSE || scene__overlay_node() != n)
    {
        return;
    }
    if (m.bar.w > 0.0f && _widget_select_internal__contains(m.bar, (float)x, (float)y) == TRUE)
    {
        // on the thumb the press takes it; beside it the rows move one page toward the pointer
        state->press_on_bar = TRUE;
        thumb = scroll__vbar_thumb_in(n, m.rows, m.content, state->scroll_target, m.scale);
        if (_widget_select_internal__contains(thumb, (float)x, (float)y) == TRUE)
        {
            state->thumb_held = TRUE;
            state->thumb_pointer = (float)y;
            state->thumb_scroll = state->scroll_target;
            state->scroll = state->scroll_target;
            state->scroll_velocity = 0.0f;
            return;
        }
        _widget_select_internal__scroll_by(n, &m, ((float)y < thumb.y) ? -m.rows.h : m.rows.h);
        return;
    }
    if (m.style == GUI_SELECT_STYLE__SHEET && _widget_select_internal__contains(m.header, (float)x, (float)y) == TRUE)
    {
        state->sheet_held = TRUE;
        state->sheet_pointer = (float)y - state->sheet_pull;
        return;
    }
    if (_widget_select_internal__contains(m.rows, (float)x, (float)y) == TRUE)
    {
        state->drag_held = TRUE;
        state->drag_pointer = pointer;
        state->drag_scroll = state->scroll_target;
        state->drag_last = pointer;
        state->drag_last_ms = scene__frame_time_ms();
        state->drag_speed = 0.0f;
    }
}

/**
 * @return boole -> TRUE while the drag belongs to the select: the open select is modal, and a drag of chips or a carousel moves them, so the page underneath is not panned by it
 */
static boole _widget_select_internal__on_mouse_drag(gui_node_t* n, int64 x, int64 y)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    _widget_select_internal__metrics_t m;
    float pointer = 0.0f;
    int64 now = scene__frame_time_ms();
    float wanted = 0.0f;
    float speed = 0.0f;
    boole open = (boole)(n->is_open == TRUE && scene__overlay_node() == n);

    if (state == NULL_POINTER)
    {
        return open;
    }
    _widget_select_internal__metrics(n, &m);
    pointer = (_widget_select_internal__is_horizontal(m.style) == TRUE) ? (float)x : (float)y;
    if (state->thumb_held == TRUE)
    {
        state->scroll_target = _widget_select_internal__clamp(state->thumb_scroll + ((float)y - state->thumb_pointer) * scroll__scroll_per_pixel_in(n, m.rows, m.content, m.scale), 0.0f, m.max_scroll);
        state->scroll = state->scroll_target;
        state->scroll_velocity = 0.0f;
        n->widget_animating = TRUE;
        return TRUE;
    }
    if (state->sheet_held == TRUE)
    {
        state->sheet_pull = stdlib__fmaxf(0.0f, (float)y - state->sheet_pointer);
        n->widget_animating = TRUE;
        return TRUE;
    }
    if (state->drag_held == FALSE)
    {
        return open;
    }
    if (state->drag_moved == FALSE && stdlib__fabsf(pointer - state->drag_pointer) > _WIDGET_SELECT_INTERNAL__DRAG_START * m.scale)
    {
        state->drag_moved = TRUE;
    }
    if (state->drag_moved == FALSE)
    {
        return open;
    }
    // the content follows the pointer; past an edge by half the distance
    wanted = state->drag_scroll - (pointer - state->drag_pointer);
    state->scroll_target = _widget_select_internal__clamp(wanted, 0.0f, m.max_scroll);
    state->scroll = state->scroll_target + (wanted - state->scroll_target) * 0.5f;
    state->scroll_velocity = 0.0f;
    if (now > state->drag_last_ms)
    {
        speed = (pointer - state->drag_last) / (float)(now - state->drag_last_ms);
        state->drag_speed = state->drag_speed * 0.6f + speed * 0.4f;
        state->drag_last = pointer;
        state->drag_last_ms = now;
    }
    n->widget_animating = TRUE;
    return TRUE;
}

static void _widget_select_internal__on_mouse_up(gui_node_t* n, int64 x, int64 y, int64 button)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    _widget_select_internal__metrics_t m;
    int64 row = -1;
    int64 option = -1;
    float landing = 0.0f;
    boole dragged = FALSE;
    boole pulled = FALSE;

    if (button != 0)
    {
        return;
    }
    _widget_select_internal__metrics(n, &m);
    if (state != NULL_POINTER)
    {
        if (state->press_on_bar == TRUE)
        {
            state->press_on_bar = FALSE;
            state->thumb_held = FALSE;
            return;
        }
        dragged = state->drag_moved;
        pulled = state->sheet_held;
        state->drag_held = FALSE;
        state->drag_moved = FALSE;
        state->sheet_held = FALSE;
        if (pulled == TRUE)
        {
            // far enough down closes the sheet; else it goes back to its place (scroll_step)
            if (state->sheet_pull > m.rest.h * _WIDGET_SELECT_INTERNAL__SHEET_CLOSE)
            {
                _widget_select_internal__close_pulled_sheet(n, &m);
            }
            n->widget_animating = TRUE;
            return;
        }
        if (dragged == TRUE)
        {
            // the content carries on for its speed; a wheel and a carousel stop on an option
            landing = state->scroll - state->drag_speed * _WIDGET_SELECT_INTERNAL__FLING_MS;
            if (scroll__wobble_enabled(&n->resolved) == FALSE && _widget_select_internal__smooth(n) <= 0.0f)
            {
                // the style moves nothing over time: the content stays where it was let go
                landing = state->scroll;
            }
            n->widget_animating = TRUE;
            if (_widget_select_internal__is_turning(m.style) == TRUE && m.row_height > 0.0f)
            {
                _widget_select_internal__turn_to(n, &m, (int64)_widget_select_internal__round(landing / m.row_height));
                return;
            }
            state->scroll_target = _widget_select_internal__clamp(landing, 0.0f, m.max_scroll);
            return;
        }
    }

    // a click
    if (_widget_select_internal__is_inline(m.style) == TRUE)
    {
        option = _widget_select_internal__inline_at(n, &m, (float)x, (float)y);
        if (option < 0)
        {
            return;
        }
        if (m.style == GUI_SELECT_STYLE__CHIPS)
        {
            _widget_select_internal__commit(n, option, FALSE);
            return;
        }
        _widget_select_internal__turn_to(n, &m, option);
        return;
    }
    if (n->is_open == TRUE)
    {
        if (m.style == GUI_SELECT_STYLE__PAGE && _widget_select_internal__contains(m.back, (float)x, (float)y) == TRUE)
        {
            scene__select_open(n, FALSE);
            return;
        }
        if (m.style == GUI_SELECT_STYLE__DIALOG && _widget_select_internal__contains(m.cancel, (float)x, (float)y) == TRUE)
        {
            scene__select_open(n, FALSE);
            return;
        }
        if (m.style == GUI_SELECT_STYLE__DIALOG && _widget_select_internal__contains(m.confirm, (float)x, (float)y) == TRUE)
        {
            _widget_select_internal__commit(n, (state != NULL_POINTER && state->pending >= 0) ? state->pending : (int64)n->value, TRUE);
            return;
        }
        // a centred or clamped panel can cover its own control: a displayed row takes the click first
        row = _widget_select_internal__row_at(&m, (float)x, (float)y);
    }
    if (row >= 0)
    {
        option = _widget_select_internal__option_of_row(&m, state, row);
        if (option < 0)
        {
            return;
        }
        if (m.style == GUI_SELECT_STYLE__DIALOG)
        {
            if (state != NULL_POINTER)
            {
                state->pending = option;
            }
            n->widget_animating = TRUE;
            return;
        }
        // A clicked wheel row commits immediately, including rows away from the middle.
        // Closing freezes the current geometry for the popup's fade.
        _widget_select_internal__commit(n, option, TRUE);
        return;
    }
    if (n->is_open == TRUE && _widget_select_internal__contains(m.panel, (float)x, (float)y) == TRUE)
    {
        // on the panel, beside the rows: the title, the text field, the free part of a button strip
        return;
    }
    if (_widget_select_internal__contains(n->bounds, (float)x, (float)y) == TRUE)
    {
        scene__select_open(n, (boole)(n->is_open == FALSE));
    }
    else
    {
        scene__select_open(n, FALSE);
    }
}

static boole _widget_select_internal__on_mouse_wheel(gui_node_t* n, int64 x, int64 y, float delta)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    _widget_select_internal__metrics_t m;
    int64 steps = 0;

    (void)x;
    (void)y;
    _widget_select_internal__metrics(n, &m);
    if (_widget_select_internal__is_inline(m.style) == TRUE)
    {
        if (m.style == GUI_SELECT_STYLE__SEGMENTED || m.count <= 0)
        {
            return FALSE;
        }
        if (m.style == GUI_SELECT_STYLE__CHIPS)
        {
            // nothing to scroll: the wheel goes to the page
            if (m.max_scroll <= 0.0f)
            {
                return FALSE;
            }
            _widget_select_internal__scroll_by(n, &m, -delta * _WIDGET_SELECT_INTERNAL__CHIPS_TICK * m.scale);
            return TRUE;
        }
    }
    else if (scene__overlay_node() != n)
    {
        return FALSE;
    }
    if (_widget_select_internal__is_turning(m.style) == TRUE)
    {
        // one option per tick, counted from the option the turn is heading for
        steps = (int64)_widget_select_internal__round(stdlib__fabsf(delta));
        if (steps < 1)
        {
            steps = 1;
        }
        if (state != NULL_POINTER && m.row_height > 0.0f)
        {
            _widget_select_internal__turn_to(n, &m, (int64)_widget_select_internal__round(state->scroll_target / m.row_height) + ((delta > 0.0f) ? -steps : steps));
        }
        return TRUE;
    }
    _widget_select_internal__scroll_by(n, &m, -delta * _WIDGET_SELECT_INTERNAL__WHEEL_TICK_ROWS * m.row_height);
    // an open select keeps the wheel: the page underneath does not scroll
    return TRUE;
}

/**
 * @brief search: a typed character goes into the text field; the rows become the options that contain the text
 */
static void _widget_select_internal__on_char(gui_node_t* n, uint codepoint)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);

    if (state == NULL_POINTER || n->resolved.select_style != GUI_SELECT_STYLE__SEARCH || n->is_open == FALSE)
    {
        return;
    }
    // printable ASCII and Latin-1, the range of the font atlas
    if (codepoint < 32 || codepoint > 255 || codepoint == 127)
    {
        return;
    }
    if (state->query_len + 1 >= (int64)sizeof(state->query))
    {
        return;
    }
    state->query[state->query_len] = (char)codepoint;
    state->query_len++;
    state->query[state->query_len] = 0;
    state->scroll = 0.0f;
    state->scroll_target = 0.0f;
    state->scroll_velocity = 0.0f;
    state->stagger_first = 0;
    state->highlight = 0;
    n->widget_animating = TRUE;
}

static void _widget_select_internal__on_key(gui_node_t* n, int64 vk, boole down)
{
    _widget_select_internal__state_t* state = _widget_select_internal__state(n);
    _widget_select_internal__metrics_t m;
    int64 option = -1;
    int64 step = 0;

    if (down == FALSE || state == NULL_POINTER)
    {
        return;
    }
    _widget_select_internal__metrics(n, &m);
    // the virtual-key codes of Win32, compared by value: the platform layer passes them through
    if (_widget_select_internal__is_inline(m.style) == TRUE)
    {
        if (vk == 0x25 || vk == 0x27) // VK_LEFT, VK_RIGHT
        {
            option = (int64)n->value + ((vk == 0x25) ? -1 : 1);
            if (option < 0 || option > m.count - 1)
            {
                return;
            }
            if (m.style == GUI_SELECT_STYLE__CHIPS)
            {
                _widget_select_internal__commit(n, option, FALSE);
                return;
            }
            _widget_select_internal__turn_to(n, &m, option);
        }
        return;
    }
    if (n->is_open == FALSE)
    {
        if (vk == 0x0D || vk == 0x20 || vk == 0x28) // VK_RETURN, VK_SPACE, VK_DOWN
        {
            scene__select_open(n, TRUE);
        }
        return;
    }
    if (vk == 0x1B) // VK_ESCAPE
    {
        scene__select_open(n, FALSE);
        return;
    }
    if (vk == 0x08 && m.style == GUI_SELECT_STYLE__SEARCH) // VK_BACK
    {
        if (state->query_len > 0)
        {
            state->query_len--;
            state->query[state->query_len] = 0;
            state->scroll = 0.0f;
            state->scroll_target = 0.0f;
            state->scroll_velocity = 0.0f;
            state->stagger_first = 0;
            state->highlight = (state->query_len > 0) ? 0 : -1;
            n->widget_animating = TRUE;
        }
        return;
    }
    if (vk == 0x26 || vk == 0x28) // VK_UP, VK_DOWN
    {
        step = (vk == 0x26) ? -1 : 1;
        if (m.style == GUI_SELECT_STYLE__WHEEL)
        {
            if (m.row_height > 0.0f)
            {
                _widget_select_internal__turn_to(n, &m, (int64)_widget_select_internal__round(state->scroll_target / m.row_height) + step);
            }
            return;
        }
        if (m.rows_count <= 0)
        {
            return;
        }
        // the first press starts at the current option's row
        if (state->highlight < 0)
        {
            state->highlight = _widget_select_internal__row_of_option(&m, state, (m.style == GUI_SELECT_STYLE__DIALOG) ? state->pending : (int64)n->value);
            if (state->highlight < 0)
            {
                state->highlight = (step > 0) ? -1 : m.rows_count;
            }
        }
        state->highlight = (int64)_widget_select_internal__clamp((float)(state->highlight + step), 0.0f, (float)(m.rows_count - 1));
        _widget_select_internal__scroll_to_row(n, &m, state->highlight);
        if (m.style == GUI_SELECT_STYLE__DIALOG)
        {
            state->pending = _widget_select_internal__option_of_row(&m, state, state->highlight);
        }
        n->widget_animating = TRUE;
        return;
    }
    if (vk == 0x0D) // VK_RETURN
    {
        if (m.style == GUI_SELECT_STYLE__WHEEL)
        {
            scene__select_open(n, FALSE);
            return;
        }
        if (m.style == GUI_SELECT_STYLE__DIALOG)
        {
            _widget_select_internal__commit(n, (state->pending >= 0) ? state->pending : (int64)n->value, TRUE);
            return;
        }
        // the row the keys are on; a search without one takes its first match
        option = _widget_select_internal__option_of_row(&m, state, (state->highlight >= 0) ? state->highlight : ((m.style == GUI_SELECT_STYLE__SEARCH && m.filtered == TRUE) ? 0 : -1));
        if (option >= 0)
        {
            _widget_select_internal__commit(n, option, TRUE);
        }
    }
}

/**
 * @brief a select that is freed while it is open gives the overlay slot back (the next frame would call into freed memory otherwise), and its list of matches
 */
static void _widget_select_internal__on_destroy(gui_node_t* n)
{
    _widget_select_internal__state_t* state = (_widget_select_internal__state_t*)n->user_data;
    gui_rect_t none;

    stdlib__null_memory(&none, sizeof(none));
    if (scene__overlay_node() == n)
    {
        scene__set_overlay(NULL_POINTER, none, NULL_POINTER);
    }
    if (state != NULL_POINTER && state->matches != NULL_POINTER)
    {
        GUI_FREE(state->matches);
        state->matches = NULL_POINTER;
        state->match_capacity = 0;
        state->match_count = 0;
    }
}

static const widget_vtable_t _widget_select_internal__vtable = {
    .type_name       = "select",
    .init_defaults   = _widget_select_internal__init_defaults,
    .apply_attribute = _widget_select_internal__apply_attribute,
    .layout          = _widget_select_internal__layout,
    .emit_draws      = _widget_select_internal__emit_draws,
    .on_mouse_down   = _widget_select_internal__on_mouse_down,
    .on_mouse_up     = _widget_select_internal__on_mouse_up,
    .on_mouse_drag   = _widget_select_internal__on_mouse_drag,
    .on_mouse_wheel  = _widget_select_internal__on_mouse_wheel,
    .on_char         = _widget_select_internal__on_char,
    .on_key          = _widget_select_internal__on_key,
    .state_size      = sizeof(_widget_select_internal__state_t),
    .on_destroy      = _widget_select_internal__on_destroy,
    .consumes_click  = TRUE,
    .takes_focus     = TRUE,
};

void widget_select__register(void)
{
    widget_registry__register(GUI_NODE_TYPE__SELECT, &_widget_select_internal__vtable);
}
