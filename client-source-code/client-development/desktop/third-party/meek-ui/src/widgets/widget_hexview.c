// widget_hexview.c - <hexview cols="16" rows="8" format="hex" address="0x1000"/>
// A memory viewer for debugging: one row per `cols` bytes, the address
// on the left, every byte as a value in the chosen format, and the
// printable characters on the right. The buffer comes from C
// (widget_hexview__set_buffer) and is shown live; without one the
// widget draws a note. A click selects a byte, the arrow keys move
// the selection, on_change fires with the offset in ev->change.scalar.
// attributes, named like the HTML ones where one exists:
// cols      bytes per row, 1..32 (textarea's cols); alias columns
// rows      rows the default height shows, default 8 (textarea's rows)
// address   the address printed for offset 0, decimal or 0x hex; alias base
// format    hex (default, 2 digits) | dec (3) | oct (3) | bin (8)
// group     an extra space after every N values, 0 = none
// ascii     0 hides the character column
// style: `gap` is the space between two values (default one character
// of the font), `text-transform: uppercase` makes the hex digits upper
// case (they are lower case, like the address, when unset), `line-height`
// the row height, `overflow-y: auto | scroll` makes it scroll through
// the shared scroll module, `font-family` should be monospace
// (Cousine-Regular ships with the examples).
// layout: the style's size / width / height when set; otherwise as wide
// as the columns need and `rows` rows tall. min / max sizes apply.
// draw: bg + border through scene__emit_default_bg; the address in
// accent-color, the values in font-color, the characters at 80 %, the
// selected byte on an accent-color highlight in both columns.

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "widget_hexview.h"
#include "scene.h"
#include "scroll.h"
#include "renderer.h"
#include "font.h"
#include "third_party/log.h"
#include "clib/stdlib.h"

#define _WIDGET_HEXVIEW_INTERNAL__DEFAULT_COLS 16
#define _WIDGET_HEXVIEW_INTERNAL__MAX_COLS     32
#define _WIDGET_HEXVIEW_INTERNAL__DEFAULT_ROWS 8
#define _WIDGET_HEXVIEW_INTERNAL__ADDR_DIGITS  8
#define _WIDGET_HEXVIEW_INTERNAL__ADDR_GAP     2 // characters between the address and the values
#define _WIDGET_HEXVIEW_INTERNAL__ASCII_GAP    2 // characters between the values and the characters

typedef enum _widget_hexview_internal__format_e
{
    _WIDGET_HEXVIEW_INTERNAL__FORMAT_HEX = 0,
    _WIDGET_HEXVIEW_INTERNAL__FORMAT_DEC = 1,
    _WIDGET_HEXVIEW_INTERNAL__FORMAT_OCT = 2,
    _WIDGET_HEXVIEW_INTERNAL__FORMAT_BIN = 3
} _widget_hexview_internal__format_e;

typedef struct _widget_hexview_internal__state_t
{
    const ubyte* data;     // 0x0
    int64        len;      // 0x8
    uint64       base;     // 0x10
    int64        cols;     // 0x18
    int64        rows;     // 0x20  rows the default height shows
    int64        group;    // 0x28  values per group, 0 = no grouping
    int64        selected; // 0x30  -1 = none
    int64        format;   // 0x38  _widget_hexview_internal__format_e
    boole        ascii;    // 0x40
    float        row_h;    // 0x44  from the last layout, physical px
    float        char_w;   // 0x48
    float        cell_w;   // 0x4c  digits plus the gap, physical px
    float        gap_w;    // 0x50  the gap alone
} _widget_hexview_internal__state_t;

// the geometry of the laid-out view, physical px
typedef struct _widget_hexview_internal__geometry_t
{
    gui_rect_t inner;    // 0x0   inside the border and padding
    float      values_x; // 0x10  left edge of the first value
    float      ascii_x;  // 0x14  left edge of the character column
    int64      digits;   // 0x18  characters per value
} _widget_hexview_internal__geometry_t;

