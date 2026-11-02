// widget_filepicker.c - <filepicker path="." filter=".ui;.style" show-hidden="0"/>
// A directory browser: the current path on the first line, then one
// row per entry, directories first with a trailing "/", files with
// their size on the right. A click on a directory enters it (the ".."
// row goes up), a click on a file selects it; both fire on_change with
// ev->file.path and ev->file.is_dir. The arrow keys move the
// selection, Return activates it, Backspace goes up.
// The listing comes from fs__list_dir, sorted here (directories first,
// then by name, case-insensitive). A relative `path` attribute is
// resolved against the directory of the .ui file, so a scene can point
// at its own folder. `filter` keeps only files whose name ends with one
// of the given extensions; `show-hidden` includes names starting with
// a dot.
// scrolling: the style gives the node overflow-y: auto (or scroll);
// the widget reports content_h and the shared scroll module handles
// the wheel, the thumb drag and the bar, as for a <div>.
// layout: the style's size / width / height when set, otherwise
// 320 x 240 logical px. draw: bg + border through
// scene__emit_default_bg; the path and directory names in
// accent-color, file names in font-color, sizes at 60 %, the selected
// row on an accent-color highlight.

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "widget_filepicker.h"
#include "scene.h"
#include "scroll.h"
#include "renderer.h"
#include "font.h"
#include "fs.h"
#include "parser_xml.h"
#include "third_party/log.h"
#include "clib/stdlib.h"
#include "clib/memory_manager.h"
#include "memory_types.h"

#define _WIDGET_FILEPICKER_INTERNAL__DEFAULT_W  320.0f
#define _WIDGET_FILEPICKER_INTERNAL__DEFAULT_H  240.0f
#define _WIDGET_FILEPICKER_INTERNAL__ROW_PAD    3.0f
#define _WIDGET_FILEPICKER_INTERNAL__HEADER_PAD 4.0f
#define _WIDGET_FILEPICKER_INTERNAL__HPAD       6.0f
#define _WIDGET_FILEPICKER_INTERNAL__PATH_MAX   1024
#define _WIDGET_FILEPICKER_INTERNAL__NAME_MAX   256

typedef struct _widget_filepicker_internal__entry_t
{
    char  name[_WIDGET_FILEPICKER_INTERNAL__NAME_MAX]; // 0x0
    boole is_dir;                                      // 0x100
    int64 size;                                        // 0x108
} _widget_filepicker_internal__entry_t;

typedef struct _widget_filepicker_internal__state_t
{
    char                                  path[_WIDGET_FILEPICKER_INTERNAL__PATH_MAX];   // 0x0
    char                                  picked[_WIDGET_FILEPICKER_INTERNAL__PATH_MAX]; // 0x400
    char                                  filter[64];                                    // 0x800
    _widget_filepicker_internal__entry_t* entries;                                       // 0x840
    int64                                 count;                                         // 0x848
    int64                                 cap;                                           // 0x850
    int64                                 selected;                                      // 0x858  row index, -1 = none
    boole                                 show_hidden;                                   // 0x860
    boole                                 dirty;                                         // 0x861  re-list on the next layout
    boole                                 has_parent;                                    // 0x862  row 0 is ".."
    boole                                 listed_ok;                                     // 0x863  the last listing succeeded
    float                                 row_h;                                         // 0x864  physical px, from the last layout
    float                                 header_h;                                      // 0x868
} _widget_filepicker_internal__state_t;

