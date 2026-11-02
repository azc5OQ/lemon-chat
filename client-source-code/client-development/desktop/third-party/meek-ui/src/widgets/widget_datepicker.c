// widget_datepicker.c - <datepicker value="2026-09-13" week-start="monday"/>
// An inline month calendar: a header with the month name, the year and
// an arrow on each side, a weekday row, and a 6 x 7 grid of days.
// Clicking a day selects it (a day shown from a neighbouring month
// switches the shown month first), the arrows move by one month. The
// selected date is kept in node->text as "YYYY-MM-DD" and fires
// on_change with ev->date filled in. Hosts drive it from C through
// widget_datepicker.h.
// layout: the style's size / width / height when set, otherwise seven
// square cells of (line box + 2 * 8 px) plus the two header rows and
// the padding and border. min / max sizes apply.
// draw: bg + border through scene__emit_default_bg; text in
// font-color; the selected day sits on an accent-color pill and takes
// the background color (or white); days of the neighbouring months at
// 35 % alpha; the arrows in accent-color.
// The calendar is proleptic Gregorian and knows no "today": the shown
// month defaults to the selected date, or to January 2026 when nothing
// is set, and a host that wants the current month passes it.

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "widget_datepicker.h"
#include "scene.h"
#include "renderer.h"
#include "font.h"
#include "third_party/log.h"
#include "clib/stdlib.h"

#define _WIDGET_DATEPICKER_INTERNAL__ROWS        6
#define _WIDGET_DATEPICKER_INTERNAL__COLS        7
#define _WIDGET_DATEPICKER_INTERNAL__CELL_PAD    8.0f
#define _WIDGET_DATEPICKER_INTERNAL__HEADER_PAD  8.0f
#define _WIDGET_DATEPICKER_INTERNAL__WEEKDAY_PAD 4.0f

typedef struct _widget_datepicker_internal__state_t
{
    int64 year;               // 0x0   shown month
    int64 month;              // 0x8   1..12
    int64 sel_year;           // 0x10  selected date; 0 = nothing selected
    int64 sel_month;          // 0x18
    int64 sel_day;            // 0x20
    boole week_starts_monday; // 0x28
} _widget_datepicker_internal__state_t;

// the geometry of one laid-out picker, in physical pixels
typedef struct _widget_datepicker_internal__geometry_t
{
    float header_y;  // 0x0
    float header_h;  // 0x4
    float weekday_y; // 0x8
    float weekday_h; // 0xc
    float grid_x;    // 0x10
    float grid_y;    // 0x14
    float cell_w;    // 0x18
    float cell_h;    // 0x1c
} _widget_datepicker_internal__geometry_t;

static const char* _widget_datepicker_internal__month_names[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
static const char* _widget_datepicker_internal__day_names_monday[7] = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };
static const char* _widget_datepicker_internal__day_names_sunday[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };

static _widget_datepicker_internal__state_t* _widget_datepicker_internal__state(gui_node_t* n);
static boole _widget_datepicker_internal__is_leap(int64 y);
static int64 _widget_datepicker_internal__days_in_month(int64 y, int64 m);
static int64 _widget_datepicker_internal__weekday(int64 y, int64 m, int64 d);
static void  _widget_datepicker_internal__step_day(int64* y, int64* m, int64* d, int64 delta);
static void  _widget_datepicker_internal__cell_date(_widget_datepicker_internal__state_t* st, int64 cell, int64* out_y, int64* out_m, int64* out_d);
static int64 _widget_datepicker_internal__fmt_int(char* out, int64 v, int64 min_digits);
static boole _widget_datepicker_internal__parse_date(const char* s, int64* y, int64* m, int64* d);
static void  _widget_datepicker_internal__write_text(gui_node_t* n, _widget_datepicker_internal__state_t* st);
static gui_font_t* _widget_datepicker_internal__font(gui_node_t* n, float scale);
static void  _widget_datepicker_internal__geometry(gui_node_t* n, float scale, _widget_datepicker_internal__geometry_t* g);
static void  _widget_datepicker_internal__draw_centered(gui_font_t* f, float cx, float baseline, gui_color_t color, char* text);
static void  _widget_datepicker_internal__select(gui_node_t* n, _widget_datepicker_internal__state_t* st, int64 y, int64 m, int64 d, boole notify);
static void  _widget_datepicker_internal__shift_month(_widget_datepicker_internal__state_t* st, int64 delta);
static void  datepicker_init_defaults(gui_node_t* n);
static boole datepicker_apply_attribute(gui_node_t* n, char* name, char* value);
static void  datepicker_layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale);
static void  datepicker_emit_draws(gui_node_t* n, float scale);
static void  datepicker_on_mouse_up(gui_node_t* n, int64 x, int64 y, int64 button);

