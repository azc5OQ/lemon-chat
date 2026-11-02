// Single-line UTF-8 input with selection, caret navigation and clipboard shortcuts.
// Clipboard access stays in the shared clipboard service, not individual apps.

#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "scene_text.h"
#include "clib/memory_manager.h"
#include "renderer.h"
#include "font.h"
#include "third_party/log.h"
#include "clib/stdlib.h"

// inner text padding in pixels, relative to bounds.
#define _WIDGET_INPUT_INTERNAL__HPAD 8.0f

// U+2022 BULLET, three UTF-8 bytes, one of the atlas extras
#define _WIDGET_INPUT_INTERNAL__BULLET "\xE2\x80\xA2"

static void input_layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale)
{
    (void)avail_w;
    (void)avail_h;
    gui_style_t* s = &n->resolved;
    float w = (s->size_w > 0.0f) ? s->size_w * scale : 240.0f * scale;
    float h = (s->size_h > 0.0f) ? s->size_h * scale : 32.0f * scale;
    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;
}

static float input_prefix(gui_node_t* n, gui_font_t* font, int64 at)
{
    if (font == NULL_POINTER)
    {
        return 0;
    }
    if (n->input_password == FALSE)
    {
        return font__measure_n(font, n->text, at);
    }
    int64 count = 0;
    for (int64 i = 0; i < at; i++)
    {
        if (((ubyte)n->text[i] & 0xc0) != 0x80)
        {
            count++;
        }
    }
    return count * font__measure_n(font, _WIDGET_INPUT_INTERNAL__BULLET, 3);
}

static float input_origin(gui_node_t* n, gui_font_t* font, float scale, scene_text_state_t* state)
{
    float padding = _WIDGET_INPUT_INTERNAL__HPAD * scale;
    float available = stdlib__fmaxf(0, n->bounds.w - 2 * padding);
    float width = input_prefix(n, font, n->text_len);
    float caret = input_prefix(n, font, state->caret);
    float x = n->bounds.x + padding;
    if (width <= available)
    {
        state->scroll_x = 0;
        if (n->resolved.halign == GUI_HALIGN__CENTER)
        {
            x += (available - width) * .5f;
        }
        else if (n->resolved.halign == GUI_HALIGN__RIGHT)
        {
            x += available - width;
        }
    }
    else
    {
        if (caret < state->scroll_x)
        {
            state->scroll_x = caret;
        }
        if (caret > state->scroll_x + available - 2 * scale)
        {
            state->scroll_x = caret - available + 2 * scale;
        }
        state->scroll_x = stdlib__fmaxf(0, state->scroll_x);
    }
    return x - state->scroll_x;
}

static char* input_password_mask(gui_node_t* n, scene_text_state_t* state, char* small, int64* length)
{
    int64 characters = 0;
    for (int64 i = 0; i < n->text_len; i = scene_text__next(n, i))
    {
        characters++;
    }
    *length = characters * 3;
    char* mask = small;
    if (characters > 256)
    {
        if (state->mask_capacity <= *length)
        {
            int64 capacity = state->mask_capacity > 0 ? state->mask_capacity : 1024;
            while (capacity <= *length)
            {
                capacity = capacity > (n->text_limit * 3 + 1) / 2 ? n->text_limit * 3 + 1 : capacity * 2;
            }
            char* grown = GUI_MALLOC((size_t)capacity);
            if (grown == NULL_POINTER)
            {
                *length = 0;
                return small;
            }
            GUI_FREE(state->mask);
            state->mask = grown;
            state->mask_capacity = capacity;
            state->mask_characters = -1;
        }
        mask = state->mask;
        if (state->mask_characters == characters)
        {
            return mask;
        }
        state->mask_characters = characters;
    }
    for (int64 i = 0; i < characters; i++)
    {
        stdlib__memcpy(mask + i * 3, _WIDGET_INPUT_INTERNAL__BULLET, 3);
    }
    mask[*length] = 0;
    return mask;
}