static _widget_hexview_internal__state_t* _widget_hexview_internal__state(gui_node_t* n);
static gui_font_t* _widget_hexview_internal__font(gui_node_t* n, float scale);
static int64 _widget_hexview_internal__digits(int64 format);
static void  _widget_hexview_internal__format_value(char* out, ubyte v, int64 format, boole upper);
static void  _widget_hexview_internal__hex(char* out, uint64 v, int64 digits, boole upper);
static int64 _widget_hexview_internal__rows(_widget_hexview_internal__state_t* st);
static void  _widget_hexview_internal__geometry(gui_node_t* n, _widget_hexview_internal__state_t* st, float scale, _widget_hexview_internal__geometry_t* g);
static float _widget_hexview_internal__cell_x(_widget_hexview_internal__state_t* st, _widget_hexview_internal__geometry_t* g, int64 col);
static void  _widget_hexview_internal__reveal(gui_node_t* n, _widget_hexview_internal__state_t* st);
static int64 _widget_hexview_internal__offset_at(gui_node_t* n, _widget_hexview_internal__state_t* st, float scale, float fx, float fy);
static void  _widget_hexview_internal__select(gui_node_t* n, _widget_hexview_internal__state_t* st, int64 offset);
static boole _widget_hexview_internal__parse_number(const char* value, uint64* out);
static void  hexview_init_defaults(gui_node_t* n);
static boole hexview_apply_attribute(gui_node_t* n, char* name, char* value);
static void  hexview_layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale);
static void  hexview_emit_draws(gui_node_t* n, float scale);
static void  hexview_on_mouse_down(gui_node_t* n, int64 x, int64 y, int64 button);
static boole hexview_on_mouse_drag(gui_node_t* n, int64 x, int64 y);
static void  hexview_on_mouse_up(gui_node_t* n, int64 x, int64 y, int64 button);
static void  hexview_on_key(gui_node_t* n, int64 vk, boole down);

static _widget_hexview_internal__state_t* _widget_hexview_internal__state(gui_node_t* n)
{
    if (n == NULL_POINTER || n->type != GUI_NODE_TYPE__HEXVIEW)
    {
        return NULL_POINTER;
    }

    return (_widget_hexview_internal__state_t*)scene__widget_state(n);
}

static gui_font_t* _widget_hexview_internal__font(gui_node_t* n, float scale)
{
    float size_px = 0.0f;

    size_px = (n->resolved.font_size > 0.0f ? n->resolved.font_size : 13.0f) * scale;

    return font__at(n->resolved.font_family, size_px);
}

static int64 _widget_hexview_internal__digits(int64 format)
{
    if (format == _WIDGET_HEXVIEW_INTERNAL__FORMAT_BIN)
    {
        return 8;
    }

    if (format == _WIDGET_HEXVIEW_INTERNAL__FORMAT_DEC || format == _WIDGET_HEXVIEW_INTERNAL__FORMAT_OCT)
    {
        return 3;
    }

    return 2;
}

// one byte in the chosen format, exactly digits(format) characters, no terminator written
static void _widget_hexview_internal__format_value(char* out, ubyte v, int64 format, boole upper)
{
    int64 i      = 0;
    int64 digits = 0;
    int64 radix  = 16;
    uint  value  = v;

    digits = _widget_hexview_internal__digits(format);

    if (format == _WIDGET_HEXVIEW_INTERNAL__FORMAT_HEX)
    {
        _widget_hexview_internal__hex(out, v, 2, upper);
        return;
    }

    if (format == _WIDGET_HEXVIEW_INTERNAL__FORMAT_DEC)
    {
        radix = 10;
    }
    else if (format == _WIDGET_HEXVIEW_INTERNAL__FORMAT_OCT)
    {
        radix = 8;
    }
    else
    {
        radix = 2;
    }

    for (i = digits - 1; i >= 0; i--)
    {
        out[i] = (char)('0' + (value % (uint)radix));
        value = value / (uint)radix;
    }
}

// hex, exactly `digits` characters, no terminator written
static void _widget_hexview_internal__hex(char* out, uint64 v, int64 digits, boole upper)
{
    static const char lower_table[16] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };
    static const char upper_table[16] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F' };
    const char* table = NULL_POINTER;
    int64       i     = 0;

    table = upper ? upper_table : lower_table;

    for (i = digits - 1; i >= 0; i--)
    {
        out[i] = table[v & 15u];
        v = v >> 4;
    }
}

static int64 _widget_hexview_internal__rows(_widget_hexview_internal__state_t* st)
{
    if (st->data == NULL_POINTER || st->len <= 0 || st->cols <= 0)
    {
        return 0;
    }

    return stdlib__div64(st->len + st->cols - 1, st->cols);
}