static _widget_filepicker_internal__state_t* _widget_filepicker_internal__state(gui_node_t* n);
static gui_font_t* _widget_filepicker_internal__font(gui_node_t* n, float scale);
static boole _widget_filepicker_internal__is_sep(char c);
static boole _widget_filepicker_internal__name_passes(_widget_filepicker_internal__state_t* st, const char* name, boole is_dir);
static void  _widget_filepicker_internal__on_entry(const char* name, boole is_dir, int64 size, void* user);
static int64 _widget_filepicker_internal__compare(const _widget_filepicker_internal__entry_t* a, const _widget_filepicker_internal__entry_t* b);
static void  _widget_filepicker_internal__sort(_widget_filepicker_internal__state_t* st);
static void  _widget_filepicker_internal__list(_widget_filepicker_internal__state_t* st);
static boole _widget_filepicker_internal__join(const char* dir, const char* name, char* out, int64 cap);
static boole _widget_filepicker_internal__parent(const char* path, char* out, int64 cap);
static int64 _widget_filepicker_internal__rows(_widget_filepicker_internal__state_t* st);
static void  _widget_filepicker_internal__inner(gui_node_t* n, float scale, gui_rect_t* out);
static int64 _widget_filepicker_internal__fmt_int(char* out, int64 v);
static int64 _widget_filepicker_internal__fmt_size(char* out, int64 size);
static void  _widget_filepicker_internal__notify(gui_node_t* n, const char* path, boole is_dir);
static void  _widget_filepicker_internal__enter(gui_node_t* n, _widget_filepicker_internal__state_t* st, const char* path);
static void  _widget_filepicker_internal__activate(gui_node_t* n, _widget_filepicker_internal__state_t* st, int64 row);
static void  _widget_filepicker_internal__reveal(gui_node_t* n, _widget_filepicker_internal__state_t* st);
static void  filepicker_init_defaults(gui_node_t* n);
static void  filepicker_on_destroy(gui_node_t* n);
static boole filepicker_apply_attribute(gui_node_t* n, char* name, char* value);
static void  filepicker_layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale);
static void  filepicker_emit_draws(gui_node_t* n, float scale);
static void  filepicker_on_mouse_down(gui_node_t* n, int64 x, int64 y, int64 button);
static boole filepicker_on_mouse_drag(gui_node_t* n, int64 x, int64 y);
static void  filepicker_on_mouse_up(gui_node_t* n, int64 x, int64 y, int64 button);
static void  filepicker_on_key(gui_node_t* n, int64 vk, boole down);

static _widget_filepicker_internal__state_t* _widget_filepicker_internal__state(gui_node_t* n)
{
    if (n == NULL_POINTER || n->type != GUI_NODE_TYPE__FILEPICKER)
    {
        return NULL_POINTER;
    }

    return (_widget_filepicker_internal__state_t*)scene__widget_state(n);
}

static gui_font_t* _widget_filepicker_internal__font(gui_node_t* n, float scale)
{
    float size_px = 0.0f;

    size_px = (n->resolved.font_size > 0.0f ? n->resolved.font_size : 14.0f) * scale;

    return font__at(n->resolved.font_family, size_px);
}

static boole _widget_filepicker_internal__is_sep(char c)
{
    return (boole)(c == '/' || c == '\\');
}

// hidden names and the extension filter; directories always pass the filter
static boole _widget_filepicker_internal__name_passes(_widget_filepicker_internal__state_t* st, const char* name, boole is_dir)
{
    const char* p        = NULL_POINTER;
    const char* end      = NULL_POINTER;
    const char* tail     = NULL_POINTER;
    size_t      name_len = 0;
    size_t      ext_len  = 0;
    size_t      i        = 0;
    boole       match    = FALSE;

    name_len = stdlib__strlen(name);
    p        = st->filter;

    if (st->show_hidden == FALSE && name[0] == '.')
    {
        return FALSE;
    }

    if (is_dir || st->filter[0] == 0)
    {
        return TRUE;
    }

    // ";"-separated extensions, compared case-insensitively against the tail of the name
    while (*p != 0)
    {
        end = p;

        while (*end != 0 && *end != ';')
        {
            end++;
        }

        ext_len = (size_t)(end - p);

        if (ext_len > 0 && ext_len <= name_len)
        {
            tail  = name + name_len - ext_len;
            match = TRUE;

            for (i = 0; i < ext_len; i++)
            {
                if (stdlib__tolower((int)(unsigned char)tail[i]) != stdlib__tolower((int)(unsigned char)p[i]))
                {
                    match = FALSE;
                    break;
                }
            }

            if (match)
            {
                return TRUE;
            }
        }

        p = *end == ';' ? end + 1 : end;
    }

    return FALSE;
}