static _widget_datepicker_internal__state_t* _widget_datepicker_internal__state(gui_node_t* n)
{
    if (n == NULL_POINTER || n->type != GUI_NODE_TYPE__DATEPICKER)
    {
        return NULL_POINTER;
    }

    return (_widget_datepicker_internal__state_t*)scene__widget_state(n);
}

static boole _widget_datepicker_internal__is_leap(int64 y)
{
    return (boole)((stdlib__rem64(y, 4) == 0 && stdlib__rem64(y, 100) != 0) || stdlib__rem64(y, 400) == 0);
}

static int64 _widget_datepicker_internal__days_in_month(int64 y, int64 m)
{
    static const int64 days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if (m < 1 || m > 12)
    {
        return 30;
    }

    if (m == 2 && _widget_datepicker_internal__is_leap(y))
    {
        return 29;
    }

    return days[m - 1];
}

// 0 = Sunday .. 6 = Saturday (Sakamoto's method, proleptic Gregorian)
static int64 _widget_datepicker_internal__weekday(int64 y, int64 m, int64 d)
{
    static const int64 t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int64 yy = y;
    int64 w  = 0;

    if (m < 3)
    {
        yy = yy - 1;
    }

    w = stdlib__rem64(yy + stdlib__div64(yy, 4) - stdlib__div64(yy, 100)
        + stdlib__div64(yy, 400) + t[stdlib__rem64(m - 1, 12)] + d, 7);

    if (w < 0)
    {
        w = w + 7;
    }

    return w;
}

// moves a date by delta days, delta of any sign; small |delta| in practice (at most 42)
static void _widget_datepicker_internal__step_day(int64* y, int64* m, int64* d, int64 delta)
{
    while (delta > 0)
    {
        *d = *d + 1;
        if (*d > _widget_datepicker_internal__days_in_month(*y, *m))
        {
            *d = 1;
            *m = *m + 1;
            if (*m > 12)
            {
                *m = 1;
                *y = *y + 1;
            }
        }
        delta--;
    }

    while (delta < 0)
    {
        *d = *d - 1;
        if (*d < 1)
        {
            *m = *m - 1;
            if (*m < 1)
            {
                *m = 12;
                *y = *y - 1;
            }
            *d = _widget_datepicker_internal__days_in_month(*y, *m);
        }
        delta++;
    }
}

// the date shown in grid cell `cell` (0 .. 41): the first cell is the
// week-start day on or before the first of the shown month
static void _widget_datepicker_internal__cell_date(_widget_datepicker_internal__state_t* st, int64 cell, int64* out_y, int64* out_m, int64* out_d)
{
    int64 y      = st->year;
    int64 m      = st->month;
    int64 d      = 1;
    int64 first  = 0;
    int64 start  = 0;
    int64 offset = 0;

    first  = _widget_datepicker_internal__weekday(y, m, 1);
    start  = st->week_starts_monday ? 1 : 0;
    offset = stdlib__rem64(first - start + 7, 7);

    _widget_datepicker_internal__step_day(&y, &m, &d, cell - offset);
    *out_y = y;
    *out_m = m;
    *out_d = d;
}