static void _widget_hexview_internal__geometry(gui_node_t* n, _widget_hexview_internal__state_t* st, float scale, _widget_hexview_internal__geometry_t* g)
{
    gui_style_t* s      = NULL_POINTER;
    float        border = 0.0f;
    float        groups = 0.0f;

    s      = &n->resolved;
    border = scene__border_width(n, scale);

    g->inner.x  = n->bounds.x + border + s->pad_l * scale;
    g->inner.y  = n->bounds.y + border + s->pad_t * scale;
    g->inner.w  = n->bounds.w - border * 2.0f - (s->pad_l + s->pad_r) * scale;
    g->inner.h  = n->bounds.h - border * 2.0f - (s->pad_t + s->pad_b) * scale;
    g->digits   = _widget_hexview_internal__digits(st->format);
    g->values_x = g->inner.x + st->char_w * (float)(_WIDGET_HEXVIEW_INTERNAL__ADDR_DIGITS + _WIDGET_HEXVIEW_INTERNAL__ADDR_GAP);

    if (st->group > 0)
    {
        groups = (float)stdlib__div64(st->cols - 1, st->group);
    }

    g->ascii_x = g->values_x + st->cell_w * (float)st->cols + st->gap_w * groups + st->char_w * (float)_WIDGET_HEXVIEW_INTERNAL__ASCII_GAP;
}

// left edge of value `col`: cells plus one extra gap after every group
static float _widget_hexview_internal__cell_x(_widget_hexview_internal__state_t* st, _widget_hexview_internal__geometry_t* g, int64 col)
{
    float x = 0.0f;

    x = g->values_x + st->cell_w * (float)col;

    if (st->group > 0)
    {
        x += st->gap_w * (float)stdlib__div64(col, st->group);
    }

    return x;
}

// scrolls so the selected byte's row is inside the inner box
static void _widget_hexview_internal__reveal(gui_node_t* n, _widget_hexview_internal__state_t* st)
{
    _widget_hexview_internal__geometry_t g;
    float row_top = 0.0f;

    stdlib__null_memory(&g, sizeof(g));

    if (st->selected < 0 || st->row_h <= 0.0f || st->cols <= 0)
    {
        return;
    }

    _widget_hexview_internal__geometry(n, st, scene__scale(), &g);
    row_top = (float)stdlib__div64(st->selected, st->cols) * st->row_h;

    if (row_top < n->scroll_y)
    {
        n->scroll_y = row_top;
    }
    else if (row_top + st->row_h > n->scroll_y + g.inner.h)
    {
        n->scroll_y = row_top + st->row_h - g.inner.h;
    }

    n->scroll_y_target = n->scroll_y;
    scroll__clamp(n);
}

// the byte under a point, -1 when the point is outside every value and character
static int64 _widget_hexview_internal__offset_at(gui_node_t* n, _widget_hexview_internal__state_t* st, float scale, float fx, float fy)
{
    _widget_hexview_internal__geometry_t g;
    float value_w = 0.0f;
    float cx      = 0.0f;
    int64 col     = -1;
    int64 c       = 0;
    int64 row     = 0;
    int64 offset  = 0;

    stdlib__null_memory(&g, sizeof(g));

    if (st->row_h <= 0.0f || st->char_w <= 0.0f || st->cols <= 0)
    {
        return -1;
    }

    _widget_hexview_internal__geometry(n, st, scale, &g);
    value_w = st->char_w * (float)g.digits;

    if (fy < g.inner.y || fy >= g.inner.y + g.inner.h)
    {
        return -1;
    }

    for (c = 0; c < st->cols; c++)
    {
        cx = _widget_hexview_internal__cell_x(st, &g, c);

        if (fx >= cx && fx < cx + value_w + st->gap_w)
        {
            col = c;
            break;
        }
    }

    if (col < 0 && st->ascii && fx >= g.ascii_x && fx < g.ascii_x + st->char_w * (float)st->cols)
    {
        col = (int64)((fx - g.ascii_x) / st->char_w);
    }

    if (col < 0 || col >= st->cols)
    {
        return -1;
    }

    row    = (int64)((fy - g.inner.y + n->scroll_y) / st->row_h);
    offset = row * st->cols + col;

    if (row < 0 || offset < 0 || offset >= st->len)
    {
        return -1;
    }

    return offset;
}

static void _widget_hexview_internal__select(gui_node_t* n, _widget_hexview_internal__state_t* st, int64 offset)
{
    if (offset < -1 || offset >= st->len)
    {
        return;
    }

    st->selected = offset;
    _widget_hexview_internal__reveal(n, st);
    scene__dispatch_change(n, (float)offset);
}

