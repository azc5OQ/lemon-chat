// widget_text.c - static label (<label> in .ui, formerly <Text>).
// auto-sizes to its text's measured width + the line height (the
// font's own, or line-height from the style), plus its padding and
// border, unless style specifies explicit size_w / size_h. honors the
// avail_w passed in by the parent's layout and WORD-WRAPS when the
// text's natural width would exceed that bound; avail_w <= 0 means
// unconstrained (a single line at its natural width).
// <label text="Hello world" />
// layout pass:
// 1. pick the working font_size + family out of resolved style.
// 2. if size_w is pinned in style, use it as the wrap width AND as
// the final bounds.w. otherwise wrap against avail_w (or don't
// wrap at all when avail_w is 0 / the parent is happy with a
// wide single-line child).
// 3. walk the text greedily: accumulate xadvance until a space
// pushes us past the wrap width, then break at the last space.
// count lines, track max line width.
// 4. bounds.w = wrap_width (or max line width if no wrap needed)
// bounds.h = lines * line_height (or pinned size_h).
// draw pass:
// repeat the same walk, calling font__draw_n per line with the
// line's substring + the correct y baseline. pen_y advances by
// line_height between lines.

#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "third_party/log.h"
#include "renderer.h"
#include "font.h"
#include "clib/stdlib.h"

// decide whether a byte is a word separator for wrap purposes. CSS
// treats runs of whitespace as a single break point and collapses
// them on render; we do the same by accepting the end-of-line offset
// and then skipping trailing spaces before the next line starts.
// also counts newlines so a literal '\n' in text forces a break
// regardless of wrap width -- useful if user uses &#10; in XML text
// or pastes a multi-line string in programmatically.
static boole _widget_text_internal__is_break(char c)
{
    return (boole)(c == ' ' || c == '\t' || c == '\n');
}

// scan forward from `start` and return the byte offset at which the
// CURRENT line should end. greedy: fits as many characters as we can
// before the cumulative xadvance would exceed wrap_w, then breaks at
// the last break-character seen. if no break-character fits before
// the overflow, we return the overflow point itself (the single long
// word is allowed to overhang its own line -- matches CSS default
// overflow-wrap: normal). a forced break character ('\n') snaps us
// early regardless of width.
// decodes the UTF-8 sequence at text[i]: the codepoint goes to *out_cp,
// the sequence length comes back (at least 1, never past text_len).
// a malformed lead byte counts as one byte with its own value.
static int64 _widget_text_internal__utf8_at(const char* text, int64 text_len, int64 i, uint* out_cp)
{
    unsigned char lead = (unsigned char)text[i];
    int64         len  = 1;
    uint          cp   = lead;
    int64         k    = 0;

    if (lead >= 0xF0)
    {
        len = 4;
        cp  = lead & 0x07u;
    }
    else if (lead >= 0xE0)
    {
        len = 3;
        cp  = lead & 0x0Fu;
    }
    else if (lead >= 0xC0)
    {
        len = 2;
        cp  = lead & 0x1Fu;
    }

    if (i + len > text_len)
    {
        len = text_len - i;
    }

    for (k = 1; k < len; k++)
    {
        cp = (cp << 6) | ((unsigned char)text[i + k] & 0x3Fu);
    }

    *out_cp = cp;

    return len;
}

static int64 _widget_text_internal__find_line_end(gui_font_t* f, const char* text, int64        text_len, int64        start, float        wrap_w)
{
    if (start >= text_len)
    {
        return start;
    }
    // wrap_w <= 0 means "no wrap budget" -- caller wants a single line.
    // just scan for a literal '\n' or the end of the string.
    if (wrap_w <= 0.0f)
    {
        int64 i = start;
        while (i < text_len && text[i] != '\n')
        {
            i++;
        }
        return i;
    }

    int64 last_break = -1;
    float x          = 0.0f;
    int64 i          = start;
    uint  prev_cp    = 0;
    while (i < text_len)
    {
        char c = text[i];
        if (c == '\n')
        {
            // forced newline -- break here immediately.
            return i;
        }
        if (_widget_text_internal__is_break(c))
        {
            // remember this as a candidate wrap point. we still advance
            // past the space so a subsequent "width exceeded" check
            // counts the space toward the line budget.
            last_break = i;
        }

        // the running width is accumulated exactly as font__measure_n does
        // it, kerning included: the draw pass wraps against the width
        // layout measured, and a walk without kerning exceeded it by the
        // kerning of the line and broke at the last space (seen with
        // Roboto on every backend). one whole UTF-8 sequence per step, so
        // the kerning pair is the real codepoint pair.
        uint  cp  = 0;
        int64 len = _widget_text_internal__utf8_at(text, text_len, i, &cp);
        x += font__kern(f, prev_cp, cp) + font__measure_n(f, (char*)&text[i], len);
        prev_cp = cp;

        if (x > wrap_w && last_break >= 0)
        {
            // overflow and we have somewhere to break. split there.
            return last_break;
        }
        i += len;
    }
    // entire remaining tail fits. return the end-of-string offset.
    return i;
}

