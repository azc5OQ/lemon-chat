// Multiline UTF-8 editor. Clipboard and selection mutations share the input
// implementation; wrapping determines visual caret and pointer positions.
#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "scene_text.h"
#include "renderer.h"
#include "font.h"
#include "clib/stdlib.h"

#define _WIDGET_TEXTAREA_INTERNAL__HPAD 8.0f
#define _WIDGET_TEXTAREA_INTERNAL__VPAD 6.0f

// Walk text from `start` forward and find the byte index at which a
// line break falls. Returns the number of bytes to draw on the
// current line (excluding the break). Break priorities, high to low:
// 1. an explicit '\n' (consumes it -- caller skips 1 byte past)
// 2. last whitespace before content would overflow wrap_w
// 3. a hard break at the character that would cause the overflow
// Third case yields character-level wrapping for tokens wider than
// the widget (long URLs, code, etc.), matching CSS
// `overflow-wrap: anywhere` behaviour.
static int64 _widget_textarea_internal__line_len(gui_font_t* f, char* text, int64 from, int64 total, float wrap_w, boole* out_break_is_newline)
{
    *out_break_is_newline = FALSE;
    if (text == NULL_POINTER)
    {
        return total - from;
    }
    float x           = 0.0f;
    int64 last_break  = -1;
    uint previous = 0;
    for (int64 i = from; i < total;)
    {
        char c = text[i];
        if (c == '\n')
        {
            *out_break_is_newline = TRUE;
            return i - from;
        }
        if (c == ' ' || c == '\t')
        {
            last_break = i;
        }
        int64 next = i + 1;
        while (next < total && ((ubyte)text[next] & 0xc0) == 0x80)
        {
            next++;
        }
        int bytes = (int)(next - i);
        uint cp = bytes == 1 ? (ubyte)c : (ubyte)c & (0x7f >> bytes);
        for (int j = 1; j < bytes; j++)
        {
            cp = (cp << 6) | ((ubyte)text[i + j] & 63);
        }
        x += f != NULL_POINTER ? font__kern(f, previous, cp) + font__measure_n(f, &text[i], bytes) : 1.0f;
        previous = cp;
        if (x > wrap_w && last_break >= from)
        {
            // break at the last whitespace we saw on this line.
            // caller skips the whitespace byte via the "+1" advance
            // on soft-wrap returns (handled below).
            return last_break - from;
        }
        if (x > wrap_w && i > from)
        {
            // long run with no break. split at this character (hard
            // break) -- returning i - from, caller picks up at i.
            return i - from;
        }
        i = next;
    }
    return total - from;
}

static void textarea_layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale)
{
    gui_style_t* s = &n->resolved;
    float w = (s->size_w > 0.0f) ? s->size_w * scale : (avail_w > 0.0f ? avail_w : 240.0f * scale);
    float h = (s->size_h > 0.0f) ? s->size_h * scale : 120.0f * scale;
    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;
}

typedef struct textarea_view_t
{
    gui_font_t* font;
    gui_rect_t clip;
    float line_h;
    float ascent;
} textarea_view_t;

typedef struct textarea_line_t
{
    int64 start;
    int64 end;
    int64 next;
    boole has_next;
} textarea_line_t;

static textarea_view_t textarea_view(gui_node_t* n, float scale)
{
    textarea_view_t view;
    float size = (n->resolved.font_size > 0 ? n->resolved.font_size : 14) * scale;
    view.font = font__at(n->resolved.font_family, size);
    view.line_h = view.font != NULL_POINTER ? font__line_height_for(view.font, &n->resolved, scale) : size;
    view.line_h = stdlib__fmaxf(1, view.line_h);
    view.ascent = view.font != NULL_POINTER ? font__baseline_for(view.font, &n->resolved, scale) : 0;
    view.clip.x = n->bounds.x + _WIDGET_TEXTAREA_INTERNAL__HPAD * scale;
    view.clip.y = n->bounds.y + _WIDGET_TEXTAREA_INTERNAL__VPAD * scale;
    view.clip.w = stdlib__fmaxf(1, n->bounds.w - 2 * _WIDGET_TEXTAREA_INTERNAL__HPAD * scale);
    view.clip.h = stdlib__fmaxf(0, n->bounds.h - 2 * _WIDGET_TEXTAREA_INTERNAL__VPAD * scale);
    return view;
}