// decimal into out, zero-padded to min_digits, no terminator written; returns the length
static int64 _widget_datepicker_internal__fmt_int(char* out, int64 v, int64 min_digits)
{
    char  tmp[24];
    int64 len = 0;
    int64 i   = 0;
    boole neg = FALSE;

    stdlib__null_memory(tmp, sizeof(tmp));
    neg = (boole)(v < 0);

    if (neg)
    {
        v = -v;
    }

    do
    {
        int64 digit;
        v = stdlib__divmod64(v, 10, &digit);
        tmp[len++] = (char)('0' + digit);
    } while (v > 0 && len < 20);

    while (len < min_digits)
    {
        tmp[len++] = '0';
    }

    if (neg)
    {
        out[i++] = '-';
    }

    while (len > 0)
    {
        out[i++] = tmp[--len];
    }

    return i;
}

// "YYYY-MM-DD" (the day and month may have one digit)
static boole _widget_datepicker_internal__parse_date(const char* s, int64* y, int64* m, int64* d)
{
    int64 parts[3];
    int64 part   = 0;
    int64 digits = 0;
    int64 i      = 0;

    stdlib__null_memory(parts, sizeof(parts));

    if (s == NULL_POINTER)
    {
        return FALSE;
    }

    for (i = 0; s[i] != 0; i++)
    {
        if (s[i] >= '0' && s[i] <= '9')
        {
            parts[part] = parts[part] * 10 + (s[i] - '0');
            digits++;
        }
        else if (s[i] == '-' && digits > 0 && part < 2)
        {
            part++;
            digits = 0;
        }
        else
        {
            return FALSE;
        }
    }

    if (part != 2 || digits == 0)
    {
        return FALSE;
    }

    if (parts[1] < 1 || parts[1] > 12)
    {
        return FALSE;
    }

    if (parts[2] < 1 || parts[2] > _widget_datepicker_internal__days_in_month(parts[0], parts[1]))
    {
        return FALSE;
    }

    *y = parts[0];
    *m = parts[1];
    *d = parts[2];

    return TRUE;
}

static void _widget_datepicker_internal__write_text(gui_node_t* n, _widget_datepicker_internal__state_t* st)
{
    int64 len = 0;

    if (st->sel_year == 0)
    {
        n->text[0]  = 0;
        n->text_len = 0;
        return;
    }

    len += _widget_datepicker_internal__fmt_int(n->text + len, st->sel_year, 4);
    n->text[len++] = '-';
    len += _widget_datepicker_internal__fmt_int(n->text + len, st->sel_month, 2);
    n->text[len++] = '-';
    len += _widget_datepicker_internal__fmt_int(n->text + len, st->sel_day, 2);
    n->text[len] = 0;
    n->text_len  = len;
    n->value     = (float)st->sel_day;
}

static gui_font_t* _widget_datepicker_internal__font(gui_node_t* n, float scale)
{
    float size_px = 0.0f;

    size_px = (n->resolved.font_size > 0.0f ? n->resolved.font_size : 14.0f) * scale;

    return font__at(n->resolved.font_family, size_px);
}

static void _widget_datepicker_internal__geometry(gui_node_t* n, float scale, _widget_datepicker_internal__geometry_t* g)
{
    gui_style_t* s      = NULL_POINTER;
    gui_font_t*  f      = NULL_POINTER;
    float        line_h = 0.0f;
    float        border = 0.0f;
    float        cx     = 0.0f;
    float        cy     = 0.0f;
    float        cw     = 0.0f;
    float        ch     = 0.0f;

    s      = &n->resolved;
    f      = _widget_datepicker_internal__font(n, scale);
    line_h = f != NULL_POINTER ? font__line_height_for(f, s, scale) : 16.0f * scale;
    border = scene__border_width(n, scale);
    cx     = n->bounds.x + border + s->pad_l * scale;
    cy     = n->bounds.y + border + s->pad_t * scale;
    cw     = n->bounds.w - border * 2.0f - (s->pad_l + s->pad_r) * scale;
    ch     = n->bounds.h - border * 2.0f - (s->pad_t + s->pad_b) * scale;

    g->header_y  = cy;
    g->header_h  = line_h + _WIDGET_DATEPICKER_INTERNAL__HEADER_PAD * 2.0f * scale;
    g->weekday_y = g->header_y + g->header_h;
    g->weekday_h = line_h + _WIDGET_DATEPICKER_INTERNAL__WEEKDAY_PAD * 2.0f * scale;
    g->grid_x    = cx;
    g->grid_y    = g->weekday_y + g->weekday_h;
    g->cell_w    = cw / (float)_WIDGET_DATEPICKER_INTERNAL__COLS;
    g->cell_h    = (ch - g->header_h - g->weekday_h) / (float)_WIDGET_DATEPICKER_INTERNAL__ROWS;

    if (g->cell_h < 0.0f)
    {
        g->cell_h = 0.0f;
    }
}