// skip any trailing break characters after a line's end so the next
// line doesn't start with the space that was consumed by the break.
static int64 _widget_text_internal__skip_breaks(const char* text, int64 text_len, int64 i)
{
    while (i < text_len && _widget_text_internal__is_break(text[i]) && text[i] != '\n')
    {
        i++;
    }
    if (i < text_len && text[i] == '\n')
    {
        i++;
    }
    return i;
}

static void text_layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale)
{
    (void)avail_h;
    gui_style_t* s = &n->resolved;

    // resolved font at scaled size. font_size 0 falls back to a 14 px
    // default (same logic as before the wrap rewrite).
    float     size_px = (s->font_size > 0.0f ? s->font_size : 14.0f) * scale;
    gui_font_t* f       = font__at(s->font_family, size_px);

    // if there's no font registered yet (e.g. Android: fonts stubbed
    // out), fall back to a pure sizing approximation so layout can
    // still make progress. uses the scaled font_size for height and
    // zero for width.
    if (f == NULL_POINTER)
    {
        n->bounds.x = x;
        n->bounds.y = y;
        n->bounds.w = (s->size_w > 0.0f) ? s->size_w * scale : 0.0f;
        n->bounds.h = (s->size_h > 0.0f) ? s->size_h * scale : size_px;
        return;
    }

    float line_h = font__line_height_for(f, s, scale);

    // padding and border belong to the label's box as in CSS: they
    // narrow the wrap width and widen / heighten the final bounds
    float bw    = scene__border_width(n, scale);
    float pad_x = (s->pad_l + s->pad_r) * scale + 2.0f * bw;
    float pad_y = (s->pad_t + s->pad_b) * scale + 2.0f * bw;

    // a pinned width: size / width in px, or width: N% of the space the
    // parent offers (the parent's content box, as in CSS)
    float pinned_w = 0.0f;
    if (s->size_w > 0.0f)
    {
        pinned_w = s->size_w * scale;
    }
    else if (s->width_pct > 0.0f && avail_w > 0.0f)
    {
        pinned_w = avail_w * s->width_pct / 100.0f;
    }

    // figure out wrap width. pin beats avail_w beats "no wrap".
    // when neither is set (pinned_w = 0 AND avail_w <= 0) we do a single
    // line and report the measured width -- matches pre-wrap behavior.
    float wrap_w = 0.0f;
    if (pinned_w > 0.0f)
    {
        wrap_w = pinned_w - pad_x;
    }
    else if (avail_w > 0.0f)
    {
        wrap_w = avail_w - pad_x;
    }
    // max-width narrows the wrap width too, as in CSS: the lines break
    // inside the box the clamp allows, so the drawn lines and the
    // measured height agree
    if (s->max_w > 0.0f && (wrap_w <= 0.0f || wrap_w > s->max_w * scale - pad_x))
    {
        wrap_w = s->max_w * scale - pad_x;
    }
    if (wrap_w < 0.0f)
    {
        wrap_w = 0.0f;
    }

    // Layout and draw use the same complete text and wrapping rules.
    int64 text_len = (int64)stdlib__strlen(n->text);

    int64 lines      = 0;
    float max_line_w = 0.0f;
    int64 cursor     = 0;
    while (cursor < text_len)
    {
        int64 end = _widget_text_internal__find_line_end(f, n->text, text_len, cursor, wrap_w);
        if (end <= cursor && !_widget_text_internal__is_break(n->text[cursor]))
        {
            // find_line_end refused to advance (shouldn't happen with
            // valid inputs, but guard so a bad text blob doesn't loop
            // forever). force one character of progress.
            uint cp = 0;
            end = cursor + _widget_text_internal__utf8_at(n->text, text_len, cursor, &cp);
        }
        float line_w = font__measure_n(f, &n->text[cursor], end - cursor);
        if (line_w > max_line_w)
        {
            max_line_w = line_w;
        }
        lines++;
        cursor = _widget_text_internal__skip_breaks(n->text, text_len, end);
    }
    // an empty label has no line box, as an empty element in HTML: its
    // height is its padding and border only (lines stays 0)

    // final sizing. size_w pinned wins; else max_line_w bounded by
    // wrap_w (we never return a bounds.w larger than the parent told
    // us we could have, to keep scissor / scroll math honest).
    float w;
    if (pinned_w > 0.0f)
    {
        w = pinned_w;
    }
    else if (wrap_w > 0.0f && max_line_w > wrap_w)
    {
        w = wrap_w + pad_x;
    }
    else
    {
        w = max_line_w + pad_x;
    }

    float h = (s->size_h > 0.0f) ? s->size_h * scale : (float)lines * line_h + pad_y;

    scene__apply_min_max(n, &w, &h, scale);

    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;
}