// decimal, or hex with a 0x prefix
static boole _widget_hexview_internal__parse_number(const char* value, uint64* out)
{
    const char* p     = NULL_POINTER;
    uint64      v     = 0;
    int64       radix = 10;
    int64       d     = 0;
    char        c     = 0;

    p = value;

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        p     = p + 2;
        radix = 16;
    }

    while (*p != 0)
    {
        c = *p;
        d = -1;

        if (c >= '0' && c <= '9')
        {
            d = c - '0';
        }
        else if (radix == 16 && c >= 'a' && c <= 'f')
        {
            d = 10 + (c - 'a');
        }
        else if (radix == 16 && c >= 'A' && c <= 'F')
        {
            d = 10 + (c - 'A');
        }

        if (d < 0)
        {
            return FALSE;
        }

        v = v * (uint64)radix + (uint64)d;
        p++;
    }

    *out = v;

    return TRUE;
}

static void hexview_init_defaults(gui_node_t* n)
{
    _widget_hexview_internal__state_t* st = NULL_POINTER;

    st = _widget_hexview_internal__state(n);

    if (st == NULL_POINTER)
    {
        return;
    }

    st->cols     = _WIDGET_HEXVIEW_INTERNAL__DEFAULT_COLS;
    st->rows     = _WIDGET_HEXVIEW_INTERNAL__DEFAULT_ROWS;
    st->selected = -1;
    st->ascii    = TRUE;
    st->format   = _WIDGET_HEXVIEW_INTERNAL__FORMAT_HEX;
}

static boole hexview_apply_attribute(gui_node_t* n, char* name, char* value)
{
    _widget_hexview_internal__state_t* st = NULL_POINTER;
    int64  c       = 0;
    uint64 address = 0;

    st = _widget_hexview_internal__state(n);

    if (st == NULL_POINTER)
    {
        return FALSE;
    }

    if (stdlib__strcmp(name, "cols") == 0 || stdlib__strcmp(name, "columns") == 0)
    {
        widget_hexview__set_columns(n, (int64)stdlib__atoi(value));
        return TRUE;
    }

    if (stdlib__strcmp(name, "rows") == 0)
    {
        c = (int64)stdlib__atoi(value);
        st->rows = c < 1 ? 1 : c;
        return TRUE;
    }

    if (stdlib__strcmp(name, "group") == 0)
    {
        c = (int64)stdlib__atoi(value);
        st->group = c < 0 ? 0 : c;
        return TRUE;
    }

    if (stdlib__strcmp(name, "format") == 0)
    {
        if (widget_hexview__set_format(n, value) == FALSE)
        {
            log_warn("%s %s %s", "hexview: format '", value, "' (hex|dec|oct|bin)");
        }
        return TRUE;
    }

    if (stdlib__strcmp(name, "ascii") == 0)
    {
        st->ascii = (boole)(stdlib__strcmp(value, "0") != 0 && stdlib__strcmp(value, "false") != 0);
        return TRUE;
    }

    if (stdlib__strcmp(name, "address") == 0 || stdlib__strcmp(name, "base") == 0)
    {
        if (_widget_hexview_internal__parse_number(value, &address) == FALSE)
        {
            log_warn("%s %s %s", "hexview: address '", value, "' is not a number");
            return TRUE;
        }

        st->base = address;
        return TRUE;
    }

    return FALSE;
}

