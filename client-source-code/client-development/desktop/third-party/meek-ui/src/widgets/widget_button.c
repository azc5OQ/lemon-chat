// widget_button.c - clickable rectangle with optional centered text.
// the box is sized as an HTML button: the label's width and line
// height plus padding and border, unless the style pins a size or a
// percent width / height. default click semantics: scene's input state
// machine routes mouse-up-on-same-node-as-mouse-down to the on_click
// handler. if node->text is non-empty, the button also draws the text
// centered in its content box using the font specified in the resolved
// style (font_family + font_size).

#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "renderer.h"
#include "font.h"
#include "clib/stdlib.h"

static void button_layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale)
{
    gui_style_t* s       = &n->resolved;
    float      size_px = (s->font_size > 0.0f ? s->font_size : 14.0f) * scale;
    gui_font_t*  f       = font__at(s->font_family, size_px);
    float      bw      = scene__border_width(n, scale);
    float      text_w  = (f != NULL_POINTER && n->text[0] != 0) ? font__measure(f, n->text) : 0.0f;
    float      line_h  = (f != NULL_POINTER) ? font__line_height_for(f, s, scale) : size_px;
    float      w       = 0.0f;
    float      h       = 0.0f;

    if (avail_w < 0.0f)
    {
        avail_w = 0.0f;
    }

    if (avail_h < 0.0f)
    {
        avail_h = 0.0f;
    }

    // explicit pixel size, then percent of the parent, then the label
    // plus padding and border, the way an HTML button sizes itself
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
        w = text_w + (s->pad_l + s->pad_r) * scale + 2.0f * bw;
    }

    if (s->size_h > 0.0f)
    {
        h = s->size_h * scale;
    }
    else if (s->height_pct > 0.0f)
    {
        h = avail_h * (s->height_pct / 100.0f);
    }
    else
    {
        h = line_h + (s->pad_t + s->pad_b) * scale + 2.0f * bw;
    }

    scene__apply_min_max(n, &w, &h, scale);

    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;
}

// centered text: measure width, position so left = bounds.x + (w-tw)/2;
// y position uses ascent so the text's visual baseline sits near the
// vertical center. font_size is multiplied by scale so text grows with
// the scale slider.
static void button_emit_draws(gui_node_t* n, float scale)
{
    // Route bg + border through the shared helper so buttons get
    // background-gradient, box-shadow, bg-image, blur, and border-
    // gradient for free (same bundle the container widgets use).
    // Previously this function hand-rolled `if (has_bg_color)
    // submit_rect` which silently ignored every other bg property.
    scene__emit_default_bg(n, scale);

    if (n->text_len <= 0 && n->text[0] == 0)
    {
        return;
    }

    // request the font at (family, scaled size). font_size of 0 falls
    // back to a default inside font__at.
    float     size_px = n->resolved.font_size > 0.0f ? n->resolved.font_size : 14.0f;
    size_px *= scale;
    gui_font_t* f = font__at(n->resolved.font_family, size_px);
    if (f == NULL_POINTER)
    {
        return; // no font registered.
    }

    float text_w   = font__measure(f, n->text);
    float baseline = font__baseline_for(f, &n->resolved, scale);
    float line_h   = font__line_height_for(f, &n->resolved, scale);
    float bw       = scene__border_width(n, scale);
    float cx       = n->bounds.x + n->resolved.pad_l * scale + bw;
    float cy       = n->bounds.y + n->resolved.pad_t * scale + bw;
    float cw       = n->bounds.w - (n->resolved.pad_l + n->resolved.pad_r) * scale - 2.0f * bw;
    float ch       = n->bounds.h - (n->resolved.pad_t + n->resolved.pad_b) * scale - 2.0f * bw;

    // center the label in the content box (inside padding and border);
    // the baseline sits where the line box puts it (ascent plus
    // half-leading), which is where a browser centers a button's label.
    float tx = cx + (cw - text_w) * 0.5f;
    float ty = cy + (ch - line_h) * 0.5f + baseline;

    gui_color_t color = n->resolved.has_font_color ? n->resolved.font_color : scene__rgb(1.0f, 1.0f, 1.0f);
    font__draw(f, tx, ty, color, n->text);
}

// `value=` is the caption of an HTML <input type="button">; the same
// slot as `text=`.
static boole button_apply_attribute(gui_node_t* n, char* name, char* value)
{
    if (stdlib__strcmp(name, "value") != 0)
    {
        return FALSE;
    }

    return scene__set_text(n, value, (int64)stdlib__strlen(value));
}

static const widget_vtable_t g_button_vtable = {
    .type_name        = "button",
    .init_defaults    = NULL_POINTER,
    .apply_attribute  = button_apply_attribute,
    .layout           = button_layout,
    .emit_draws       = button_emit_draws,
    .on_mouse_down    = NULL_POINTER,
    .on_mouse_up      = NULL_POINTER,
    .on_mouse_drag    = NULL_POINTER,
    .on_char          = NULL_POINTER,
    .on_key           = NULL_POINTER,
    .consumes_click   = FALSE,
    .takes_focus      = FALSE,
};

void widget_button__register(void)
{
    widget_registry__register(GUI_NODE_TYPE__BUTTON, &g_button_vtable);
}