// fs__list_dir callback: appends an entry, growing the array as needed
static void _widget_filepicker_internal__on_entry(const char* name, boole is_dir, int64 size, void* user)
{
    _widget_filepicker_internal__state_t* st      = NULL_POINTER;
    _widget_filepicker_internal__entry_t* e       = NULL_POINTER;
    _widget_filepicker_internal__entry_t* grown   = NULL_POINTER;
    int64                                 new_cap = 0;
    size_t                                len     = 0;

    st = (_widget_filepicker_internal__state_t*)user;

    if (_widget_filepicker_internal__name_passes(st, name, is_dir) == FALSE)
    {
        return;
    }

    if (st->count == st->cap)
    {
        new_cap = st->cap > 0 ? st->cap * 2 : 32;
        grown   = (_widget_filepicker_internal__entry_t*)GUI_MALLOC_T((size_t)new_cap * sizeof(_widget_filepicker_internal__entry_t), MM_TYPE__GENERIC);

        if (grown == NULL_POINTER)
        {
            return;
        }

        stdlib__null_memory(grown, (int64)((size_t)new_cap * sizeof(_widget_filepicker_internal__entry_t)));

        if (st->entries != NULL_POINTER)
        {
            stdlib__memcpy(grown, st->entries, (size_t)st->count * sizeof(_widget_filepicker_internal__entry_t));
            GUI_FREE(st->entries);
        }

        st->entries = grown;
        st->cap     = new_cap;
    }

    e   = &st->entries[st->count];
    len = stdlib__strlen(name);

    if (len >= sizeof(e->name))
    {
        len = sizeof(e->name) - 1;
    }

    stdlib__memcpy(e->name, name, len);
    e->name[len] = 0;
    e->is_dir    = is_dir;
    e->size      = size;
    st->count++;
}

// directories first, then case-insensitive by name
static int64 _widget_filepicker_internal__compare(const _widget_filepicker_internal__entry_t* a, const _widget_filepicker_internal__entry_t* b)
{
    size_t i  = 0;
    int    ca = 0;
    int    cb = 0;

    if (a->is_dir != b->is_dir)
    {
        return a->is_dir ? -1 : 1;
    }

    for (i = 0; ; i++)
    {
        ca = stdlib__tolower((int)(unsigned char)a->name[i]);
        cb = stdlib__tolower((int)(unsigned char)b->name[i]);

        if (ca != cb)
        {
            return ca < cb ? -1 : 1;
        }

        if (ca == 0)
        {
            return 0;
        }
    }
}

// insertion sort: listings are small and mostly ordered already
static void _widget_filepicker_internal__sort(_widget_filepicker_internal__state_t* st)
{
    _widget_filepicker_internal__entry_t key;
    int64 i = 0;
    int64 j = 0;

    stdlib__null_memory(&key, sizeof(key));

    for (i = 1; i < st->count; i++)
    {
        key = st->entries[i];
        j   = i - 1;

        while (j >= 0 && _widget_filepicker_internal__compare(&st->entries[j], &key) > 0)
        {
            st->entries[j + 1] = st->entries[j];
            j--;
        }

        st->entries[j + 1] = key;
    }
}

static void _widget_filepicker_internal__list(_widget_filepicker_internal__state_t* st)
{
    char parent[_WIDGET_FILEPICKER_INTERNAL__PATH_MAX];

    stdlib__null_memory(parent, sizeof(parent));

    st->count      = 0;
    st->has_parent = _widget_filepicker_internal__parent(st->path, parent, (int64)sizeof(parent));
    st->listed_ok  = fs__list_dir(st->path, _widget_filepicker_internal__on_entry, st);
    st->dirty      = FALSE;

    if (st->listed_ok == FALSE)
    {
        log_warn("%s %s %s", "filepicker: cannot list '", st->path, "'");
    }

    _widget_filepicker_internal__sort(st);
}

// dir + "/" + name; FALSE when it does not fit
static boole _widget_filepicker_internal__join(const char* dir, const char* name, char* out, int64 cap)
{
    size_t dl  = 0;
    size_t nl  = 0;
    boole  sep = FALSE;

    dl  = stdlib__strlen(dir);
    nl  = stdlib__strlen(name);
    sep = (boole)(dl > 0 && _widget_filepicker_internal__is_sep(dir[dl - 1]) == FALSE);

    if ((int64)(dl + (sep ? 1 : 0) + nl + 1) > cap)
    {
        return FALSE;
    }

    stdlib__memcpy(out, dir, dl);

    if (sep)
    {
        out[dl++] = '/';
    }

    stdlib__memcpy(out + dl, name, nl);
    out[dl + nl] = 0;

    return TRUE;
}