static void _widget_datepicker_internal__draw_centered(gui_font_t* f, float cx, float baseline, gui_color_t color, char* text)
{
    float w = 0.0f;

    w = font__measure(f, text);
    font__draw(f, cx - w * 0.5f, baseline, color, text);
}

static void _widget_datepicker_internal__select(gui_node_t* n, _widget_datepicker_internal__state_t* st, int64 y, int64 m, int64 d, boole notify)
{
    gui_event_t ev;

    stdlib__null_memory(&ev, sizeof(ev));

    st->sel_year  = y;
    st->sel_month = m;
    st->sel_day   = d;
    st->year      = y;
    st->month     = m;
    _widget_datepicker_internal__write_text(n, st);

    if (notify == FALSE)
    {
        return;
    }

    ev.type       = GUI_EVENT_TYPE__CHANGE;
    ev.date.year  = y;
    ev.date.month = m;
    ev.date.day   = d;
    scene__dispatch_event(n, &ev);
}

// shows the month `delta` months away (delta is -1 or +1 from the arrows)
static void _widget_datepicker_internal__shift_month(_widget_datepicker_internal__state_t* st, int64 delta)
{
    st->month = st->month + delta;

    if (st->month < 1)
    {
        st->month = 12;
        st->year  = st->year - 1;
    }

    if (st->month > 12)
    {
        st->month = 1;
        st->year  = st->year + 1;
    }
}

static void datepicker_init_defaults(gui_node_t* n)
{
    _widget_datepicker_internal__state_t* st = NULL_POINTER;

    st = _widget_datepicker_internal__state(n);

    if (st == NULL_POINTER)
    {
        return;
    }

    st->year               = 2026;
    st->month              = 1;
    st->week_starts_monday = TRUE;
}

static boole datepicker_apply_attribute(gui_node_t* n, char* name, char* value)
{
    _widget_datepicker_internal__state_t* st = NULL_POINTER;
    char   buf[16];
    size_t len = 0;
    int64  y   = 0;
    int64  m   = 0;
    int64  d   = 0;

    stdlib__null_memory(buf, sizeof(buf));
    st = _widget_datepicker_internal__state(n);

    if (st == NULL_POINTER)
    {
        return FALSE;
    }

    if (stdlib__strcmp(name, "value") == 0)
    {
        if (_widget_datepicker_internal__parse_date(value, &y, &m, &d) == FALSE)
        {
            log_warn("%s %s %s", "datepicker: value '", value, "' is not YYYY-MM-DD");
            return TRUE;
        }

        _widget_datepicker_internal__select(n, st, y, m, d, FALSE);
        return TRUE;
    }

    if (stdlib__strcmp(name, "week-start") == 0)
    {
        if (stdlib__strcmp(value, "sunday") == 0)
        {
            st->week_starts_monday = FALSE;
        }
        else if (stdlib__strcmp(value, "monday") == 0)
        {
            st->week_starts_monday = TRUE;
        }
        else
        {
            log_warn("%s %s %s", "datepicker: week-start '", value, "' (monday|sunday)");
        }
        return TRUE;
    }

    if (stdlib__strcmp(name, "month") == 0)
    {
        // "YYYY-MM": the month to show when nothing is selected
        len = stdlib__strlen(value);

        if (len > 7)
        {
            len = 7;
        }

        stdlib__memcpy(buf, value, len);
        stdlib__memcpy(buf + len, "-01", 4);

        if (_widget_datepicker_internal__parse_date(buf, &y, &m, &d) == FALSE)
        {
            log_warn("%s %s %s", "datepicker: month '", value, "' is not YYYY-MM");
            return TRUE;
        }

        st->year  = y;
        st->month = m;
        return TRUE;
    }

    return FALSE;
}

