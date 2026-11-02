// scene_render.c -- draw emission split out of scene.c. Owns:
// * scene__submit_fitted_image   (public helper for fit modes)
// * scene__border_width          (public helper to query border width)
// * scene__emit_border           (public helper)
// * scene__emit_default_bg       (public helper widgets call)
// * recursive tree walk with z-index sort + opacity cascade
// scene__emit_draws itself stays in scene.c (it drives root + overlay).
// Widgets call scene__emit_default_bg / scene__emit_border from their
// own vtable emit_draws; the recursive walker here calls vt->emit_draws
// for each node that has one, or scene__emit_default_bg otherwise.

#include <stdlib.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "renderer.h"
#include "border.h"
#include "scene.h"
#include "scene_render.h"
#include "font.h"
#include "compositor.h"
#include "scroll.h"
#include "clib/stdlib.h"
#include "third_party/log.h"
#include "widgets/widget_image_cache.h"

// Default bg emission. Honors box-shadow (behind bg), bg-gradient
// (painted over the color, as CSS layers background-image over
// background-color), and opacity (multiplies every alpha).
static void _scene_render_internal__emit_default(gui_node_t* n, float scale)
{
    scene__emit_default_bg(n, scale);
}

// UV-aware variant. Used by widget_image when an icon lives in
// an atlas page (one big GL texture, many small rects, addressed
// by sub-rect UVs). The legacy submit_fitted_image below is a
// thin wrapper that calls this with (0,0,1,1) for full-texture
// sampling.
/**
 * @brief uV-aware variant of submit_fitted_image. Same fit logic + placeholder gradient on tex==NULL, but the texture is sampled using the explicit (u0,v0,u1,v1) sub-rect instead of the full [0,1]^2 region. Used by atlased images: many widget_image instances share one big atlas texture, each addressing a different sub-rect.
 *
 * @param gui_rect_t bounds -> Destination rect.
 * @param void* tex -> Texture handle.
 * @param int natural_w -> , natural_h - Image's natural pixel size (for CONTAIN/COVER fit math).
 * @param int fit -> Fit mode (GUI_IMAGE_FIT__FILL/CONTAIN/COVER/NONE).
 * @param gui_color_t tint -> RGBA multiplier.
 * @param float radius -> Corner radius for placeholder fallback.
 * @param float u0 -> ,v0,u1,v1 Source UV in [0,1].
 *
 * @return void
 */
void scene__submit_fitted_image_uv(gui_rect_t bounds, void* tex, int natural_w, int natural_h, int fit, gui_color_t tint, float radius, float u0, float v0, float u1, float v1)
{
    if (tex == NULL_POINTER)
    {
        gui_color_t a; a.r = 0.20f; a.g = 0.22f; a.b = 0.27f; a.a = tint.a;
        gui_color_t b; b.r = 0.10f; b.g = 0.11f; b.b = 0.14f; b.a = tint.a;
        renderer__submit_rect_gradient(bounds, a, b, (int)GUI_GRADIENT_DIR__DIAGONAL_TL, radius);
        return;
    }

    gui_rect_t dest = bounds;
    boole   need_scissor = FALSE;

    if (natural_w > 0 && natural_h > 0 && bounds.w > 0.0f && bounds.h > 0.0f)
    {
        float nw = (float)natural_w;
        float nh = (float)natural_h;
        if (fit == GUI_IMAGE_FIT__CONTAIN)
        {
            float sx = bounds.w / nw;
            float sy = bounds.h / nh;
            float s  = (sx < sy) ? sx : sy;
            float dw = nw * s;
            float dh = nh * s;
            dest.x = bounds.x + (bounds.w - dw) * 0.5f;
            dest.y = bounds.y + (bounds.h - dh) * 0.5f;
            dest.w = dw;
            dest.h = dh;
        }
        else if (fit == GUI_IMAGE_FIT__COVER)
        {
            float sx = bounds.w / nw;
            float sy = bounds.h / nh;
            float s  = (sx > sy) ? sx : sy;
            float dw = nw * s;
            float dh = nh * s;
            dest.x = bounds.x + (bounds.w - dw) * 0.5f;
            dest.y = bounds.y + (bounds.h - dh) * 0.5f;
            dest.w = dw;
            dest.h = dh;
            need_scissor = TRUE;
        }
        else if (fit == GUI_IMAGE_FIT__NONE)
        {
            dest.x = bounds.x + (bounds.w - nw) * 0.5f;
            dest.y = bounds.y + (bounds.h - nh) * 0.5f;
            dest.w = nw;
            dest.h = nh;
            need_scissor = (boole)(nw > bounds.w || nh > bounds.h);
        }
    }

    if (need_scissor)
    {
        renderer__push_scissor(bounds);
    }
    renderer__submit_image_uv(dest, tex, tint, u0, v0, u1, v1);
    if (need_scissor)
    {
        renderer__pop_scissor();
    }
    (void)radius;
}