static void input_on_destroy(gui_node_t* n)
{
    scene_text_state_t* state = n->user_data;
    if (state != NULL_POINTER)
    {
        GUI_FREE(state->mask);
    }
}

static void input_emit_draws(gui_node_t* n, float scale)
{
    scene__emit_default_bg(n, scale);
    scene_text_state_t* state = scene_text__state(n);
    if (state == NULL_POINTER)
    {
        return;
    }
    float size = (n->resolved.font_size > 0 ? n->resolved.font_size : 14) * scale;
    gui_font_t* font = font__at(n->resolved.font_family, size);
    float line_h = font != NULL_POINTER ? font__line_height_for(font, &n->resolved, scale) : size;
    float ascent = font != NULL_POINTER ? font__baseline_for(font, &n->resolved, scale) : 0;
    float y = n->bounds.y + (n->bounds.h - line_h) * .5f;
    float x = input_origin(n, font, scale, state);
    gui_color_t color = n->resolved.has_font_color ? n->resolved.font_color : scene__rgb(1, 1, 1);
    gui_rect_t clip = n->bounds;
    clip.x += 3 * scale;
    clip.w = stdlib__fmaxf(0, clip.w - 6 * scale);
    renderer__push_scissor(clip);
    if (scene__focus() == n && state->caret != state->anchor)
    {
        float a = input_prefix(n, font, state->caret);
        float b = input_prefix(n, font, state->anchor);
        gui_rect_t selection = { x + stdlib__fminf(a, b), y, stdlib__fabsf(a - b), line_h };
        gui_color_t highlight = n->resolved.accent_color;
        if (highlight.a <= 0)
        {
            highlight = scene__rgb(.30f, .47f, .70f);
        }
        highlight.a *= .4f;
        renderer__submit_rect(selection, highlight, 0);
    }
    if (font != NULL_POINTER && n->text_len > 0)
    {
        if (n->input_password)
        {
            // The cache contains bullets only. Stable long passwords do not
            // allocate each frame, and drawing keeps the normal font rounding.
            char small[256 * 3 + 1];
            int64 length = 0;
            char* mask = input_password_mask(n, state, small, &length);
            font__draw_n_clipped(font, x, y + ascent, color, mask, length, clip);
        }
        else
        {
            font__draw_n_clipped(font, x, y + ascent, color, n->text, n->text_len, clip);
        }
    }
    if (scene__focus() == n)
    {
        gui_rect_t caret = { x + input_prefix(n, font, state->caret), y, 2 * scale, line_h };
        renderer__submit_rect(caret, color, 0);
    }
    renderer__pop_scissor();
}

static void input_pointer(gui_node_t* n, int64 x, boole extend)
{
    scene_text_state_t* state = scene_text__state(n);
    if (state == NULL_POINTER)
    {
        return;
    }
    float scale = scene__scale();
    gui_font_t* font = font__at(n->resolved.font_family, (n->resolved.font_size > 0 ? n->resolved.font_size : 14) * scale);
    float origin = input_origin(n, font, scale, state);
    int64 at = 0;
    if (n->input_password)
    {
        float advance = font__measure_n(font, _WIDGET_INPUT_INTERNAL__BULLET, 3);
        float position = 0;
        while (at < n->text_len && (float)x - origin >= position + advance * .5f)
        {
            position += advance;
            at = scene_text__next(n, at);
        }
    }
    else
    {
        at = font != NULL_POINTER ? font__text_position(font, n->text, n->text_len, (float)x - origin) : n->text_len;
    }
    state->caret = at;
    if (!extend)
    {
        state->anchor = at;
    }
}

static void input_on_mouse_down(gui_node_t* n, int64 x, int64 y, int64 button)
{
    (void)y;
    if (button == 0)
    {
        input_pointer(n, x, (scene__get_active_modifiers() & 1) != 0);
    }
}