// the directory above `path`; FALSE at a root ("/", "C:/", "C:")
static boole _widget_filepicker_internal__parent(const char* path, char* out, int64 cap)
{
    int64 len = 0;
    int64 end = 0;
    int64 cut = 0;

    len = (int64)stdlib__strlen(path);
    end = len;

    // ignore a trailing separator
    while (end > 0 && _widget_filepicker_internal__is_sep(path[end - 1]))
    {
        end--;
    }

    cut = end;

    while (cut > 0 && _widget_filepicker_internal__is_sep(path[cut - 1]) == FALSE)
    {
        cut--;
    }

    if (cut == 0)
    {
        // no separator left: a bare name has a parent (".") unless it is a drive or empty
        if (end == 0 || (end == 2 && path[1] == ':'))
        {
            return FALSE;
        }

        if (cap < 2)
        {
            return FALSE;
        }

        out[0] = '.';
        out[1] = 0;
        return TRUE;
    }

    // keep the separator when it is the root itself ("/" or "C:/")
    if (cut == 1 || (cut == 3 && path[1] == ':'))
    {
        if (end <= cut)
        {
            return FALSE;
        }
    }
    else
    {
        cut--;
    }

    if (cut + 1 > cap)
    {
        return FALSE;
    }

    stdlib__memcpy(out, path, (size_t)cut);
    out[cut] = 0;

    return TRUE;
}

static int64 _widget_filepicker_internal__rows(_widget_filepicker_internal__state_t* st)
{
    return st->count + (st->has_parent ? 1 : 0);
}

static void _widget_filepicker_internal__inner(gui_node_t* n, float scale, gui_rect_t* out)
{
    gui_style_t* s      = NULL_POINTER;
    float        border = 0.0f;

    s      = &n->resolved;
    border = scene__border_width(n, scale);

    out->x = n->bounds.x + border + s->pad_l * scale;
    out->y = n->bounds.y + border + s->pad_t * scale;
    out->w = n->bounds.w - border * 2.0f - (s->pad_l + s->pad_r) * scale;
    out->h = n->bounds.h - border * 2.0f - (s->pad_t + s->pad_b) * scale;
}

// decimal into out, no terminator written; returns the length
static int64 _widget_filepicker_internal__fmt_int(char* out, int64 v)
{
    char  tmp[24];
    int64 len = 0;
    int64 i   = 0;

    stdlib__null_memory(tmp, sizeof(tmp));

    do
    {
        int64 digit;
        v = stdlib__divmod64(v, 10, &digit);
        tmp[len++] = (char)('0' + digit);
    } while (v > 0 && len < 20);

    while (len > 0)
    {
        out[i++] = tmp[--len];
    }

    return i;
}

// "512 B", "12.3 KB", "4.0 MB", "1.2 GB"; returns the length, writes no terminator
static int64 _widget_filepicker_internal__fmt_size(char* out, int64 size)
{
    static const char* units[4] = { " B", " KB", " MB", " GB" };
    int64  unit   = 0;
    int64  tenths = 0;
    int64  len    = 0;
    size_t ul     = 0;

    while (size >= 1024 && unit < 3)
    {
        tenths = stdlib__div64(stdlib__rem64(size, 1024) * 10, 1024);
        size   = stdlib__div64(size, 1024);
        unit++;
    }

    len += _widget_filepicker_internal__fmt_int(out + len, size);

    if (unit > 0)
    {
        out[len++] = '.';
        out[len++] = (char)('0' + tenths);
    }

    ul = stdlib__strlen(units[unit]);
    stdlib__memcpy(out + len, units[unit], ul);
    len += (int64)ul;

    return len;
}

static void _widget_filepicker_internal__notify(gui_node_t* n, const char* path, boole is_dir)
{
    gui_event_t ev;

    stdlib__null_memory(&ev, sizeof(ev));

    ev.type        = GUI_EVENT_TYPE__CHANGE;
    ev.file.path   = path;
    ev.file.is_dir = is_dir;
    scene__dispatch_event(n, &ev);
}

// shows another directory: the listing is read on the next layout
static void _widget_filepicker_internal__enter(gui_node_t* n, _widget_filepicker_internal__state_t* st, const char* path)
{
    size_t len = 0;

    len = stdlib__strlen(path);

    if (len >= sizeof(st->path))
    {
        log_warn("%s %s", "filepicker: path too long:", path);
        return;
    }

    stdlib__memcpy(st->path, path, len + 1);
    st->dirty          = TRUE;
    st->selected       = -1;
    n->scroll_y        = 0.0f;
    n->scroll_y_target = 0.0f;
    _widget_filepicker_internal__notify(n, st->path, TRUE);
}