/**
 * @brief submit a textured image rect fitted into `bounds` per the gui_image_fit_e mode. Shared between <image>'s own emit_draws and any container's background-image rendering so the fit math lives in one place. FILL     stretch across `bounds` (breaks aspect ratio). CONTAIN  uniform scale to fit inside, centered, letterboxed. COVER    uniform scale to cover, centered; excess is scissored. NONE     natural pixel size, centered; scissored if bigger. `tex` may be NULL (renderer backend without an image pipeline); this function falls back to a muted gradient placeholder so the slot is at least visible.
 *
 * @param gui_rect_t bounds -> Destination rect in pixels.
 * @param void* tex -> Texture from renderer__create_texture_rgba / widget_image__cache_get_or_load. NULL = placeholder.
 * @param int natural_w -> Image natural width in pixels.
 * @param int natural_h -> Image natural height in pixels.
 * @param int fit -> gui_image_fit_e value (cast to int for ABI stability).
 * @param gui_color_t tint -> Multiplied into sampled RGBA. Pass (1,1,1,1) for "draw as-is".
 * @param float radius -> Corner radius for the placeholder rect. Ignored for the textured path.
 *
 * @return void
 */
void scene__submit_fitted_image(gui_rect_t bounds, void* tex, int natural_w, int natural_h, int fit, gui_color_t tint, float radius)
{
    scene__submit_fitted_image_uv(bounds, tex, natural_w, natural_h, fit, tint, radius, 0.0f, 0.0f, 1.0f, 1.0f);
}

/**
 * @brief effective scaled border thickness for a node, or 0.0f when the node has no visible border. Encodes the same three-way gate as scene__emit_border (border_width > 0, has_border_color set, border_style != NONE) so layout code can ask "how much room does the border eat?" without re-implementing the rule. Returns `border_width * scale` when the gate passes. Container widgets (window/div/column/row/collection) call this to shrink their inner content area and shift the child origin so children sit INSIDE the border line, not under it.
 *
 * @param gui_node_t* n -> Node whose resolved border style to read.
 * @param float scale -> Global UI scale (pass scene__scale()).
 *
 * @return float -> Pixel border width, or 0.0f when no border applies.
 */
float scene__border_width(gui_node_t* n, float scale)
{
    if (n == NULL_POINTER)
    {
        return 0.0f;
    }
    gui_style_t* s = &n->resolved;
    if (s->border_width <= 0.0f)
    {
        return 0.0f;
    }
    if (s->border_style == GUI_BORDER_STYLE__NONE || s->border_style == GUI_BORDER_STYLE__HIDDEN)
    {
        return 0.0f;
    }
    if (s->has_border_color == FALSE)
    {
        return 0.0f;
    }
    float bw = s->border_width * scale;
    if (bw < 0.0f)
    {
        bw = 0.0f;
    }
    // whole device pixels, at least one, as a browser snaps borders: a
    // 1 px border at 125 % is one device pixel, not one and a quarter,
    // which keeps a page of bordered blocks from drifting away from
    // the browser's layout by a fraction of a pixel per block
    if (bw > 0.0f)
    {
        bw = (float)(int64)(bw + 0.5f);
        if (bw < 1.0f)
        {
            bw = 1.0f;
        }
    }
    return bw;
}

/**
 * @brief paints the node border without filling or erasing the center
 * @param gui_node_t* n -> resolved border style and inherited opacity
 * @param gui_rect_t bounds -> outer border bounds
 * @param float scale -> logical-to-device scale
 * @return void
 */
void scene__emit_border(gui_node_t* n, gui_rect_t bounds, float scale)
{
    gui_style_t* style = NULL_POINTER;

    if (n == NULL_POINTER)
    {
        return;
    }
    style = &n->resolved;
    if (!style->has_border_color)
    {
        return;
    }
    border__draw(bounds, style->radius * scale, scene__border_width(n, scale), style->border_style,
        style->border_color, style->has_border_gradient ? &style->border_gradient : NULL_POINTER, scale, n->effective_opacity);
}