static void datepicker_layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale)
{
    gui_style_t* s      = NULL_POINTER;
    gui_font_t*  f      = NULL_POINTER;
    float        line_h = 0.0f;
    float        cell   = 0.0f;
    float        border = 0.0f;
    float        w      = 0.0f;
    float        h      = 0.0f;

    (void)avail_h;

    s      = &n->resolved;
    f      = _widget_datepicker_internal__font(n, scale);
    line_h = f != NULL_POINTER ? font__line_height_for(f, s, scale) : 16.0f * scale;
    cell   = line_h + _WIDGET_DATEPICKER_INTERNAL__CELL_PAD * 2.0f * scale;
    border = scene__border_width(n, scale);
    w      = cell * (float)_WIDGET_DATEPICKER_INTERNAL__COLS + (s->pad_l + s->pad_r) * scale + border * 2.0f;
    h      = (line_h + _WIDGET_DATEPICKER_INTERNAL__HEADER_PAD * 2.0f * scale) + (line_h + _WIDGET_DATEPICKER_INTERNAL__WEEKDAY_PAD * 2.0f * scale) + cell * (float)_WIDGET_DATEPICKER_INTERNAL__ROWS + (s->pad_t + s->pad_b) * scale + border * 2.0f;

    if (s->size_w > 0.0f)
    {
        w = s->size_w * scale;
    }
    else if (s->width_pct > 0.0f && avail_w > 0.0f)
    {
        w = avail_w * s->width_pct / 100.0f;
    }

    if (s->size_h > 0.0f)
    {
        h = s->size_h * scale;
    }

    scene__apply_min_max(n, &w, &h, scale);

    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;
}