// a row was clicked or confirmed: enter the directory or pick the file
static void _widget_filepicker_internal__activate(gui_node_t* n, _widget_filepicker_internal__state_t* st, int64 row)
{
    _widget_filepicker_internal__entry_t* e = NULL_POINTER;
    char next[_WIDGET_FILEPICKER_INTERNAL__PATH_MAX];

    stdlib__null_memory(next, sizeof(next));

    if (row < 0 || row >= _widget_filepicker_internal__rows(st))
    {
        return;
    }

    if (st->has_parent && row == 0)
    {
        if (_widget_filepicker_internal__parent(st->path, next, (int64)sizeof(next)))
        {
            _widget_filepicker_internal__enter(n, st, next);
        }
        return;
    }

    e = &st->entries[row - (st->has_parent ? 1 : 0)];

    if (_widget_filepicker_internal__join(st->path, e->name, next, (int64)sizeof(next)) == FALSE)
    {
        log_warn("%s %s", "filepicker: path too long for", e->name);
        return;
    }

    if (e->is_dir)
    {
        _widget_filepicker_internal__enter(n, st, next);
        return;
    }

    st->selected = row;
    stdlib__memcpy(st->picked, next, stdlib__strlen(next) + 1);
    _widget_filepicker_internal__notify(n, st->picked, FALSE);
}

// scrolls so the selected row is inside the list box
static void _widget_filepicker_internal__reveal(gui_node_t* n, _widget_filepicker_internal__state_t* st)
{
    gui_rect_t inner;
    float      list_h  = 0.0f;
    float      row_top = 0.0f;

    stdlib__null_memory(&inner, sizeof(inner));

    if (st->selected < 0 || st->row_h <= 0.0f)
    {
        return;
    }

    _widget_filepicker_internal__inner(n, scene__scale(), &inner);
    list_h  = inner.h - st->header_h;
    row_top = (float)st->selected * st->row_h;

    if (row_top < n->scroll_y)
    {
        n->scroll_y = row_top;
    }
    else if (row_top + st->row_h > n->scroll_y + list_h)
    {
        n->scroll_y = row_top + st->row_h - list_h;
    }

    n->scroll_y_target = n->scroll_y;
    scroll__clamp(n);
}

static void filepicker_init_defaults(gui_node_t* n)
{
    _widget_filepicker_internal__state_t* st = NULL_POINTER;

    st = _widget_filepicker_internal__state(n);

    if (st == NULL_POINTER)
    {
        return;
    }

    st->path[0]  = '.';
    st->path[1]  = 0;
    st->selected = -1;
    st->dirty    = TRUE;
}

static void filepicker_on_destroy(gui_node_t* n)
{
    _widget_filepicker_internal__state_t* st = NULL_POINTER;

    st = _widget_filepicker_internal__state(n);

    if (st == NULL_POINTER)
    {
        return;
    }

    if (st->entries != NULL_POINTER)
    {
        GUI_FREE(st->entries);
        st->entries = NULL_POINTER;
    }

    st->count = 0;
    st->cap   = 0;
}

static boole filepicker_apply_attribute(gui_node_t* n, char* name, char* value)
{
    _widget_filepicker_internal__state_t* st = NULL_POINTER;
    const char* base     = NULL_POINTER;
    char        joined[_WIDGET_FILEPICKER_INTERNAL__PATH_MAX];
    size_t      len      = 0;
    boole       absolute = FALSE;

    stdlib__null_memory(joined, sizeof(joined));
    st = _widget_filepicker_internal__state(n);

    if (st == NULL_POINTER)
    {
        return FALSE;
    }

    if (stdlib__strcmp(name, "path") == 0)
    {
        // relative paths are relative to the .ui file, like image sources
        base     = parser_xml__base_dir();
        absolute = (boole)(_widget_filepicker_internal__is_sep(value[0]) || (value[0] != 0 && value[1] == ':'));

        if (absolute == FALSE && base != NULL_POINTER && base[0] != 0 && _widget_filepicker_internal__join(base, value, joined, (int64)sizeof(joined)))
        {
            widget_filepicker__set_path(n, joined);
            return TRUE;
        }

        widget_filepicker__set_path(n, value);
        return TRUE;
    }

    if (stdlib__strcmp(name, "filter") == 0)
    {
        len = stdlib__strlen(value);

        if (len >= sizeof(st->filter))
        {
            len = sizeof(st->filter) - 1;
        }

        stdlib__memcpy(st->filter, value, len);
        st->filter[len] = 0;
        st->dirty       = TRUE;
        return TRUE;
    }

    if (stdlib__strcmp(name, "show-hidden") == 0)
    {
        st->show_hidden = (boole)(stdlib__strcmp(value, "0") != 0 && stdlib__strcmp(value, "false") != 0);
        st->dirty       = TRUE;
        return TRUE;
    }

    return FALSE;
}