static boole input_on_mouse_drag(gui_node_t* n, int64 x, int64 y)
{
    (void)y;
    input_pointer(n, x, TRUE);
    return TRUE;
}

// `placeholder=` is an HTML-muscle-memory alias for `text=` on
// empty inputs. Copies into node->text the same way the generic
// `text=` attribute would, so stylesheet/parser code downstream
// doesn't need to know there's a difference. Authors can still
// write `text=` if they prefer.
static boole input_apply_attribute(gui_node_t* n, char* name, char* value)
{
    gui_node_type_e target = GUI_NODE_TYPE__INPUT;
    const widget_vtable_t* target_vt = NULL_POINTER;

    if (stdlib__strcmp(name, "type") == 0)
    {
        // the HTML input types. text and password stay an input; the
        // others re-type the node into the widget that implements them,
        // so <input type="checkbox"> is a <checkbox>. Write `type` first
        // in the tag: the attributes after it belong to the new widget.
        if (stdlib__strcmp(value, "password") == 0)
        {
            n->input_password = TRUE;
            return TRUE;
        }
        if (stdlib__strcmp(value, "text") == 0 || stdlib__strcmp(value, "search") == 0 || stdlib__strcmp(value, "email") == 0 || stdlib__strcmp(value, "url") == 0 || stdlib__strcmp(value, "tel") == 0 || stdlib__strcmp(value, "number") == 0)
        {
            n->input_password = FALSE;
            return TRUE;
        }
        if (stdlib__strcmp(value, "checkbox") == 0)
        {
            target = GUI_NODE_TYPE__CHECKBOX;
        }
        else if (stdlib__strcmp(value, "radio") == 0)
        {
            target = GUI_NODE_TYPE__RADIO;
        }
        else if (stdlib__strcmp(value, "range") == 0)
        {
            target = GUI_NODE_TYPE__SLIDER;
        }
        else if (stdlib__strcmp(value, "color") == 0)
        {
            target = GUI_NODE_TYPE__COLOR_PICKER;
        }
        else if (stdlib__strcmp(value, "date") == 0)
        {
            target = GUI_NODE_TYPE__DATEPICKER;
        }
        else if (stdlib__strcmp(value, "file") == 0)
        {
            target = GUI_NODE_TYPE__FILEPICKER;
        }
        else if (stdlib__strcmp(value, "button") == 0 || stdlib__strcmp(value, "submit") == 0 || stdlib__strcmp(value, "reset") == 0)
        {
            target = GUI_NODE_TYPE__BUTTON;
        }
        else
        {
            log_warn("%s %s %s", "input: type '", value, "' (text|password|checkbox|radio|range|color|date|file|button)");
            return TRUE;
        }

        n->type = target;
        target_vt = widget_registry__get(target);
        if (target_vt != NULL_POINTER && target_vt->init_defaults != NULL_POINTER)
        {
            target_vt->init_defaults(n);
        }
        return TRUE;
    }

    // `value=` is the HTML initial text; `placeholder=` is an alias for
    // it here (the widget has no separate hint). Both copy into
    // node->text the way the generic `text=` attribute would.
    if (stdlib__strcmp(name, "placeholder") == 0 || stdlib__strcmp(name, "value") == 0)
    {
        return scene__set_text(n, value, (int64)stdlib__strlen(value));
    }

    return FALSE;
}

static const widget_vtable_t g_input_vtable =
{
    .type_name = "input",
    .on_destroy = input_on_destroy,
    .apply_attribute = input_apply_attribute,
    .layout = input_layout,
    .emit_draws = input_emit_draws,
    .on_mouse_down = input_on_mouse_down,
    .on_mouse_drag = input_on_mouse_drag,
    .captures_drag = TRUE,
    .state_size = sizeof(scene_text_state_t),
    .on_char = scene_text__on_char,
    .on_key = scene_text__on_key,
    .takes_focus = TRUE,
};

void widget_input__register(void)
{
    widget_registry__register(GUI_NODE_TYPE__INPUT, &g_input_vtable);
}