static void datepicker_emit_draws(gui_node_t* n, float scale)
{
    _widget_datepicker_internal__state_t*   st = NULL_POINTER;
    _widget_datepicker_internal__geometry_t g;
    gui_style_t*                            s  = NULL_POINTER;
    gui_font_t*                             f  = NULL_POINTER;
    gui_color_t                             text_color;
    gui_color_t                             accent;
    gui_color_t                             on_accent;
    gui_color_t                             dim;
    gui_color_t                             color;
    gui_rect_t                              pill;
    const char**                            day_names = NULL_POINTER;
    const char*                             month_name = NULL_POINTER;
    char                                    buf[40];
    float                                   baseline = 0.0f;
    float                                   line_h   = 0.0f;
    float                                   ty       = 0.0f;
    float                                   cx       = 0.0f;
    float                                   top      = 0.0f;
    float                                   side     = 0.0f;
    size_t                                  name_len = 0;
    int64                                   len      = 0;
    int64                                   cell     = 0;
    int64                                   col      = 0;
    int64                                   row      = 0;
    int64                                   y        = 0;
    int64                                   m        = 0;
    int64                                   d        = 0;
    boole                                   in_month = FALSE;
    boole                                   selected = FALSE;

    stdlib__null_memory(&g, sizeof(g));
    stdlib__null_memory(&text_color, sizeof(text_color));
    stdlib__null_memory(&accent, sizeof(accent));
    stdlib__null_memory(&on_accent, sizeof(on_accent));
    stdlib__null_memory(&dim, sizeof(dim));
    stdlib__null_memory(&color, sizeof(color));
    stdlib__null_memory(&pill, sizeof(pill));
    stdlib__null_memory(buf, sizeof(buf));

    scene__emit_default_bg(n, scale);

    st = _widget_datepicker_internal__state(n);
    s  = &n->resolved;
    f  = _widget_datepicker_internal__font(n, scale);

    if (st == NULL_POINTER || f == NULL_POINTER)
    {
        return;
    }

    _widget_datepicker_internal__geometry(n, scale, &g);

    text_color    = s->has_font_color ? s->font_color : scene__rgb(1.0f, 1.0f, 1.0f);
    accent        = s->has_accent_color ? s->accent_color : scene__rgb(0.39f, 0.40f, 0.95f);
    on_accent     = s->has_background_color ? s->background_color : scene__rgb(1.0f, 1.0f, 1.0f);
    on_accent.a   = 1.0f;
    dim           = text_color;
    dim.a         = dim.a * 0.35f;
    text_color.a *= n->effective_opacity;
    accent.a     *= n->effective_opacity;
    on_accent.a  *= n->effective_opacity;
    dim.a        *= n->effective_opacity;

    line_h   = font__line_height_for(f, s, scale);
    baseline = font__baseline_for(f, s, scale);

    // header: "< September 2026 >"
    ty         = g.header_y + (g.header_h - line_h) * 0.5f + baseline;
    month_name = _widget_datepicker_internal__month_names[stdlib__rem64(st->month - 1, 12)];
    name_len   = stdlib__strlen(month_name);
    stdlib__memcpy(buf, month_name, name_len);
    len        = (int64)name_len;
    buf[len++] = ' ';
    len       += _widget_datepicker_internal__fmt_int(buf + len, st->year, 4);
    buf[len]   = 0;
    _widget_datepicker_internal__draw_centered(f, g.grid_x + g.cell_w * 3.5f, ty, text_color, buf);
    _widget_datepicker_internal__draw_centered(f, g.grid_x + g.cell_w * 0.5f, ty, accent, "<");
    _widget_datepicker_internal__draw_centered(f, g.grid_x + g.cell_w * 6.5f, ty, accent, ">");

    // weekday row
    day_names = st->week_starts_monday ? _widget_datepicker_internal__day_names_monday : _widget_datepicker_internal__day_names_sunday;
    ty        = g.weekday_y + (g.weekday_h - line_h) * 0.5f + baseline;

    for (col = 0; col < _WIDGET_DATEPICKER_INTERNAL__COLS; col++)
    {
        _widget_datepicker_internal__draw_centered(f, g.grid_x + g.cell_w * ((float)col + 0.5f), ty, dim, (char*)day_names[col]);
    }

    // the grid
    for (cell = 0; cell < _WIDGET_DATEPICKER_INTERNAL__ROWS * _WIDGET_DATEPICKER_INTERNAL__COLS; cell++)
    {
        row = stdlib__divmod64(cell, _WIDGET_DATEPICKER_INTERNAL__COLS, &col);
        cx  = g.grid_x + g.cell_w * ((float)col + 0.5f);
        top = g.grid_y + g.cell_h * (float)row;
        ty  = top + (g.cell_h - line_h) * 0.5f + baseline;

        _widget_datepicker_internal__cell_date(st, cell, &y, &m, &d);
        in_month = (boole)(y == st->year && m == st->month);
        selected = (boole)(st->sel_year != 0 && y == st->sel_year && m == st->sel_month && d == st->sel_day);

        if (selected)
        {
            // a round pill as large as the cell allows
            side   = g.cell_w < g.cell_h ? g.cell_w : g.cell_h;
            pill.w = side - 4.0f * scale;
            pill.h = side - 4.0f * scale;
            pill.x = cx - pill.w * 0.5f;
            pill.y = top + (g.cell_h - pill.h) * 0.5f;
            renderer__submit_rect(pill, accent, pill.w * 0.5f);
            color = on_accent;
        }
        else
        {
            color = in_month ? text_color : dim;
        }

        len      = _widget_datepicker_internal__fmt_int(buf, d, 1);
        buf[len] = 0;
        _widget_datepicker_internal__draw_centered(f, cx, ty, color, buf);
    }
}