static void filepicker_layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale)
{
    _widget_filepicker_internal__state_t* st     = NULL_POINTER;
    gui_style_t*                          s      = NULL_POINTER;
    gui_font_t*                           f      = NULL_POINTER;
    float                                 line_h = 0.0f;
    float                                 border = 0.0f;
    float                                 w      = 0.0f;
    float                                 h      = 0.0f;

    (void)avail_h;

    st = _widget_filepicker_internal__state(n);
    s  = &n->resolved;
    f  = _widget_filepicker_internal__font(n, scale);

    if (st == NULL_POINTER)
    {
        n->bounds.x = x;
        n->bounds.y = y;
        n->bounds.w = 0.0f;
        n->bounds.h = 0.0f;
        return;
    }

    if (st->dirty)
    {
        _widget_filepicker_internal__list(st);
    }

    line_h       = f != NULL_POINTER ? font__line_height_for(f, s, scale) : 16.0f * scale;
    border       = scene__border_width(n, scale);
    st->row_h    = line_h + _WIDGET_FILEPICKER_INTERNAL__ROW_PAD * 2.0f * scale;
    st->header_h = line_h + _WIDGET_FILEPICKER_INTERNAL__HEADER_PAD * 2.0f * scale;
    w            = _WIDGET_FILEPICKER_INTERNAL__DEFAULT_W * scale;
    h            = _WIDGET_FILEPICKER_INTERNAL__DEFAULT_H * scale;

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

    // the extent the scroll module clamps against: the header stays, the rows scroll
    n->content_w = w;
    n->content_h = st->header_h + st->row_h * (float)_widget_filepicker_internal__rows(st) + (s->pad_t + s->pad_b) * scale + border * 2.0f;
    scroll__clamp(n);
}