// Include a final empty line after a trailing LF. A hard wrap shares a byte
// offset with the following line; prefer_line_end records which side was hit.
static textarea_line_t textarea_line(gui_node_t* n, textarea_view_t* view, int64 start)
{
    textarea_line_t line;
    boole newline = FALSE;
    line.start = start;
    line.end = start + _widget_textarea_internal__line_len(view->font, n->text, start, n->text_len, view->clip.w, &newline);
    line.next = line.end;
    line.has_next = line.end < n->text_len;
    if (line.has_next && (newline || n->text[line.end] == ' ' || n->text[line.end] == '\t'))
    {
        line.next++;
    }
    return line;
}

static boole textarea_contains(textarea_line_t* line, scene_text_state_t* state)
{
    return !line->has_next || state->caret < line->next ||
        (state->prefer_line_end && state->caret == line->end);
}

static float textarea_prefix(gui_node_t* n, textarea_view_t* view, textarea_line_t* line, int64 at)
{
    at = at < line->end ? at : line->end;
    if (view->font != NULL_POINTER)
    {
        return font__measure_n(view->font, n->text + line->start, at - line->start);
    }
    float width = 0;
    for (int64 i = line->start; i < at; i = scene_text__next(n, i))
    {
        width++;
    }
    return width;
}

static int64 textarea_hit(gui_node_t* n, textarea_view_t* view, textarea_line_t* line, float x)
{
    if (view->font != NULL_POINTER)
    {
        return line->start + font__text_position(view->font, n->text + line->start, line->end - line->start, x);
    }
    int64 at = line->start;
    for (float width = .5f; at < line->end && x >= width; width++)
    {
        at = scene_text__next(n, at);
    }
    return at;
}

static textarea_line_t textarea_row(gui_node_t* n, textarea_view_t* view, int64 row)
{
    textarea_line_t line = textarea_line(n, view, 0);
    for (int64 i = 0; i < row && line.has_next; i++)
    {
        line = textarea_line(n, view, line.next);
    }
    return line;
}

static float textarea_measure(gui_node_t* n, textarea_view_t* view, scene_text_state_t* state, textarea_line_t* caret_line)
{
    textarea_line_t line = textarea_line(n, view, 0);
    int64 row = 0;
    float caret_y = 0;
    boole found = FALSE;
    for (;;)
    {
        if (!found && textarea_contains(&line, state))
        {
            caret_y = row * view->line_h;
            if (caret_line != NULL_POINTER)
            {
                *caret_line = line;
            }
            found = TRUE;
        }
        row++;
        if (!line.has_next)
        {
            break;
        }
        line = textarea_line(n, view, line.next);
    }
    n->content_h = row * view->line_h;
    return caret_y;
}

static void textarea_clamp_scroll(gui_node_t* n, textarea_view_t* view)
{
    float maximum = stdlib__fmaxf(0, n->content_h - stdlib__fmaxf(view->line_h, view->clip.h));
    n->scroll_y = stdlib__fmaxf(0, stdlib__fminf(n->scroll_y, maximum));
    n->scroll_y_target = n->scroll_y;
}