static void text_emit_draws(gui_node_t* n, float scale)
{
    // bg + border via the shared helper so text labels can carry
    // gradient / shadow / bg-image / blur / border-gradient the
    // same way containers do. No effect when no bg style is set.
    scene__emit_default_bg(n, scale);

    if (n->text[0] == 0)
    {
        return;
    }

    float     size_px = (n->resolved.font_size > 0.0f ? n->resolved.font_size : 14.0f) * scale;
    gui_font_t* f       = font__at(n->resolved.font_family, size_px);
    if (f == NULL_POINTER)
    {
        return;
    }

    float baseline = font__baseline_for(f, &n->resolved, scale);
    float line_h   = font__line_height_for(f, &n->resolved, scale);
    float bw       = scene__border_width(n, scale);
    float pad_x    = (n->resolved.pad_l + n->resolved.pad_r) * scale + 2.0f * bw;
    float origin_x = n->bounds.x + n->resolved.pad_l * scale + bw;
    float origin_y = n->bounds.y + n->resolved.pad_t * scale + bw;

    // re-derive wrap width (must match what text_layout used): the box
    // minus padding and border on both sides.
    float wrap_w = n->bounds.w - pad_x;
    if (wrap_w < 0.0f)
    {
        wrap_w = 0.0f;
    }

    int64 text_len = (int64)stdlib__strlen(n->text);

    gui_color_t color = n->resolved.has_font_color ? n->resolved.font_color : scene__rgb(1.0f, 1.0f, 1.0f);
    color.a *= n->effective_opacity;

    // walk lines, same algorithm as layout. emit each line's substring
    // at (bounds.x, bounds.y + baseline + line_index * line_h). pen y
    // is the baseline -- font__draw_n uses y as baseline; baseline is
    // the ascent plus the half-leading a line-height adds above the
    // glyphs, line_h the advance to the next line.
    int64 cursor     = 0;
    int64 line_index = 0;
    while (cursor < text_len)
    {
        int64 end = _widget_text_internal__find_line_end(f, n->text, text_len, cursor, wrap_w);
        if (end <= cursor && !_widget_text_internal__is_break(n->text[cursor]))
        {
            uint cp = 0;
            end = cursor + _widget_text_internal__utf8_at(n->text, text_len, cursor, &cp);
        }
        // text-align places each line inside the box's inner width
        float line_w = (n->resolved.text_align != GUI_TEXT_ALIGN__LEFT) ? font__measure_n(f, &n->text[cursor], end - cursor) : 0.0f;
        float tx     = origin_x;
        float ty     = origin_y + baseline + (float)line_index * line_h;
        if (n->resolved.text_align == GUI_TEXT_ALIGN__CENTER)
        {
            tx = origin_x + (wrap_w - line_w) * 0.5f;
        }
        else if (n->resolved.text_align == GUI_TEXT_ALIGN__RIGHT)
        {
            tx = origin_x + (wrap_w - line_w);
        }
        font__draw_n(f, tx, ty, color, &n->text[cursor], end - cursor);

        line_index++;
        cursor = _widget_text_internal__skip_breaks(n->text, text_len, end);
    }
}

static const widget_vtable_t g_text_vtable = {
    .type_name       = "label",
    .layout          = text_layout,
    .emit_draws      = text_emit_draws,
};

void widget_text__register(void)
{
    widget_registry__register(GUI_NODE_TYPE__TEXT, &g_text_vtable);
}