static void hexview_layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale)
{
    _widget_hexview_internal__state_t* st     = NULL_POINTER;
    gui_style_t*                       s      = NULL_POINTER;
    gui_font_t*                        f      = NULL_POINTER;
    float                              border = 0.0f;
    float                              groups = 0.0f;
    float                              w      = 0.0f;
    float                              h      = 0.0f;

    (void)avail_h;

    st = _widget_hexview_internal__state(n);
    s  = &n->resolved;
    f  = _widget_hexview_internal__font(n, scale);

    if (st == NULL_POINTER)
    {
        n->bounds.x = x;
        n->bounds.y = y;
        n->bounds.w = 0.0f;
        n->bounds.h = 0.0f;
        return;
    }

    border     = scene__border_width(n, scale);
    st->row_h  = f != NULL_POINTER ? font__line_height_for(f, s, scale) : 16.0f * scale;
    st->char_w = f != NULL_POINTER ? font__measure(f, "0") : 8.0f * scale;
    st->gap_w  = s->gap > 0.0f ? s->gap * scale : st->char_w;
    st->cell_w = st->char_w * (float)_widget_hexview_internal__digits(st->format) + st->gap_w;

    if (st->group > 0)
    {
        groups = (float)stdlib__div64(st->cols - 1, st->group);
    }

    // as wide as the address, the values (one gap fewer than values) and the characters need
    w = st->char_w * (float)(_WIDGET_HEXVIEW_INTERNAL__ADDR_DIGITS + _WIDGET_HEXVIEW_INTERNAL__ADDR_GAP) + st->cell_w * (float)st->cols - st->gap_w + st->gap_w * groups;

    if (st->ascii)
    {
        w += st->char_w * (float)(_WIDGET_HEXVIEW_INTERNAL__ASCII_GAP + st->cols);
    }

    w += (s->pad_l + s->pad_r) * scale + border * 2.0f;
    h  = st->row_h * (float)st->rows + (s->pad_t + s->pad_b) * scale + border * 2.0f;

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

    // the extent the scroll module clamps against and sizes the thumb from
    n->content_w = w;
    n->content_h = st->row_h * (float)_widget_hexview_internal__rows(st) + (s->pad_t + s->pad_b) * scale + border * 2.0f;
    scroll__clamp(n);
}

static void hexview_emit_draws(gui_node_t* n, float scale)
{
    _widget_hexview_internal__state_t*   st = NULL_POINTER;
    _widget_hexview_internal__geometry_t g;
    gui_style_t*                         s  = NULL_POINTER;
    gui_font_t*                          f  = NULL_POINTER;
    gui_rect_t                           hl;
    gui_color_t                          text_color;
    gui_color_t                          accent;
    gui_color_t                          faint;
    gui_color_t                          highlight;
    char                                 buf[_WIDGET_HEXVIEW_INTERNAL__MAX_COLS + 12];
    float                                baseline = 0.0f;
    float                                top      = 0.0f;
    float                                ty       = 0.0f;
    float                                cx       = 0.0f;
    int64                                rows     = 0;
    int64                                first    = 0;
    int64                                r        = 0;
    int64                                c        = 0;
    int64                                len      = 0;
    int64                                offset   = 0;
    int64                                col      = 0;
    boole                                upper    = FALSE;
    ubyte                                b        = 0;

    stdlib__null_memory(&g, sizeof(g));
    stdlib__null_memory(&hl, sizeof(hl));
    stdlib__null_memory(&text_color, sizeof(text_color));
    stdlib__null_memory(&accent, sizeof(accent));
    stdlib__null_memory(&faint, sizeof(faint));
    stdlib__null_memory(&highlight, sizeof(highlight));
    stdlib__null_memory(buf, sizeof(buf));

    scene__emit_default_bg(n, scale);

    st = _widget_hexview_internal__state(n);
    s  = &n->resolved;
    f  = _widget_hexview_internal__font(n, scale);

    if (st == NULL_POINTER || f == NULL_POINTER)
    {
        return;
    }

    _widget_hexview_internal__geometry(n, st, scale, &g);

    text_color    = s->has_font_color ? s->font_color : scene__rgb(0.9f, 0.9f, 0.92f);
    accent        = s->has_accent_color ? s->accent_color : scene__rgb(0.39f, 0.40f, 0.95f);
    faint         = text_color;
    faint.a       = faint.a * 0.8f;
    highlight     = accent;
    highlight.a   = highlight.a * 0.35f;
    text_color.a *= n->effective_opacity;
    accent.a     *= n->effective_opacity;
    faint.a      *= n->effective_opacity;
    highlight.a  *= n->effective_opacity;
    upper         = (boole)(s->text_transform == GUI_TEXT_TRANSFORM__UPPERCASE);

    baseline = font__baseline_for(f, s, scale);
    rows     = _widget_hexview_internal__rows(st);

    if (rows == 0)
    {
        font__draw(f, g.inner.x, g.inner.y + baseline, faint, "(no buffer: widget_hexview__set_buffer)");
        return;
    }

    first = (int64)(n->scroll_y / st->row_h);

    if (first < 0)
    {
        first = 0;
    }

    renderer__push_scissor(g.inner);

    for (r = first; r < rows; r++)
    {
        top = g.inner.y + (float)r * st->row_h - n->scroll_y;
        ty  = top + baseline;

        if (top >= g.inner.y + g.inner.h)
        {
            break;
        }

        // the selection highlight goes under the text, in both columns
        if (st->selected >= 0 && stdlib__div64(st->selected, st->cols) == r)
        {
            col  = stdlib__rem64(st->selected, st->cols);
            hl.x = _widget_hexview_internal__cell_x(st, &g, col) - 1.0f * scale;
            hl.y = top;
            hl.w = st->char_w * (float)g.digits + 2.0f * scale;
            hl.h = st->row_h;
            renderer__submit_rect(hl, highlight, 2.0f * scale);

            if (st->ascii)
            {
                hl.x = g.ascii_x + st->char_w * (float)col;
                hl.w = st->char_w;
                renderer__submit_rect(hl, highlight, 2.0f * scale);
            }
        }

        // address
        _widget_hexview_internal__hex(buf, st->base + (uint64)(r * st->cols), _WIDGET_HEXVIEW_INTERNAL__ADDR_DIGITS, upper);
        buf[_WIDGET_HEXVIEW_INTERNAL__ADDR_DIGITS] = 0;
        font__draw(f, g.inner.x, ty, accent, buf);

        // values, one draw each so a gap can be any width
        for (c = 0; c < st->cols; c++)
        {
            offset = r * st->cols + c;

            if (offset >= st->len)
            {
                break;
            }

            cx = _widget_hexview_internal__cell_x(st, &g, c);
            _widget_hexview_internal__format_value(buf, st->data[offset], st->format, upper);
            font__draw_n(f, cx, ty, text_color, buf, g.digits);
        }

        // characters
        if (st->ascii)
        {
            len = 0;

            for (c = 0; c < st->cols; c++)
            {
                offset = r * st->cols + c;

                if (offset < st->len)
                {
                    b          = st->data[offset];
                    buf[len++] = (char)((b >= 32 && b < 127) ? b : '.');
                }
                else
                {
                    buf[len++] = ' ';
                }
            }

            buf[len] = 0;
            font__draw(f, g.ascii_x, ty, faint, buf);
        }
    }

    renderer__pop_scissor();

    if (scroll__vbar_visible(n))
    {
        scroll__draw_vbar(n, scale);
    }
}