static void textarea_emit_draws(gui_node_t* n, float scale)
{
    scene__emit_default_bg(n, scale);
    scene_text_state_t* state = scene_text__state(n);
    if (state == NULL_POINTER)
    {
        return;
    }
    textarea_view_t view = textarea_view(n, scale);
    textarea_line_t caret_line;
    float caret_y = textarea_measure(n, &view, state, &caret_line);
    if (scene__focus() == n && state->reveal_caret)
    {
        if (caret_y < n->scroll_y)
        {
            n->scroll_y = caret_y;
        }
        if (caret_y + view.line_h > n->scroll_y + view.clip.h)
        {
            n->scroll_y = caret_y + view.line_h - view.clip.h;
        }
        state->reveal_caret = FALSE;
    }
    textarea_clamp_scroll(n, &view);
    if (view.font == NULL_POINTER)
    {
        return;
    }
    gui_color_t color = n->resolved.has_font_color ? n->resolved.font_color : scene__rgb(1, 1, 1);
    color.a *= n->effective_opacity;
    gui_color_t highlight = n->resolved.accent_color;
    if (highlight.a <= 0)
    {
        highlight = scene__rgb(.30f, .47f, .70f);
    }
    highlight.a *= .4f * n->effective_opacity;
    int64 start = state->caret < state->anchor ? state->caret : state->anchor;
    int64 end = state->caret > state->anchor ? state->caret : state->anchor;
    textarea_line_t line = textarea_line(n, &view, 0);
    float y = view.clip.y - n->scroll_y;
    renderer__push_scissor(view.clip);
    for (;;)
    {
        if (y + view.line_h >= view.clip.y)
        {
            if (scene__focus() == n && start < line.next && end > line.start)
            {
                int64 a = start > line.start ? start : line.start;
                int64 b = end < line.end ? end : line.end;
                float left = textarea_prefix(n, &view, &line, a);
                float right = end > line.end ? view.clip.w : textarea_prefix(n, &view, &line, b);
                gui_rect_t selection =
                {
                    view.clip.x + left, y, stdlib__fmaxf(0, right - left), view.line_h
                };
                renderer__submit_rect(selection, highlight, 0);
            }
            font__draw_n_clipped(view.font, view.clip.x, y + view.ascent, color,
                n->text + line.start, line.end - line.start, view.clip);
        }
        if (!line.has_next || y + view.line_h > view.clip.y + view.clip.h)
        {
            break;
        }
        line = textarea_line(n, &view, line.next);
        y += view.line_h;
    }
    if (scene__focus() == n)
    {
        gui_rect_t caret =
        {
            view.clip.x + textarea_prefix(n, &view, &caret_line, state->caret),
            view.clip.y + caret_y - n->scroll_y, 2 * scale, view.line_h
        };
        // Keep the caret inside the scissor even at an exact-width hard wrap.
        caret.x = stdlib__fminf(caret.x, view.clip.x + view.clip.w - caret.w);
        renderer__submit_rect(caret, color, 0);
    }
    renderer__pop_scissor();
}

static void textarea_on_key(gui_node_t* n, int64 vk, boole down)
{
    if (!down)
    {
        return;
    }
    uint modifiers = scene__get_active_modifiers();
    boole ctrl = (modifiers & 4) && !(modifiers & 8);
    if ((!ctrl && (vk == 0x24 || vk == 0x23)) || vk == 0x26 || vk == 0x28)
    {
        scene_text_state_t* state = scene_text__state(n);
        if (state == NULL_POINTER)
        {
            return;
        }
        textarea_view_t view = textarea_view(n, scene__scale());
        textarea_line_t line = textarea_line(n, &view, 0);
        int64 row = 0;
        while (!textarea_contains(&line, state))
        {
            line = textarea_line(n, &view, line.next);
            row++;
        }
        if (vk == 0x24 || vk == 0x23)
        {
            state->caret = vk == 0x24 ? line.start : line.end;
            state->has_preferred_x = FALSE;
        }
        else
        {
            if (!state->has_preferred_x)
            {
                state->preferred_x = textarea_prefix(n, &view, &line, state->caret);
                state->has_preferred_x = TRUE;
            }
            line = textarea_row(n, &view, vk == 0x26 ? row - 1 : row + 1);
            state->caret = textarea_hit(n, &view, &line, state->preferred_x);
        }
        state->prefer_line_end = state->caret == line.end;
        state->reveal_caret = TRUE;
        if (!(modifiers & 1))
        {
            state->anchor = state->caret;
        }
        return;
    }
    scene_text__on_key(n, vk, down);
}