static void filepicker_emit_draws(gui_node_t* n, float scale)
{
    _widget_filepicker_internal__state_t* st = NULL_POINTER;
    _widget_filepicker_internal__entry_t* e  = NULL_POINTER;
    gui_style_t*                          s  = NULL_POINTER;
    gui_font_t*                           f  = NULL_POINTER;
    gui_rect_t                            inner;
    gui_rect_t                            list;
    gui_rect_t                            hl;
    gui_color_t                           text_color;
    gui_color_t                           accent;
    gui_color_t                           faint;
    gui_color_t                           highlight;
    char                                  shown[_WIDGET_FILEPICKER_INTERNAL__PATH_MAX + 4];
    char                                  buf[_WIDGET_FILEPICKER_INTERNAL__NAME_MAX + 8];
    float                                 baseline = 0.0f;
    float                                 hpad     = 0.0f;
    float                                 avail    = 0.0f;
    float                                 ty       = 0.0f;
    float                                 top      = 0.0f;
    float                                 sw       = 0.0f;
    size_t                                plen     = 0;
    size_t                                skip     = 0;
    size_t                                nl       = 0;
    int64                                 rows     = 0;
    int64                                 first    = 0;
    int64                                 r        = 0;
    int64                                 sl       = 0;

    stdlib__null_memory(&inner, sizeof(inner));
    stdlib__null_memory(&list, sizeof(list));
    stdlib__null_memory(&hl, sizeof(hl));
    stdlib__null_memory(&text_color, sizeof(text_color));
    stdlib__null_memory(&accent, sizeof(accent));
    stdlib__null_memory(&faint, sizeof(faint));
    stdlib__null_memory(&highlight, sizeof(highlight));
    stdlib__null_memory(shown, sizeof(shown));
    stdlib__null_memory(buf, sizeof(buf));

    scene__emit_default_bg(n, scale);

    st = _widget_filepicker_internal__state(n);
    s  = &n->resolved;
    f  = _widget_filepicker_internal__font(n, scale);

    if (st == NULL_POINTER || f == NULL_POINTER)
    {
        return;
    }

    _widget_filepicker_internal__inner(n, scale, &inner);

    text_color    = s->has_font_color ? s->font_color : scene__rgb(0.9f, 0.9f, 0.92f);
    accent        = s->has_accent_color ? s->accent_color : scene__rgb(0.39f, 0.40f, 0.95f);
    faint         = text_color;
    faint.a       = faint.a * 0.6f;
    highlight     = accent;
    highlight.a   = highlight.a * 0.3f;
    text_color.a *= n->effective_opacity;
    accent.a     *= n->effective_opacity;
    faint.a      *= n->effective_opacity;
    highlight.a  *= n->effective_opacity;

    baseline = font__baseline_for(f, s, scale);
    hpad     = _WIDGET_FILEPICKER_INTERNAL__HPAD * scale;

    // the header: the path, elided from the left when it is too wide
    avail = inner.w - hpad * 2.0f;
    ty    = inner.y + _WIDGET_FILEPICKER_INTERNAL__HEADER_PAD * scale + baseline;
    plen  = stdlib__strlen(st->path);

    while (skip < plen && font__measure(f, st->path + skip) + (skip > 0 ? font__measure(f, "...") : 0.0f) > avail)
    {
        skip++;
    }

    if (skip > 0)
    {
        stdlib__memcpy(shown, "...", 3);
        stdlib__memcpy(shown + 3, st->path + skip, plen - skip + 1);
    }
    else
    {
        stdlib__memcpy(shown, st->path, plen + 1);
    }

    font__draw(f, inner.x + hpad, ty, accent, shown);

    if (st->listed_ok == FALSE)
    {
        font__draw(f, inner.x + hpad, ty + st->header_h, faint, "(directory not readable)");
    }

    list.x = inner.x;
    list.y = inner.y + st->header_h;
    list.w = inner.w;
    list.h = inner.h - st->header_h;

    if (list.h <= 0.0f)
    {
        return;
    }

    rows  = _widget_filepicker_internal__rows(st);
    first = st->row_h > 0.0f ? (int64)(n->scroll_y / st->row_h) : 0;

    if (first < 0)
    {
        first = 0;
    }

    renderer__push_scissor(list);

    for (r = first; r < rows; r++)
    {
        top = list.y + (float)r * st->row_h - n->scroll_y;
        ty  = top + _WIDGET_FILEPICKER_INTERNAL__ROW_PAD * scale + baseline;

        if (top >= list.y + list.h)
        {
            break;
        }

        if (r == st->selected)
        {
            hl.x = list.x;
            hl.y = top;
            hl.w = list.w;
            hl.h = st->row_h;
            renderer__submit_rect(hl, highlight, 3.0f * scale);
        }

        if (st->has_parent && r == 0)
        {
            font__draw(f, list.x + hpad, ty, accent, "../");
            continue;
        }

        e  = &st->entries[r - (st->has_parent ? 1 : 0)];
        nl = stdlib__strlen(e->name);
        stdlib__memcpy(buf, e->name, nl);

        if (e->is_dir)
        {
            buf[nl++] = '/';
        }

        buf[nl] = 0;
        font__draw(f, list.x + hpad, ty, e->is_dir ? accent : text_color, buf);

        if (e->is_dir == FALSE)
        {
            sl      = _widget_filepicker_internal__fmt_size(buf, e->size);
            buf[sl] = 0;
            sw      = font__measure(f, buf);
            font__draw(f, list.x + list.w - hpad - sw, ty, faint, buf);
        }
    }

    renderer__pop_scissor();

    if (scroll__vbar_visible(n))
    {
        scroll__draw_vbar(n, scale);
    }
}

static void filepicker_on_mouse_down(gui_node_t* n, int64 x, int64 y, int64 button)
{
    (void)button;

    scroll__on_mouse_down(n, x, y, scene__scale());
}

static boole filepicker_on_mouse_drag(gui_node_t* n, int64 x, int64 y)
{
    (void)x;

    return scroll__on_mouse_drag(n, y, scene__scale());
}