static void datepicker_on_mouse_up(gui_node_t* n, int64 x, int64 y, int64 button)
{
    _widget_datepicker_internal__state_t*   st = NULL_POINTER;
    _widget_datepicker_internal__geometry_t g;
    float                                   fx   = 0.0f;
    float                                   fy   = 0.0f;
    int64                                   col  = 0;
    int64                                   row  = 0;
    int64                                   cell = 0;
    int64                                   sy   = 0;
    int64                                   sm   = 0;
    int64                                   sd   = 0;

    (void)button;

    stdlib__null_memory(&g, sizeof(g));
    st = _widget_datepicker_internal__state(n);
    fx = (float)x;
    fy = (float)y;

    if (st == NULL_POINTER)
    {
        return;
    }

    // the release must land on the widget, like a button's click
    if (fx < n->bounds.x || fx >= n->bounds.x + n->bounds.w || fy < n->bounds.y || fy >= n->bounds.y + n->bounds.h)
    {
        return;
    }

    _widget_datepicker_internal__geometry(n, scene__scale(), &g);

    if (fy >= g.header_y && fy < g.header_y + g.header_h)
    {
        if (fx < g.grid_x + g.cell_w)
        {
            _widget_datepicker_internal__shift_month(st, -1);
        }
        else if (fx >= g.grid_x + g.cell_w * 6.0f)
        {
            _widget_datepicker_internal__shift_month(st, 1);
        }
        return;
    }

    if (fy < g.grid_y || g.cell_w <= 0.0f || g.cell_h <= 0.0f)
    {
        return;
    }

    col = (int64)((fx - g.grid_x) / g.cell_w);
    row = (int64)((fy - g.grid_y) / g.cell_h);

    if (col < 0 || col >= _WIDGET_DATEPICKER_INTERNAL__COLS || row < 0 || row >= _WIDGET_DATEPICKER_INTERNAL__ROWS)
    {
        return;
    }

    cell = row * _WIDGET_DATEPICKER_INTERNAL__COLS + col;
    _widget_datepicker_internal__cell_date(st, cell, &sy, &sm, &sd);
    _widget_datepicker_internal__select(n, st, sy, sm, sd, TRUE);
}

void widget_datepicker__set(gui_node_t* n, int64 year, int64 month, int64 day)
{
    _widget_datepicker_internal__state_t* st = NULL_POINTER;
    int64 days = 0;

    st = _widget_datepicker_internal__state(n);

    if (st == NULL_POINTER)
    {
        return;
    }

    if (month < 1)
    {
        month = 1;
    }

    if (month > 12)
    {
        month = 12;
    }

    days = _widget_datepicker_internal__days_in_month(year, month);

    if (day < 1)
    {
        day = 1;
    }

    if (day > days)
    {
        day = days;
    }

    _widget_datepicker_internal__select(n, st, year, month, day, FALSE);
}

boole widget_datepicker__get(gui_node_t* n, int64* out_year, int64* out_month, int64* out_day)
{
    _widget_datepicker_internal__state_t* st = NULL_POINTER;

    st = _widget_datepicker_internal__state(n);

    if (st == NULL_POINTER || st->sel_year == 0)
    {
        return FALSE;
    }

    if (out_year != NULL_POINTER)
    {
        *out_year = st->sel_year;
    }

    if (out_month != NULL_POINTER)
    {
        *out_month = st->sel_month;
    }

    if (out_day != NULL_POINTER)
    {
        *out_day = st->sel_day;
    }

    return TRUE;
}

void widget_datepicker__show_month(gui_node_t* n, int64 year, int64 month)
{
    _widget_datepicker_internal__state_t* st = NULL_POINTER;

    st = _widget_datepicker_internal__state(n);

    if (st == NULL_POINTER || month < 1 || month > 12)
    {
        return;
    }

    st->year  = year;
    st->month = month;
}

static const widget_vtable_t g_datepicker_vtable = {
    .type_name       = "datepicker",
    .init_defaults   = datepicker_init_defaults,
    .apply_attribute = datepicker_apply_attribute,
    .layout          = datepicker_layout,
    .emit_draws      = datepicker_emit_draws,
    .on_mouse_up     = datepicker_on_mouse_up,
    .consumes_click  = TRUE,
    .state_size      = sizeof(_widget_datepicker_internal__state_t),
};

void widget_datepicker__register(void)
{
    widget_registry__register(GUI_NODE_TYPE__DATEPICKER, &g_datepicker_vtable);
}