/**
 * @brief emit the default "bg rect + optional shadow + optional gradient" triple for a node. Widgets whose emit_draws overrides still want this common visual treatment can call this instead of duplicating the logic. Honors the node's effective_opacity so every submitted color alpha is multiplied by the accumulated parent-chain opacity.
 *
 * @param gui_node_t* n -> Node whose background decorations to emit.
 * @param float scale -> Global UI scale (pass scene__scale()).
 *
 * @return void
 */
void scene__emit_default_bg(gui_node_t* n, float scale)
{
    gui_style_t* s = &n->resolved;
    float r  = s->radius * scale;
    float op = n->effective_opacity;
    boole extended_background = FALSE;

    if (s->has_shadow && s->shadow_color.a > 0.0f)
    {
        gui_rect_t sh;
        sh.x = n->bounds.x + s->shadow_dx;
        sh.y = n->bounds.y + s->shadow_dy;
        sh.w = n->bounds.w;
        sh.h = n->bounds.h;
        gui_color_t sc = s->shadow_color;
        sc.a *= op;
        renderer__submit_shadow(sh, sc, r, s->shadow_blur);
    }

    // CSS layering: the background color is the bottom layer, the
    // gradient is the image layer painted over it
    extended_background = compositor__background(n, scale);
    if (extended_background == FALSE && s->has_background_color)
    {
        gui_color_t c = s->background_color;
        c.a *= op;
        renderer__submit_rect(n->bounds, c, r);
    }

    if (extended_background == FALSE && s->has_bg_gradient)
    {
        renderer__submit_gradient(n->bounds, &s->bg_gradient, scale, op, r);
    }

    scene__emit_border(n, n->bounds, scale);

    if (extended_background == FALSE && s->has_background_image && s->background_image[0] != 0)
    {
        int nw = 0, nh = 0;
        void* tex = widget_image__cache_get_or_load(s->background_image, &nw, &nh);
        gui_color_t tint;
        tint.r = 1.0f; tint.g = 1.0f; tint.b = 1.0f; tint.a = op;
        scene__submit_fitted_image(n->bounds, tex, nw, nh, (int)s->background_size, tint, r);
        // Flush after a bg-image is queued so anything submitted
        // by this node's CHILDREN (rects, text, more images) lands
        // in fresh batches that draw on TOP of the wallpaper. The
        // renderer's flush order is rect -> image -> text, so
        // without this flush the children's rects would be drawn
        // BEFORE the wallpaper image and get covered by it -- the
        // exact bug the lock-screen slider track hit (wallpaper
        // erasing the white pill).
        renderer__flush_pending_draws();
    }

    if (s->blur_px > 0.0f && op > 0.0f)
    {
        renderer__blur_region(n->bounds, s->blur_px);
    }
}

// Sibling-only paint-order sort (z-index, then positioned above
// static). Gathers children into a scratch array
// (stable insertion sort keeps original order for equal z_index),
// walks in the sorted order. Bounded by MAX_Z_SORT; deeper sibling
// counts fall back to unsorted.
#define _SCENE_RENDER_INTERNAL__MAX_Z_SORT 256

static void _scene_render_internal__emit_recursive(gui_node_t* n, float scale);
static boole _scene_render_internal__clips_children(gui_node_t* n);
static boole _scene_render_internal__outside_clip(gui_node_t* n);

// The clip every draw of the current subtree is limited to, in device
// pixels: the viewport at the root, narrowed by each ancestor that
// clips its children (the same overflow test widget_div and
// widget_window use before they push a scissor). It is only ever as
// narrow as the real scissor, never narrower, so culling against it
// cannot drop a visible node.
static gui_rect_t _scene_render_internal__clip;
static boole      _scene_render_internal__clip_valid = FALSE;