static void hexview_on_mouse_down(gui_node_t* n, int64 x, int64 y, int64 button)
{
    _widget_hexview_internal__state_t* st = NULL_POINTER;
    int64 offset = -1;

    (void)button;

    st = _widget_hexview_internal__state(n);

    if (st == NULL_POINTER)
    {
        return;
    }

    // the scrollbar thumb first; a press elsewhere picks a byte
    scroll__on_mouse_down(n, x, y, scene__scale());

    if (n->scroll_drag_axis != 0)
    {
        return;
    }

    offset = _widget_hexview_internal__offset_at(n, st, scene__scale(), (float)x, (float)y);

    if (offset >= 0)
    {
        _widget_hexview_internal__select(n, st, offset);
    }
}

static boole hexview_on_mouse_drag(gui_node_t* n, int64 x, int64 y)
{
    (void)x;

    return scroll__on_mouse_drag(n, y, scene__scale());
}

static void hexview_on_mouse_up(gui_node_t* n, int64 x, int64 y, int64 button)
{
    (void)x;
    (void)y;
    (void)button;

    scroll__on_mouse_up(n);
}

static void hexview_on_key(gui_node_t* n, int64 vk, boole down)
{
    _widget_hexview_internal__state_t*   st = NULL_POINTER;
    _widget_hexview_internal__geometry_t g;
    int64 next         = 0;
    int64 rows_visible = 0;

    stdlib__null_memory(&g, sizeof(g));
    st = _widget_hexview_internal__state(n);

    if (st == NULL_POINTER || down == FALSE || st->len <= 0)
    {
        return;
    }

    next = st->selected < 0 ? 0 : st->selected;
    _widget_hexview_internal__geometry(n, st, scene__scale(), &g);
    rows_visible = st->row_h > 0.0f ? (int64)(g.inner.h / st->row_h) : 1;

    if (rows_visible < 1)
    {
        rows_visible = 1;
    }

    // Win32 virtual-key codes, compared by value so this file stays
    // platform-neutral: LEFT 0x25 UP 0x26 RIGHT 0x27 DOWN 0x28
    // PRIOR 0x21 NEXT 0x22 HOME 0x24 END 0x23
    if (vk == 0x25)
    {
        next = next - 1;
    }
    else if (vk == 0x27)
    {
        next = next + 1;
    }
    else if (vk == 0x26)
    {
        next = next - st->cols;
    }
    else if (vk == 0x28)
    {
        next = next + st->cols;
    }
    else if (vk == 0x21)
    {
        next = next - st->cols * rows_visible;
    }
    else if (vk == 0x22)
    {
        next = next + st->cols * rows_visible;
    }
    else if (vk == 0x24)
    {
        next = 0;
    }
    else if (vk == 0x23)
    {
        next = st->len - 1;
    }
    else
    {
        return;
    }

    if (next < 0)
    {
        next = 0;
    }

    if (next >= st->len)
    {
        next = st->len - 1;
    }

    _widget_hexview_internal__select(n, st, next);
}