static void textarea_pointer(gui_node_t* n, int64 x, int64 y, boole extend)
{
    scene_text_state_t* state = scene_text__state(n);
    if (state == NULL_POINTER)
    {
        return;
    }
    textarea_view_t view = textarea_view(n, scene__scale());
    textarea_measure(n, &view, state, NULL_POINTER);
    if (extend && y < view.clip.y)
    {
        n->scroll_y -= view.clip.y - y;
    }
    else if (extend && y > view.clip.y + view.clip.h)
    {
        n->scroll_y += y - view.clip.y - view.clip.h;
    }
    textarea_clamp_scroll(n, &view);
    float position = (float)y - view.clip.y + n->scroll_y;
    int64 row = position > 0 ? (int64)(position / view.line_h) : 0;
    textarea_line_t line = textarea_row(n, &view, row);
    state->caret = textarea_hit(n, &view, &line, (float)x - view.clip.x);
    state->prefer_line_end = state->caret == line.end;
    state->has_preferred_x = FALSE;
    state->reveal_caret = TRUE;
    if (!extend)
    {
        state->anchor = state->caret;
    }
}

static void textarea_on_mouse_down(gui_node_t* n, int64 x, int64 y, int64 button)
{
    if (button == 0)
    {
        textarea_pointer(n, x, y, (scene__get_active_modifiers() & 1) != 0);
    }
}

static boole textarea_on_mouse_drag(gui_node_t* n, int64 x, int64 y)
{
    textarea_pointer(n, x, y, TRUE);
    return TRUE;
}

static boole textarea_on_mouse_wheel(gui_node_t* n, int64 x, int64 y, float delta)
{
    (void)x;
    (void)y;
    scene_text_state_t* state = scene_text__state(n);
    if (state == NULL_POINTER)
    {
        return FALSE;
    }
    textarea_view_t view = textarea_view(n, scene__scale());
    textarea_measure(n, &view, state, NULL_POINTER);
    float old = n->scroll_y;
    n->scroll_y -= delta * view.line_h * 3;
    textarea_clamp_scroll(n, &view);
    state->reveal_caret = FALSE;
    return old != n->scroll_y;
}

static void textarea_on_focus_gained(gui_node_t* n)
{
    scene_text_state_t* state = scene_text__state(n);
    if (state != NULL_POINTER)
    {
        state->reveal_caret = TRUE;
    }
}

static boole textarea_apply_attribute(gui_node_t* n, char* name, char* value)
{
    if (stdlib__strcmp(name, "placeholder") == 0 || stdlib__strcmp(name, "value") == 0)
    {
        return scene__set_text(n, value, (int64)stdlib__strlen(value));
    }
    return FALSE;
}

static const widget_vtable_t g_textarea_vtable =
{
    .type_name = "textarea",
    .apply_attribute = textarea_apply_attribute,
    .layout = textarea_layout,
    .emit_draws = textarea_emit_draws,
    .on_char = scene_text__on_char,
    .on_key = textarea_on_key,
    .on_mouse_down = textarea_on_mouse_down,
    .on_mouse_drag = textarea_on_mouse_drag,
    .on_mouse_wheel = textarea_on_mouse_wheel,
    .on_focus_gained = textarea_on_focus_gained,
    .state_size = sizeof(scene_text_state_t),
    .captures_drag = TRUE,
    .takes_focus = TRUE,
};

void widget_textarea__register(void)
{
    widget_registry__register(GUI_NODE_TYPE__TEXTAREA, &g_textarea_vtable);
}