// whether a container narrows its children's clip: overflow hidden or
// scroll always, auto only when a scrollbar is showing (mirrors
// _widget_div_internal__needs_clip / _widget_window_internal__needs_clip)
static boole _scene_render_internal__clips_children(gui_node_t* n)
{
    gui_overflow_e ox = n->resolved.overflow_x;
    gui_overflow_e oy = n->resolved.overflow_y;

    if (n->type != GUI_NODE_TYPE__DIV && n->type != GUI_NODE_TYPE__WINDOW)
    {
        return FALSE;
    }

    if (ox == GUI_OVERFLOW__HIDDEN || ox == GUI_OVERFLOW__SCROLL || oy == GUI_OVERFLOW__HIDDEN || oy == GUI_OVERFLOW__SCROLL)
    {
        return TRUE;
    }

    if (ox == GUI_OVERFLOW__AUTO || oy == GUI_OVERFLOW__AUTO)
    {
        return (boole)(scroll__vbar_visible(n) || scroll__hbar_visible(n));
    }

    return FALSE;
}

// TRUE when nothing this node or its descendants draw can reach the
// current clip: the subtree's paint bounds (computed after layout, with
// the effect margin) do not intersect it, and no transform in the
// subtree could move pixels elsewhere
static boole _scene_render_internal__outside_clip(gui_node_t* n)
{
    if (_scene_render_internal__clip_valid == FALSE || n->paint_bounded == FALSE || n->parent == NULL_POINTER)
    {
        return FALSE;
    }

    if (n->paint_bounds.x + n->paint_bounds.w <= _scene_render_internal__clip.x || n->paint_bounds.y + n->paint_bounds.h <= _scene_render_internal__clip.y)
    {
        return TRUE;
    }

    if (n->paint_bounds.x >= _scene_render_internal__clip.x + _scene_render_internal__clip.w || n->paint_bounds.y >= _scene_render_internal__clip.y + _scene_render_internal__clip.h)
    {
        return TRUE;
    }

    return FALSE;
}

/**
 * @brief paint-order key of a node among its siblings: z-index first, then positioned elements above static ones, which is the CSS stacking order for the common case
 *
 * @param gui_node_t* n -> the node
 *
 * @return int64 -> z_index times two, plus one when the node is positioned (relative, absolute, fixed or sticky)
 */
int64 scene_render__paint_key(gui_node_t* n)
{
    int64 key = (int64)n->resolved.z_index * 2;

    if (n->resolved.position != GUI_POSITION__STATIC)
    {
        key = key + 1;
    }

    return key;
}

static void _scene_render_internal__emit_children_sorted(gui_node_t* parent, float scale)
{
    int64 count = 0;
    for (gui_node_t* c = parent->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        count++;
    }
    if (count <= 1 || count > _SCENE_RENDER_INTERNAL__MAX_Z_SORT)
    {
        for (gui_node_t* c = parent->first_child; c != NULL_POINTER; c = c->next_sibling)
        {
            _scene_render_internal__emit_recursive(c, scale);
        }
        return;
    }

    boole any_z = FALSE;
    for (gui_node_t* c = parent->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        if (scene_render__paint_key(c) != 0)
        {
            any_z = TRUE;
            break;
        }
    }
    if (any_z == FALSE)
    {
        for (gui_node_t* c = parent->first_child; c != NULL_POINTER; c = c->next_sibling)
        {
            _scene_render_internal__emit_recursive(c, scale);
        }
        return;
    }

    gui_node_t* slots[_SCENE_RENDER_INTERNAL__MAX_Z_SORT];
    int64 n = 0;
    for (gui_node_t* c = parent->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        slots[n++] = c;
    }

    for (int64 i = 1; i < n; i++)
    {
        gui_node_t* key = slots[i];
        int64 j = i - 1;
        while (j >= 0 && scene_render__paint_key(slots[j]) > scene_render__paint_key(key))
        {
            slots[j + 1] = slots[j];
            j--;
        }
        slots[j + 1] = key;
    }
    for (int64 i = 0; i < n; i++)
    {
        // Z-boundary flush. The renderer batches text into a separate
        // late pass (text-on-top-of-rects within a flush). If we let
        // a whole frame accumulate, a lower-z sibling's text would
        // paint on top of a higher-z sibling's rects, which is the
        // opposite of what the z-index requested. Flushing at every
        // boundary where z_index actually changes keeps each z layer
        // self-contained: rects + their own text, then the next layer
        // paints on top of the composited earlier layer.
        if (i > 0 && scene_render__paint_key(slots[i]) != scene_render__paint_key(slots[i - 1]))
        {
            renderer__flush_pending_draws();
        }
        _scene_render_internal__emit_recursive(slots[i], scale);
    }
}