static void filepicker_on_mouse_up(gui_node_t* n, int64 x, int64 y, int64 button)
{
    _widget_filepicker_internal__state_t* st = NULL_POINTER;
    gui_rect_t inner;
    float      fx     = 0.0f;
    float      fy     = 0.0f;
    float      list_y = 0.0f;
    int64      row    = 0;
    boole      was_dragging = FALSE;

    (void)button;

    stdlib__null_memory(&inner, sizeof(inner));
    st = _widget_filepicker_internal__state(n);
    fx = (float)x;
    fy = (float)y;

    if (st == NULL_POINTER)
    {
        return;
    }

    was_dragging = (boole)(n->scroll_drag_axis != 0);
    scroll__on_mouse_up(n);

    if (was_dragging)
    {
        return;
    }

    _widget_filepicker_internal__inner(n, scene__scale(), &inner);
    list_y = inner.y + st->header_h;

    if (fx < inner.x || fx >= inner.x + inner.w || fy < list_y || fy >= inner.y + inner.h || st->row_h <= 0.0f)
    {
        return;
    }

    row = (int64)((fy - list_y + n->scroll_y) / st->row_h);
    _widget_filepicker_internal__activate(n, st, row);
}

static void filepicker_on_key(gui_node_t* n, int64 vk, boole down)
{
    _widget_filepicker_internal__state_t* st = NULL_POINTER;
    int64 rows = 0;

    st = _widget_filepicker_internal__state(n);

    if (st == NULL_POINTER || down == FALSE)
    {
        return;
    }

    rows = _widget_filepicker_internal__rows(st);

    // Win32 virtual-key codes by value: UP 0x26 DOWN 0x28 RETURN 0x0D BACK 0x08
    if (vk == 0x26 && rows > 0)
    {
        st->selected = st->selected <= 0 ? 0 : st->selected - 1;
        _widget_filepicker_internal__reveal(n, st);
    }
    else if (vk == 0x28 && rows > 0)
    {
        st->selected = st->selected < 0 ? 0 : (st->selected + 1 < rows ? st->selected + 1 : rows - 1);
        _widget_filepicker_internal__reveal(n, st);
    }
    else if (vk == 0x0D)
    {
        _widget_filepicker_internal__activate(n, st, st->selected);
    }
    else if (vk == 0x08 && st->has_parent)
    {
        _widget_filepicker_internal__activate(n, st, 0);
    }
}

boole widget_filepicker__set_path(gui_node_t* n, char* path)
{
    _widget_filepicker_internal__state_t* st = NULL_POINTER;
    size_t len = 0;

    st = _widget_filepicker_internal__state(n);

    if (st == NULL_POINTER || path == NULL_POINTER)
    {
        return FALSE;
    }

    len = stdlib__strlen(path);

    if (len == 0 || len >= sizeof(st->path))
    {
        return FALSE;
    }

    stdlib__memcpy(st->path, path, len + 1);
    st->dirty          = TRUE;
    st->selected       = -1;
    n->scroll_y        = 0.0f;
    n->scroll_y_target = 0.0f;

    return TRUE;
}

const char* widget_filepicker__path(gui_node_t* n)
{
    _widget_filepicker_internal__state_t* st = NULL_POINTER;

    st = _widget_filepicker_internal__state(n);

    return st != NULL_POINTER ? st->path : NULL_POINTER;
}

const char* widget_filepicker__selected_path(gui_node_t* n)
{
    _widget_filepicker_internal__state_t* st = NULL_POINTER;

    st = _widget_filepicker_internal__state(n);

    if (st == NULL_POINTER || st->picked[0] == 0)
    {
        return NULL_POINTER;
    }

    return st->picked;
}

void widget_filepicker__refresh(gui_node_t* n)
{
    _widget_filepicker_internal__state_t* st = NULL_POINTER;

    st = _widget_filepicker_internal__state(n);

    if (st != NULL_POINTER)
    {
        st->dirty = TRUE;
    }
}

static const widget_vtable_t g_filepicker_vtable = {
    .type_name       = "filepicker",
    .init_defaults   = filepicker_init_defaults,
    .on_destroy      = filepicker_on_destroy,
    .apply_attribute = filepicker_apply_attribute,
    .layout          = filepicker_layout,
    .emit_draws      = filepicker_emit_draws,
    .on_mouse_down   = filepicker_on_mouse_down,
    .on_mouse_drag   = filepicker_on_mouse_drag,
    .on_mouse_up     = filepicker_on_mouse_up,
    .on_key          = filepicker_on_key,
    .consumes_click  = TRUE,
    .takes_focus     = TRUE,
    .state_size      = sizeof(_widget_filepicker_internal__state_t),
};

void widget_filepicker__register(void)
{
    widget_registry__register(GUI_NODE_TYPE__FILEPICKER, &g_filepicker_vtable);
}