void widget_hexview__set_buffer(gui_node_t* n, const void* data, int64 len, uint64 base_address)
{
    _widget_hexview_internal__state_t* st = NULL_POINTER;

    st = _widget_hexview_internal__state(n);

    if (st == NULL_POINTER)
    {
        return;
    }

    st->data           = (const ubyte*)data;
    st->len            = data != NULL_POINTER ? len : 0;
    st->base           = base_address;
    st->selected       = -1;
    n->scroll_y        = 0.0f;
    n->scroll_y_target = 0.0f;
}

int64 widget_hexview__selected(gui_node_t* n)
{
    _widget_hexview_internal__state_t* st = NULL_POINTER;

    st = _widget_hexview_internal__state(n);

    return st != NULL_POINTER ? st->selected : -1;
}

void widget_hexview__select(gui_node_t* n, int64 offset)
{
    _widget_hexview_internal__state_t* st = NULL_POINTER;

    st = _widget_hexview_internal__state(n);

    if (st == NULL_POINTER)
    {
        return;
    }

    if (offset < 0 || offset >= st->len)
    {
        st->selected = -1;
        return;
    }

    st->selected = offset;
    _widget_hexview_internal__reveal(n, st);
}

void widget_hexview__set_columns(gui_node_t* n, int64 columns)
{
    _widget_hexview_internal__state_t* st = NULL_POINTER;

    st = _widget_hexview_internal__state(n);

    if (st == NULL_POINTER)
    {
        return;
    }

    if (columns < 1)
    {
        columns = 1;
    }

    if (columns > _WIDGET_HEXVIEW_INTERNAL__MAX_COLS)
    {
        columns = _WIDGET_HEXVIEW_INTERNAL__MAX_COLS;
    }

    st->cols     = columns;
    st->selected = -1;
}

boole widget_hexview__set_format(gui_node_t* n, const char* format)
{
    _widget_hexview_internal__state_t* st = NULL_POINTER;

    st = _widget_hexview_internal__state(n);

    if (st == NULL_POINTER || format == NULL_POINTER)
    {
        return FALSE;
    }

    if (stdlib__strcmp(format, "hex") == 0)
    {
        st->format = _WIDGET_HEXVIEW_INTERNAL__FORMAT_HEX;
    }
    else if (stdlib__strcmp(format, "dec") == 0)
    {
        st->format = _WIDGET_HEXVIEW_INTERNAL__FORMAT_DEC;
    }
    else if (stdlib__strcmp(format, "oct") == 0)
    {
        st->format = _WIDGET_HEXVIEW_INTERNAL__FORMAT_OCT;
    }
    else if (stdlib__strcmp(format, "bin") == 0)
    {
        st->format = _WIDGET_HEXVIEW_INTERNAL__FORMAT_BIN;
    }
    else
    {
        return FALSE;
    }

    return TRUE;
}

static const widget_vtable_t g_hexview_vtable = {
    .type_name       = "hexview",
    .init_defaults   = hexview_init_defaults,
    .apply_attribute = hexview_apply_attribute,
    .layout          = hexview_layout,
    .emit_draws      = hexview_emit_draws,
    .on_mouse_down   = hexview_on_mouse_down,
    .on_mouse_drag   = hexview_on_mouse_drag,
    .on_mouse_up     = hexview_on_mouse_up,
    .on_key          = hexview_on_key,
    .takes_focus     = TRUE,
    .state_size      = sizeof(_widget_hexview_internal__state_t),
};

void widget_hexview__register(void)
{
    widget_registry__register(GUI_NODE_TYPE__HEXVIEW, &g_hexview_vtable);
}