static void _scene_render_internal__emit_recursive(gui_node_t* n, float scale)
{
    gui_node_t* previous_font_node = NULL_POINTER;
    gui_rect_t  saved_clip;
    boole       saved_clip_valid = FALSE;
    boole       narrows          = FALSE;
    boole       effect_group     = FALSE;
    float       right            = 0.0f;
    float       bottom           = 0.0f;

    stdlib__null_memory(&saved_clip, sizeof(saved_clip));

    if (n == NULL_POINTER)
    {
        return;
    }
    if (n->resolved.display == GUI_DISPLAY__NONE)
    {
        return;
    }
    // a subtree that cannot touch the clip is not walked at all: no
    // effect group, no style queries, no text wrapping, no glyphs
    if (_scene_render_internal__outside_clip(n) == TRUE)
    {
        return;
    }
    saved_clip       = _scene_render_internal__clip;
    saved_clip_valid = _scene_render_internal__clip_valid;
    narrows          = _scene_render_internal__clips_children(n);
    if (narrows == TRUE)
    {
        if (_scene_render_internal__clip_valid == TRUE)
        {
            right  = _scene_render_internal__clip.x + _scene_render_internal__clip.w;
            bottom = _scene_render_internal__clip.y + _scene_render_internal__clip.h;
            if (n->bounds.x > _scene_render_internal__clip.x)
            {
                _scene_render_internal__clip.x = n->bounds.x;
            }
            if (n->bounds.y > _scene_render_internal__clip.y)
            {
                _scene_render_internal__clip.y = n->bounds.y;
            }
            if (n->bounds.x + n->bounds.w < right)
            {
                right = n->bounds.x + n->bounds.w;
            }
            if (n->bounds.y + n->bounds.h < bottom)
            {
                bottom = n->bounds.y + n->bounds.h;
            }
            _scene_render_internal__clip.w = right - _scene_render_internal__clip.x;
            _scene_render_internal__clip.h = bottom - _scene_render_internal__clip.y;
        }
        else
        {
            _scene_render_internal__clip       = n->bounds;
            _scene_render_internal__clip_valid = TRUE;
        }
    }

    float parent_op = 1.0f;
    if (n->parent != NULL_POINTER)
    {
        parent_op = n->parent->effective_opacity;
    }
    float own_op = 1.0f;
    if (n->resolved.has_opacity)
    {
        own_op = n->resolved.opacity;
        if (own_op < 0.0f)
        {
            own_op = 0.0f;
        }
        if (own_op > 1.0f)
        {
            own_op = 1.0f;
        }
    }
    n->effective_opacity = parent_op * own_op * ((n->fade_alpha > 0.0f) ? n->fade_alpha : 0.0f) * (n->navigation_effect ? n->navigation_alpha : 1.0f) * scroll__child_fade_alpha(n, scale);

    effect_group = compositor__begin_node(n, scale);
    previous_font_node = font__set_node(n);
    const widget_vtable_t* vt = widget_registry__get(n->type);

    if (n->resolved.visibility != GUI_VISIBILITY__HIDDEN)
    {
        if (vt != NULL_POINTER && vt->emit_draws != NULL_POINTER)
        {
            vt->emit_draws(n, scale);
        }
        else
        {
            _scene_render_internal__emit_default(n, scale);
        }
    }

    _scene_render_internal__emit_children_sorted(n, scale);

    if (n->resolved.visibility != GUI_VISIBILITY__HIDDEN && vt != NULL_POINTER && vt->emit_draws_post != NULL_POINTER)
    {
        vt->emit_draws_post(n, scale);
    }
    if (effect_group == TRUE)
    {
        compositor__end_node(n, scale);
    }
    font__set_node(previous_font_node);
    _scene_render_internal__clip       = saved_clip;
    _scene_render_internal__clip_valid = saved_clip_valid;
}

void scene_render__emit_tree(gui_node_t* root, float scale)
{
    if (root == NULL_POINTER)
    {
        return;
    }
    root->effective_opacity = 1.0f;
    // the root's box is the outermost clip: the window widget's scissor
    // when it clips, the back buffer otherwise (nothing draws outside
    // the viewport either way)
    _scene_render_internal__clip       = root->bounds;
    _scene_render_internal__clip_valid = TRUE;
    _scene_render_internal__emit_recursive(root, scale);
    _scene_render_internal__clip_valid = FALSE;
}
