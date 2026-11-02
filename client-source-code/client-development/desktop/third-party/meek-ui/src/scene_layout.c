// scene_layout.c -- layout pass split out of scene.c. Owns the
// column / row / generic dispatch helpers that walk the tree and
// write node->bounds.
// All widget-specific sizing logic still lives in widget_<name>.c
// files; this TU is the coordinator. Accesses shared state via
// scene__root() + scene__scale() (public accessors) rather than
// reaching into scene.c's statics directly.

#include <stdlib.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "scroll.h"
#include "font.h"
#include "motion.h"
#include "clib/stdlib.h"

// Last viewport size passed to scene__layout. Popups / overlays that
// need to center themselves on the window read this via
// scene__viewport. Updated every frame.
static void _scene_layout_internal__paint_bounds(gui_node_t* n, float scale);
static uint64 _scene_layout_internal__text_key(gui_node_t* n, float avail_w, float scale);
static uint64 _scene_layout_internal__mix(uint64 h, float v);

// the layout pass counter the label memo is keyed on: a memo entry from
// an earlier pass never matches, so style, text and font changes between
// frames cannot be served from it
static uint64 _scene_layout_internal__pass = 0;
static int64 _scene_layout_internal__measure_depth = 0;
static int64 _scene_layout_internal__viewport_w = 0;
static int64 _scene_layout_internal__viewport_h = 0;

static float _scene_layout_internal__inset(float px, float pct, boole set, float ref, float scale, boole* out_have);
static void _scene_layout_internal__content_rect(gui_node_t* n, boole scrolled, float* out_x, float* out_y, float* out_w, float* out_h);
static void _scene_layout_internal__padding_rect(gui_node_t* n, boole scrolled, float* out_x, float* out_y, float* out_w, float* out_h);
static gui_node_t* _scene_layout_internal__containing_block(gui_node_t* parent);
static gui_node_t* _scene_layout_internal__scroll_container(gui_node_t* n);
static gui_justify_e _scene_layout_internal__column_main(const gui_style_t* s);
static gui_justify_e _scene_layout_internal__row_main(const gui_style_t* s);
static gui_halign_e _scene_layout_internal__column_cross(const gui_style_t* s, const gui_node_t* c);
static gui_valign_e _scene_layout_internal__row_cross(const gui_style_t* s, const gui_node_t* c);
static void _scene_layout_internal__distribute(gui_justify_e main, float slack, int64 count, boole safe, float* out_offset, float* out_extra_gap);
static boole _scene_layout_internal__in_flow(const gui_node_t* c);
static void _scene_layout_internal__column_child(gui_node_t* parent, gui_node_t* c, float x, float y, float avail_w, float avail_h);

/**
 * @brief lay out a single node by dispatching through its widget vtable. Widget layout functions call this to lay out their children.
 *
 * @param gui_node_t* n -> Node to lay out (may be NULL; no-op).
 * @param float x -> Origin x.
 * @param float y -> Origin y.
 * @param float avail_w -> Available width.
 * @param float avail_h -> Available height.
 *
 * @return void
 */
void scene__layout_node(gui_node_t* n, float x, float y, float avail_w, float avail_h)
{
    gui_node_t* previous_font_node = NULL_POINTER;
    uint64      key                = 0;
    if (n == NULL_POINTER)
    {
        return;
    }
    // avail_w < 0 is a container asking its child for its max-content
    // width (the CSS fit-content measure a hugging column or row needs);
    // column, row, div and label understand it, every other widget gets
    // 0 and reports its own size
    if (avail_w < 0.0f && n->type != GUI_NODE_TYPE__TABLE && n->type != GUI_NODE_TYPE__TD && n->type != GUI_NODE_TYPE__TH && n->type != GUI_NODE_TYPE__DIV && n->type != GUI_NODE_TYPE__TEXT)
    {
        avail_w = 0.0f;
    }
    const widget_vtable_t* vt = widget_registry__get(n->type);
    if (vt == NULL_POINTER || vt->layout == NULL_POINTER)
    {
        n->bounds.x = x;
        n->bounds.y = y;
        n->bounds.w = 0.0f;
        n->bounds.h = 0.0f;
        return;
    }
    // a label's layout reads its text, its resolved style, the scale and
    // the available width, and writes only its bounds; the flows call it
    // several times per pass with the same inputs (measure, overflow,
    // placement, at every nesting level), so the later calls only place
    // the box the first one measured
    if (n->type == GUI_NODE_TYPE__TEXT)
    {
        key = _scene_layout_internal__text_key(n, avail_w, scene__scale());

        if (n->layout_memo_frame == _scene_layout_internal__pass && n->layout_memo_key == key)
        {
            n->bounds.x = x;
            n->bounds.y = y;
            n->bounds.w = n->layout_memo_w;
            n->bounds.h = n->layout_memo_h;
            return;
        }
    }
    float saved_scroll_x = n->scroll_x;
    float saved_scroll_y = n->scroll_y;
    float saved_target_x = n->scroll_x_target;
    float saved_target_y = n->scroll_y_target;
    previous_font_node = font__set_node(n);
    vt->layout(n, x, y, avail_w, avail_h, scene__scale());
    font__set_node(previous_font_node);
    if (_scene_layout_internal__measure_depth > 0)
    {
        n->scroll_x = saved_scroll_x;
        n->scroll_y = saved_scroll_y;
        n->scroll_x_target = saved_target_x;
        n->scroll_y_target = saved_target_y;
    }
    if (n->type == GUI_NODE_TYPE__TEXT)
    {
        n->layout_memo_frame = _scene_layout_internal__pass;
        n->layout_memo_key   = key;
        n->layout_memo_w     = n->bounds.w;
        n->layout_memo_h     = n->bounds.h;
    }
}

void scene__measure_node(gui_node_t* n, float x, float y, float avail_w, float avail_h)
{
    _scene_layout_internal__measure_depth++;
    scene__layout_node(n, x, y, avail_w, avail_h);
    _scene_layout_internal__measure_depth--;
}

boole scene__layout_is_measuring(void)
{
    return (boole)(_scene_layout_internal__measure_depth > 0);
}

static uint64 _scene_layout_internal__mix(uint64 h, float v)
{
    uint bits = 0;

    stdlib__memcpy(&bits, &v, sizeof(bits));
    h = (h ^ (uint64)bits) * 1099511628211ULL;
    return h;
}

// every input text_layout reads besides the text itself (constant
// within a pass) and the font, which the family and size select
static uint64 _scene_layout_internal__text_key(gui_node_t* n, float avail_w, float scale)
{
    gui_style_t* s = &n->resolved;
    uint64       h = 14695981039346656037ULL;

    h = _scene_layout_internal__mix(h, avail_w);
    h = _scene_layout_internal__mix(h, scale);
    h = _scene_layout_internal__mix(h, s->font_size);
    h = _scene_layout_internal__mix(h, s->line_height);
    h = _scene_layout_internal__mix(h, s->size_w);
    h = _scene_layout_internal__mix(h, s->size_h);
    h = _scene_layout_internal__mix(h, s->width_pct);
    h = _scene_layout_internal__mix(h, s->height_pct);
    h = _scene_layout_internal__mix(h, s->max_w);
    h = _scene_layout_internal__mix(h, s->min_w);
    h = _scene_layout_internal__mix(h, s->max_h);
    h = _scene_layout_internal__mix(h, s->min_h);
    h = _scene_layout_internal__mix(h, s->pad_l);
    h = _scene_layout_internal__mix(h, s->pad_r);
    h = _scene_layout_internal__mix(h, s->pad_t);
    h = _scene_layout_internal__mix(h, s->pad_b);
    h = _scene_layout_internal__mix(h, s->border_width);
    h = _scene_layout_internal__mix(h, (float)s->border_style);
    h = _scene_layout_internal__mix(h, (float)s->has_border_color);
    h = _scene_layout_internal__mix(h, s->line_height_px);
    return h;
}

/**
 * @brief resolve a node's effective pixel width from its style, honoring (in priority order): width_pct -> size_w*scale -> fallback to avail_w. Widgets call this at the top of their layout function instead of reading style->size_w directly, so `width: 100%` in a .style file actually scales with the parent container.
 *
 * @param gui_node_t* n -> Node being laid out.
 * @param float avail_w -> Width available from the parent (the fallback).
 * @param float scale -> Global UI scale (pass scene__scale()).
 *
 * @return float -> Pixel width to assign to n->bounds.w.
 */
float scene__layout_width(gui_node_t* n, float avail_w, float scale)
{
    if (avail_w < 0.0f)
    {
        avail_w = 0.0f;
    }
    if (n == NULL_POINTER)
    {
        return avail_w;
    }
    gui_style_t* s = &n->resolved;
    float w = avail_w;
    if (s->width_pct > 0.0f)
    {
        w = avail_w * (s->width_pct / 100.0f);
    }
    else if (s->size_w > 0.0f || s->size_w_explicit == TRUE)
    {
        w = s->size_w * scale;
    }
    scene__apply_min_max(n, &w, NULL_POINTER, scale);
    return w;
}

/**
 * @brief scene__layout_height
 *
 * @return float -> void
 */
float scene__layout_height(gui_node_t* n, float avail_h, float scale)
{
    if (n == NULL_POINTER)
    {
        return avail_h;
    }
    gui_style_t* s = &n->resolved;
    float h = avail_h;
    if (s->height_pct > 0.0f)
    {
        h = avail_h * (s->height_pct / 100.0f);
    }
    else if (s->size_h > 0.0f || s->size_h_explicit == TRUE)
    {
        h = s->size_h * scale;
    }
    scene__apply_min_max(n, NULL_POINTER, &h, scale);
    return h;
}

/**
 * @brief translate a node and its entire descendant subtree by (dx, dy). Pure bounds shift -- no re-measure, no widget vtable calls. Used by layout helpers to apply alignment offsets after a child has already been laid out.
 *
 * @param gui_node_t* n -> Subtree root to move.
 * @param float dx -> Pixel shift on X.
 * @param float dy -> Pixel shift on Y.
 *
 * @return void
 */
void scene__shift_tree(gui_node_t* n, float dx, float dy)
{
    if (n == NULL_POINTER)
    {
        return;
    }
    n->bounds.x += dx;
    n->bounds.y += dy;
    for (gui_node_t* c = n->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        scene__shift_tree(c, dx, dy);
    }
}

static void _scene_layout_internal__shift_tree(gui_node_t* n, float dx, float dy)
{
    scene__shift_tree(n, dx, dy);
}

// Resolve one edge inset to pixels. An edge counts as set when the
// rule wrote a pixel value (non-zero), a percent, or the set flag
// (which is how `top: 0` and negative offsets are told apart from
// "not set"). Percent is relative to `ref`: the containing block's
// width for left / right, its height for top / bottom.
static float _scene_layout_internal__inset(float px, float pct, boole set, float ref, float scale, boole* out_have)
{
    if (px != 0.0f)
    {
        *out_have = TRUE;
        return px * scale;
    }

    if (pct != 0.0f)
    {
        *out_have = TRUE;
        return ref * (pct / 100.0f);
    }

    if (set == TRUE)
    {
        *out_have = TRUE;
        return 0.0f;
    }

    *out_have = FALSE;
    return 0.0f;
}

// Content rect of a node: bounds minus padding and border. With
// `scrolled` the rect is shifted by the node's scroll offsets, which
// puts it in the same coordinates the node's flow children use.
static void _scene_layout_internal__content_rect(gui_node_t* n, boole scrolled, float* out_x, float* out_y, float* out_w, float* out_h)
{
    float scale = scene__scale();
    float bw = scene__border_width(n, scale);
    gui_style_t* s = &n->resolved;
    float x = n->bounds.x + s->pad_l * scale + bw;
    float y = n->bounds.y + s->pad_t * scale + bw;
    float w = n->bounds.w - (s->pad_l + s->pad_r) * scale - 2.0f * bw;
    float h = n->bounds.h - (s->pad_t + s->pad_b) * scale - 2.0f * bw;

    if (scrolled == TRUE)
    {
        // the rubber-band offset moves the children with the scroll
        x = x - n->scroll_x - n->overscroll_x;
        y = y - n->scroll_y - n->overscroll_y;
    }

    if (w < 0.0f)
    {
        w = 0.0f;
    }

    if (h < 0.0f)
    {
        h = 0.0f;
    }

    *out_x = x;
    *out_y = y;
    *out_w = w;
    *out_h = h;
}

// Padding box of a node: bounds minus the border and minus a visible
// scrollbar, which sits between the border and the padding as in CSS.
// This is the containing block of an absolutely positioned child.
static void _scene_layout_internal__padding_rect(gui_node_t* n, boole scrolled, float* out_x, float* out_y, float* out_w, float* out_h)
{
    float scale = scene__scale();
    float bw = scene__border_width(n, scale);
    float x = n->bounds.x + bw;
    float y = n->bounds.y + bw;
    float w = n->bounds.w - 2.0f * bw;
    float h = n->bounds.h - 2.0f * bw;

    if (scroll__vbar_visible(n) == TRUE)
    {
        w = w - scroll__bar_size(n, scale);
    }

    if (scroll__hbar_visible(n) == TRUE)
    {
        h = h - scroll__bar_size(n, scale);
    }

    if (scrolled == TRUE)
    {
        // the rubber-band offset moves the children with the scroll
        x = x - n->scroll_x - n->overscroll_x;
        y = y - n->scroll_y - n->overscroll_y;
    }

    if (w < 0.0f)
    {
        w = 0.0f;
    }

    if (h < 0.0f)
    {
        h = 0.0f;
    }

    *out_x = x;
    *out_y = y;
    *out_w = w;
    *out_h = h;
}

// CSS containing block of an absolutely positioned child: the nearest
// ancestor, starting at the parent, whose own position is not static.
// The root is the fallback.
static gui_node_t* _scene_layout_internal__containing_block(gui_node_t* parent)
{
    gui_node_t* p = parent;

    while (p != NULL_POINTER && p->parent != NULL_POINTER && p->resolved.position == GUI_POSITION__STATIC)
    {
        p = p->parent;
    }

    return p;
}

// Nearest ancestor, starting at `n` itself, that clips or scrolls its
// content (overflow other than visible), or the root. Sticky children
// pin to this node's visible content rect.
static gui_node_t* _scene_layout_internal__scroll_container(gui_node_t* n)
{
    gui_node_t* p = n;

    while (p != NULL_POINTER)
    {
        if (p->parent == NULL_POINTER)
        {
            return p;
        }

        if (p->resolved.overflow_y != GUI_OVERFLOW__VISIBLE || p->resolved.overflow_x != GUI_OVERFLOW__VISIBLE)
        {
            return p;
        }

        p = p->parent;
    }

    return n;
}

/**
 * @brief lays out one position:absolute or position:fixed child. Absolute children anchor to the padding box of the nearest positioned ancestor (the CSS containing block: inside the border, the padding included), fixed children anchor to the viewport. Public so container widgets with their own child loops share the same rules.
 *
 * @param gui_node_t* parent -> the child's parent
 * @param gui_node_t* c -> the absolute or fixed child to position
 * @param float content_x -> parent's content rect x, already shifted by the parent's scroll offset
 * @param float content_y -> parent's content rect y, already shifted by the parent's scroll offset
 * @param float content_w -> parent's content rect width, narrowed by a visible scrollbar
 * @param float content_h -> parent's content rect height
 *
 * @attention two opposite edges stretch the child between them and its explicit width / height on that axis is ignored; a single edge anchors the child's own size to that edge; no edge on an axis anchors at the start edge. Percent insets are relative to the containing block's padding box
 *
 * @return void
 */
void scene__layout_absolute_child(gui_node_t* parent, gui_node_t* c, float content_x, float content_y, float content_w, float content_h)
{
    float scale = scene__scale();
    gui_style_t* cs = &c->resolved;
    gui_node_t* cb = NULL_POINTER;
    int64 vp_w = 0;
    int64 vp_h = 0;
    boole have_l = FALSE;
    boole have_r = FALSE;
    boole have_t = FALSE;
    boole have_b = FALSE;
    float l = 0.0f;
    float r = 0.0f;
    float t = 0.0f;
    float b = 0.0f;
    float child_x = 0.0f;
    float child_y = 0.0f;
    float child_w = 0.0f;
    float child_h = 0.0f;
    float saved_width_pct = cs->width_pct;
    float saved_height_pct = cs->height_pct;
    float saved_size_w = cs->size_w;
    float saved_size_h = cs->size_h;
    boole saved_width_explicit = cs->size_w_explicit;
    boole saved_height_explicit = cs->size_h_explicit;

    if (cs->position == GUI_POSITION__FIXED)
    {
        scene__viewport(&vp_w, &vp_h);
        content_x = 0.0f;
        content_y = 0.0f;
        content_w = (float)vp_w;
        content_h = (float)vp_h;
    }
    else
    {
        cb = _scene_layout_internal__containing_block(parent);

        if (cb == NULL_POINTER)
        {
            cb = parent;
        }

        // the containing block is the padding box of that ancestor (CSS
        // anchors absolute children inside the border, not inside the
        // padding), read from its final bounds rather than from the
        // content rect the caller passed: every container sets its
        // bounds before this post-pass, while the passed rect is 0 high
        // for a hugging container (its children give it its height, so
        // it offers none), which put a bottom-anchored child at the top
        _scene_layout_internal__padding_rect(cb, TRUE, &content_x, &content_y, &content_w, &content_h);
    }

    l = _scene_layout_internal__inset(cs->inset_l, cs->inset_l_pct, cs->inset_l_set, content_w, scale, &have_l);
    r = _scene_layout_internal__inset(cs->inset_r, cs->inset_r_pct, cs->inset_r_set, content_w, scale, &have_r);
    t = _scene_layout_internal__inset(cs->inset_t, cs->inset_t_pct, cs->inset_t_set, content_h, scale, &have_t);
    b = _scene_layout_internal__inset(cs->inset_b, cs->inset_b_pct, cs->inset_b_set, content_h, scale, &have_b);
    // app-region: drag moved the box by the mouse: the offset is part of
    // the placement, so the children, the memo and hit-testing all see it
    l += c->drag_offset_x;
    r -= c->drag_offset_x;
    t += c->drag_offset_y;
    b -= c->drag_offset_y;

    if (have_l == TRUE && have_r == TRUE)
    {
        child_x = content_x + l;
        child_w = content_w - l - r;
    }
    else
    {
        child_w = scene__layout_width(c, content_w, scale);

        if (have_r == TRUE)
        {
            child_x = content_x + content_w - child_w - r;
        }
        else
        {
            child_x = content_x + l;
        }
    }

    if (child_w < 0.0f)
    {
        child_w = 0.0f;
    }

    if (have_t == TRUE && have_b == TRUE)
    {
        child_y = content_y + t;
        child_h = content_h - t - b;
    }
    else
    {
        child_h = scene__layout_height(c, content_h, scale);

        if (have_b == TRUE)
        {
            child_y = content_y + content_h - child_h - b;
        }
        else
        {
            child_y = content_y + t;
        }
    }

    if (child_h < 0.0f)
    {
        child_h = 0.0f;
    }

    // Percentages have already been resolved against the containing block.
    // Present those used dimensions to the widget so it cannot apply the
    // percentage a second time against its own allocated rectangle.
    if (saved_width_pct > 0.0f || (have_l == TRUE && have_r == TRUE))
    {
        cs->width_pct = 0.0f;
        cs->size_w = child_w / scale;
        cs->size_w_explicit = TRUE;
    }
    if (saved_height_pct > 0.0f || (have_t == TRUE && have_b == TRUE))
    {
        cs->height_pct = 0.0f;
        cs->size_h = child_h / scale;
        cs->size_h_explicit = TRUE;
    }
    scene__layout_node(c, child_x, child_y, child_w, child_h);
    cs->width_pct = saved_width_pct;
    cs->height_pct = saved_height_pct;
    cs->size_w = saved_size_w;
    cs->size_h = saved_size_h;
    cs->size_w_explicit = saved_width_explicit;
    cs->size_h_explicit = saved_height_explicit;
    // Auto-sized columns and text establish their final size during layout.
    // Anchor that measured box, including all descendants, to a single end edge.
    if (have_r == TRUE && have_l == FALSE)
    {
        scene__shift_tree(c, content_x + content_w - r - c->bounds.w - c->bounds.x, 0.0f);
    }
    if (have_b == TRUE && have_t == FALSE)
    {
        scene__shift_tree(c, 0.0f, content_y + content_h - b - c->bounds.h - c->bounds.y);
    }
}

/**
 * @brief the post-pass every container runs after its flow pass: lays out each absolute and fixed child against the right containing block
 *
 * @param gui_node_t* parent -> the container
 * @param float content_x -> parent's content rect x, shifted by the parent's scroll offset
 * @param float content_y -> parent's content rect y, shifted by the parent's scroll offset
 * @param float content_w -> parent's content rect width
 * @param float content_h -> parent's content rect height
 *
 * @return void
 */
void scene__layout_positioned_children(gui_node_t* parent, float content_x, float content_y, float content_w, float content_h)
{
    gui_node_t* ac = parent->first_child;

    while (ac != NULL_POINTER)
    {
        if (ac->resolved.display != GUI_DISPLAY__NONE && (ac->resolved.position == GUI_POSITION__ABSOLUTE || ac->resolved.position == GUI_POSITION__FIXED))
        {
            scene__layout_absolute_child(parent, ac, content_x, content_y, content_w, content_h);
        }

        ac = ac->next_sibling;
    }
}

/**
 * @brief applies the post-flow shift of a relative or sticky child right after the flow pass placed it; static, absolute and fixed children are left alone
 *
 * @param gui_node_t* parent -> the child's parent
 * @param gui_node_t* c -> the child, already placed by the flow pass
 * @param float content_w -> parent's content width, the reference for percent insets on the x axis
 * @param float content_h -> parent's content height, the reference for percent insets on the y axis
 *
 * @attention relative: left wins over right and top over bottom when both are set, as in CSS. sticky: the child pins to the nearest scrolling ancestor's visible edge once scrolled past it, but stays inside its parent's content box; the parent's content extent from the previous frame is used for that limit
 *
 * @return void
 */
void scene__apply_position_offset(gui_node_t* parent, gui_node_t* c, float content_w, float content_h)
{
    float scale = scene__scale();
    gui_style_t* cs = &c->resolved;
    boole have_l = FALSE;
    boole have_r = FALSE;
    boole have_t = FALSE;
    boole have_b = FALSE;
    float l = 0.0f;
    float r = 0.0f;
    float t = 0.0f;
    float b = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;
    gui_node_t* sc = NULL_POINTER;
    float sx = 0.0f;
    float sy = 0.0f;
    float sw = 0.0f;
    float sh = 0.0f;
    float px = 0.0f;
    float py = 0.0f;
    float pw = 0.0f;
    float ph = 0.0f;
    float p_bottom = 0.0f;
    float p_right = 0.0f;
    float limit = 0.0f;

    if (cs->position != GUI_POSITION__RELATIVE && cs->position != GUI_POSITION__STICKY)
    {
        return;
    }

    l = _scene_layout_internal__inset(cs->inset_l, cs->inset_l_pct, cs->inset_l_set, content_w, scale, &have_l);
    r = _scene_layout_internal__inset(cs->inset_r, cs->inset_r_pct, cs->inset_r_set, content_w, scale, &have_r);
    t = _scene_layout_internal__inset(cs->inset_t, cs->inset_t_pct, cs->inset_t_set, content_h, scale, &have_t);
    b = _scene_layout_internal__inset(cs->inset_b, cs->inset_b_pct, cs->inset_b_set, content_h, scale, &have_b);
    // app-region: drag moved the box by the mouse: the offset is part of
    // the placement, so the children, the memo and hit-testing all see it
    l += c->drag_offset_x;
    r -= c->drag_offset_x;
    t += c->drag_offset_y;
    b -= c->drag_offset_y;

    if (cs->position == GUI_POSITION__RELATIVE)
    {
        if (have_l == TRUE)
        {
            dx = l;
        }
        else if (have_r == TRUE)
        {
            dx = -r;
        }

        if (have_t == TRUE)
        {
            dy = t;
        }
        else if (have_b == TRUE)
        {
            dy = -b;
        }
    }
    else
    {
        // Sticky. The visible rect of the nearest scrolling ancestor is
        // the pinning frame; the parent's content box is the limit the
        // child may never leave.
        sc = _scene_layout_internal__scroll_container(parent);
        _scene_layout_internal__content_rect(sc, FALSE, &sx, &sy, &sw, &sh);
        _scene_layout_internal__content_rect(parent, TRUE, &px, &py, &pw, &ph);
        p_bottom = py + ph;
        p_right = px + pw;

        if (parent->content_h > 0.0f)
        {
            p_bottom = py + parent->content_h - parent->resolved.pad_b * scale;
        }

        if (parent->content_w > 0.0f)
        {
            p_right = px + parent->content_w - parent->resolved.pad_r * scale;
        }

        if (have_t == TRUE)
        {
            limit = sy + t;

            if (c->bounds.y < limit)
            {
                dy = limit - c->bounds.y;
            }
        }

        if (have_b == TRUE)
        {
            limit = sy + sh - b - c->bounds.h;

            if (c->bounds.y + dy > limit)
            {
                dy = limit - c->bounds.y;
            }
        }

        if (have_l == TRUE)
        {
            limit = sx + l;

            if (c->bounds.x < limit)
            {
                dx = limit - c->bounds.x;
            }
        }

        if (have_r == TRUE)
        {
            limit = sx + sw - r - c->bounds.w;

            if (c->bounds.x + dx > limit)
            {
                dx = limit - c->bounds.x;
            }
        }

        // never leave the parent's content box
        if (c->bounds.y + dy + c->bounds.h > p_bottom)
        {
            dy = p_bottom - c->bounds.h - c->bounds.y;
        }

        if (c->bounds.y + dy < py)
        {
            dy = py - c->bounds.y;
        }

        if (c->bounds.x + dx + c->bounds.w > p_right)
        {
            dx = p_right - c->bounds.w - c->bounds.x;
        }

        if (c->bounds.x + dx < px)
        {
            dx = px - c->bounds.x;
        }
    }

    if (dx != 0.0f || dy != 0.0f)
    {
        scene__shift_tree(c, dx, dy);
    }
}

/**
 * @brief lay out parent's children inside parent's bounds as a vertical column, honoring the parent's resolved padding, border and gap; the placement itself is scene__layout_column_flow. Widgets (Column) call this to do the standard column placement without re-implementing the math.
 *
 * @param gui_node_t* parent -> Parent whose children to lay out.
 *
 * @return void
 */
void scene__layout_children_as_column(gui_node_t* parent)
{
    gui_style_t* s     = &parent->resolved;
    float      scale = scene__scale();
    float      bw    = scene__border_width(parent, scale);
    float      cx    = parent->bounds.x + s->pad_l * scale + bw;
    float      cy    = parent->bounds.y + s->pad_t * scale + bw;
    float      cw    = parent->bounds.w - (s->pad_l + s->pad_r) * scale - 2.0f * bw;
    float      ch    = parent->bounds.h - (s->pad_t + s->pad_b) * scale - 2.0f * bw;
    if (cw < 0.0f)
    {
        cw = 0.0f;
    }
    if (ch < 0.0f)
    {
        ch = 0.0f;
    }

    scene__layout_column_flow(parent, cx, cy, cw, ch, 0.0f, 0.0f, NULL_POINTER, NULL_POINTER);
}

/**
 * @brief horizontal counterpart of scene__layout_children_as_column; the placement itself is scene__layout_row_flow. Children receive the full inner-height (parent.h minus pad_t/pad_b) so widgets that auto-size to their content (Text, checkbox, etc.) can pick the size they want; widgets that expand to fill (e.g. explicit size_w) just consume that width and the cursor advances past them.
 *
 * @param gui_node_t* parent -> Parent whose children to lay out.
 *
 * @return void
 */
void scene__layout_children_as_row(gui_node_t* parent)
{
    gui_style_t* s     = &parent->resolved;
    float      scale = scene__scale();
    float      bw    = scene__border_width(parent, scale);
    float      cx    = parent->bounds.x + s->pad_l * scale + bw;
    float      cy    = parent->bounds.y + s->pad_t * scale + bw;
    float      cw    = parent->bounds.w - (s->pad_l + s->pad_r) * scale - 2.0f * bw;
    float      ch    = parent->bounds.h - (s->pad_t + s->pad_b) * scale - 2.0f * bw;
    if (cw < 0.0f)
    {
        cw = 0.0f;
    }
    if (ch < 0.0f)
    {
        ch = 0.0f;
    }

    scene__layout_row_flow(parent, cx, cy, cw, ch);
}

// Effective main-axis alignment of a column-like container: justify-content
// when set, else the legacy vertical-alignment.
static gui_justify_e _scene_layout_internal__column_main(const gui_style_t* s)
{
    if (s->justify_content != GUI_JUSTIFY__UNSET)
    {
        return s->justify_content;
    }

    if (s->valign == GUI_VALIGN__CENTER)
    {
        return GUI_JUSTIFY__CENTER;
    }

    if (s->valign == GUI_VALIGN__BOTTOM)
    {
        return GUI_JUSTIFY__END;
    }

    return GUI_JUSTIFY__START;
}

// Effective main-axis alignment of a row: justify-content when set, else the
// legacy horizontal-alignment.
static gui_justify_e _scene_layout_internal__row_main(const gui_style_t* s)
{
    if (s->justify_content != GUI_JUSTIFY__UNSET)
    {
        return s->justify_content;
    }

    if (s->halign == GUI_HALIGN__CENTER)
    {
        return GUI_JUSTIFY__CENTER;
    }

    if (s->halign == GUI_HALIGN__RIGHT)
    {
        return GUI_JUSTIFY__END;
    }

    return GUI_JUSTIFY__START;
}

// Cross-axis alignment of one child in a column: its align-self, else the
// container's align-items, else the container's horizontal-alignment.
static gui_halign_e _scene_layout_internal__column_cross(const gui_style_t* s, const gui_node_t* c)
{
    gui_align_e a = (c->resolved.align_self != GUI_ALIGN__UNSET) ? c->resolved.align_self : s->align_items;

    if (a == GUI_ALIGN__START || a == GUI_ALIGN__STRETCH)
    {
        return GUI_HALIGN__LEFT;
    }

    if (a == GUI_ALIGN__CENTER)
    {
        return GUI_HALIGN__CENTER;
    }

    if (a == GUI_ALIGN__END)
    {
        return GUI_HALIGN__RIGHT;
    }

    return s->halign;
}

// Cross-axis alignment of one child in a row: its align-self, else the
// container's align-items, else the container's vertical-alignment.
static gui_valign_e _scene_layout_internal__row_cross(const gui_style_t* s, const gui_node_t* c)
{
    gui_align_e a = (c->resolved.align_self != GUI_ALIGN__UNSET) ? c->resolved.align_self : s->align_items;

    if (a == GUI_ALIGN__START || a == GUI_ALIGN__STRETCH)
    {
        return GUI_VALIGN__TOP;
    }

    if (a == GUI_ALIGN__CENTER)
    {
        return GUI_VALIGN__CENTER;
    }

    if (a == GUI_ALIGN__END)
    {
        return GUI_VALIGN__BOTTOM;
    }

    return s->valign;
}

// Main-axis distribution of the free space: the offset before the first
// child and the extra space after each child (the space-* keywords).
// Negative free space (content that overflows) follows CSS: center and
// end let the children overflow both ends or the start, the space-*
// keywords fall back to start (between) or center (around, evenly).
// With `safe` the children stay at the start instead when they
// overflow, which is what a scrolling container needs so its first
// item can be reached.
static void _scene_layout_internal__distribute(gui_justify_e main, float slack, int64 count, boole safe, float* out_offset, float* out_extra_gap)
{
    *out_offset    = 0.0f;
    *out_extra_gap = 0.0f;

    if (count <= 0)
    {
        return;
    }

    if (slack < 0.0f)
    {
        if (safe == TRUE || main == GUI_JUSTIFY__SPACE_BETWEEN)
        {
            return;
        }

        if (main == GUI_JUSTIFY__SPACE_AROUND || main == GUI_JUSTIFY__SPACE_EVENLY)
        {
            main = GUI_JUSTIFY__CENTER;
        }
    }

    switch (main)
    {
        case GUI_JUSTIFY__CENTER:
            *out_offset = slack * 0.5f;
            break;
        case GUI_JUSTIFY__END:
            *out_offset = slack;
            break;
        case GUI_JUSTIFY__SPACE_BETWEEN:
            if (count > 1)
            {
                *out_extra_gap = slack / (float)(count - 1);
            }
            break;
        case GUI_JUSTIFY__SPACE_AROUND:
            *out_extra_gap = slack / (float)count;
            *out_offset    = *out_extra_gap * 0.5f;
            break;
        case GUI_JUSTIFY__SPACE_EVENLY:
            *out_extra_gap = slack / (float)(count + 1);
            *out_offset    = *out_extra_gap;
            break;
        default:
            break;
    }
}

// A child takes part in the flow unless it is display: none or lifted out
// by position: absolute / fixed.
static boole _scene_layout_internal__in_flow(const gui_node_t* c)
{
    if (c->resolved.display == GUI_DISPLAY__NONE)
    {
        return FALSE;
    }

    if (c->resolved.position == GUI_POSITION__ABSOLUTE || c->resolved.position == GUI_POSITION__FIXED)
    {
        return FALSE;
    }

    return TRUE;
}

// Lays out one flow child of a column. A child that is aligned rather than
// stretched (center / right, or its own align-self) and has no width of its
// own is shrink-wrapped the CSS way: its max-content width, capped at the
// width the column offers.
static void _scene_layout_internal__column_child(gui_node_t* parent, gui_node_t* c, float x, float y, float avail_w, float avail_h)
{
    gui_halign_e cross = _scene_layout_internal__column_cross(&parent->resolved, c);

    if (cross != GUI_HALIGN__LEFT && c->resolved.size_w <= 0.0f && c->resolved.width_pct <= 0.0f)
    {
        scene__layout_node(c, x, y, -1.0f, avail_h);

        if (c->bounds.w > avail_w)
        {
            scene__layout_node(c, x, y, avail_w, avail_h);
        }

        return;
    }

    scene__layout_node(c, x, y, avail_w, avail_h);
}

/**
 * @brief the column flow every column-like container shares (column, div, window): places the parent's flow children top to bottom inside the given content rect, honoring margins, gap, the main-axis alignment (vertical-alignment or justify-content, including the space-* distributions), the cross-axis alignment (horizontal-alignment, align-items, align-self) with CSS shrink-wrapping of aligned auto-width children, then the relative / sticky offsets, and finally places the absolute / fixed children against the content rect
 *
 * @param gui_node_t* parent -> the container
 * @param float cx -> content rect x, unscrolled
 * @param float cy -> content rect y, unscrolled
 * @param float cw -> content rect width, narrowed for a visible scrollbar
 * @param float ch -> content rect height
 * @param float scroll_x -> the parent's horizontal scroll offset, subtracted from every child's x
 * @param float scroll_y -> the parent's vertical scroll offset, subtracted from every child's y
 * @param float* out_content_w -> receives the width of the children's extent measured from cx, margins included, or NULL
 * @param float* out_content_h -> receives the height of the children's extent measured from cy as if unscrolled, or NULL
 *
 * @attention each child receives the height left below it as avail_h, which is what lets a div fill the rest of the column; a percent height is a share of the space left after the other flow children, so a 100 % child between two fixed bars fills exactly the gap between them
 *
 * @return void
 */
void scene__layout_column_flow(gui_node_t* parent, float cx, float cy, float cw, float ch, float scroll_x, float scroll_y, float* out_content_w, float* out_content_h)
{
    gui_style_t*  s              = &parent->resolved;
    float         scale          = scene__scale();
    gui_justify_e main           = _scene_layout_internal__column_main(s);
    float         gap            = s->gap * scale;
    float         total_h        = 0.0f;
    int64         count          = 0;
    float         offset         = 0.0f;
    float         extra_gap      = 0.0f;
    float         x0             = cx - scroll_x;
    float         y0             = cy - scroll_y;
    float         y_cursor       = 0.0f;
    // Start at the scrolled content origin. Starting at the viewport origin
    // invents content when every child is above it after a content shrink.
    float         content_bottom = y0;
    float         content_right  = cx;
    gui_node_t*   c              = NULL_POINTER;
    boole         has_pct        = FALSE;
    float         pct_avail      = 0.0f;
    float         fixed_total    = 0.0f;
    int64         all            = 0;

    // a percent height is a share of the space left after the other flow
    // children, the fill-the-rest idiom (21-height-pct-repro shows it), so
    // the fixed siblings are measured first whenever a child asks for one
    for (c = parent->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        if (_scene_layout_internal__in_flow(c) == TRUE && c->resolved.height_pct > 0.0f && c->resolved.size_h <= 0.0f)
        {
            has_pct = TRUE;
        }
    }

    if (has_pct == TRUE)
    {
        for (c = parent->first_child; c != NULL_POINTER; c = c->next_sibling)
        {
            float m_l      = 0.0f;
            float m_r      = 0.0f;
            float child_cw = 0.0f;

            if (_scene_layout_internal__in_flow(c) == FALSE)
            {
                continue;
            }

            all++;

            if (c->resolved.height_pct > 0.0f && c->resolved.size_h <= 0.0f)
            {
                continue;
            }

            m_l      = c->resolved.margin_l * scale;
            m_r      = c->resolved.margin_r * scale;
            child_cw = cw - (m_l + m_r);

            if (child_cw < 0.0f)
            {
                child_cw = 0.0f;
            }

            _scene_layout_internal__measure_depth++;
            _scene_layout_internal__column_child(parent, c, x0 + m_l, 0.0f, child_cw, 0.0f);
            _scene_layout_internal__measure_depth--;
            fixed_total += c->bounds.h + (c->resolved.margin_t + c->resolved.margin_b) * scale;
        }

        if (all > 1)
        {
            fixed_total += gap * (float)(all - 1);
        }

        pct_avail = ch - fixed_total;

        if (pct_avail < 0.0f)
        {
            pct_avail = 0.0f;
        }
    }

    // measure pass, only when the main-axis alignment needs the total height
    if (main != GUI_JUSTIFY__START)
    {
        for (c = parent->first_child; c != NULL_POINTER; c = c->next_sibling)
        {
            float m_l      = 0.0f;
            float m_r      = 0.0f;
            float child_cw = 0.0f;

            if (_scene_layout_internal__in_flow(c) == FALSE)
            {
                continue;
            }

            m_l      = c->resolved.margin_l * scale;
            m_r      = c->resolved.margin_r * scale;
            child_cw = cw - (m_l + m_r);

            if (child_cw < 0.0f)
            {
                child_cw = 0.0f;
            }

            _scene_layout_internal__measure_depth++;
            _scene_layout_internal__column_child(parent, c, x0 + m_l, 0.0f, child_cw, (c->resolved.height_pct > 0.0f && c->resolved.size_h <= 0.0f) ? pct_avail : 0.0f);
            _scene_layout_internal__measure_depth--;
            total_h += c->bounds.h + (c->resolved.margin_t + c->resolved.margin_b) * scale;
            count++;
        }

        if (count > 1)
        {
            total_h += gap * (float)(count - 1);
        }

        // a scrolling or clipping column keeps overflowing content at the
        // top (safe alignment) so the scroll position can reach it
        _scene_layout_internal__distribute(main, ch - total_h, count, (boole)(s->overflow_y != GUI_OVERFLOW__VISIBLE || s->overflow_x != GUI_OVERFLOW__VISIBLE), &offset, &extra_gap);
    }

    y_cursor = y0 + offset;

    for (c = parent->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        float        m_t      = 0.0f;
        float        m_b      = 0.0f;
        float        m_l      = 0.0f;
        float        m_r      = 0.0f;
        float        child_x  = 0.0f;
        float        child_cw = 0.0f;
        float        slack_w  = 0.0f;
        gui_halign_e cross    = GUI_HALIGN__LEFT;

        if (_scene_layout_internal__in_flow(c) == FALSE)
        {
            continue;
        }

        m_t = c->resolved.margin_t * scale;
        m_b = c->resolved.margin_b * scale;
        m_l = c->resolved.margin_l * scale;
        m_r = c->resolved.margin_r * scale;

        y_cursor += m_t;
        child_x   = x0 + m_l;
        child_cw  = cw - (m_l + m_r);

        if (child_cw < 0.0f)
        {
            child_cw = 0.0f;
        }

        // a percent child gets the space the fixed siblings left, every
        // other child the height left below it
        _scene_layout_internal__column_child(parent, c, child_x, y_cursor, child_cw, (c->resolved.height_pct > 0.0f && c->resolved.size_h <= 0.0f) ? pct_avail : ch - (y_cursor - y0));

        cross = _scene_layout_internal__column_cross(s, c);

        if (cross != GUI_HALIGN__LEFT)
        {
            slack_w = child_cw - c->bounds.w;

            if (slack_w > 0.0f)
            {
                scene__shift_tree(c, (cross == GUI_HALIGN__CENTER) ? slack_w * 0.5f : slack_w, 0.0f);
            }
        }

        // relative / sticky shift, applied after the flow placement
        scene__apply_position_offset(parent, c, cw, ch);

        if (c->bounds.y + c->bounds.h + m_b > content_bottom)
        {
            content_bottom = c->bounds.y + c->bounds.h + m_b;
        }

        if (c->bounds.x + scroll_x + c->bounds.w + m_r > content_right)
        {
            content_right = c->bounds.x + scroll_x + c->bounds.w + m_r;
        }

        y_cursor += c->bounds.h + m_b + gap + extra_gap;
    }

    // absolute children anchor to their containing block, fixed children to the viewport
    scene__layout_positioned_children(parent, x0, y0, cw, ch);

    if (out_content_w != NULL_POINTER)
    {
        *out_content_w = content_right - cx;
    }

    if (out_content_h != NULL_POINTER)
    {
        *out_content_h = (content_bottom - cy) + scroll_y;
    }
}

/**
 * @brief the row flow: places the parent's flow children left to right inside the given content rect, honoring margins, gap, the main-axis alignment (horizontal-alignment or justify-content, including the space-* distributions, with the children shrink-wrapped to their content as flex items are), the cross-axis alignment (vertical-alignment, align-items, align-self), then the relative / sticky offsets, and finally places the absolute / fixed children
 *
 * @param gui_node_t* parent -> the container
 * @param float cx -> content rect x
 * @param float cy -> content rect y
 * @param float cw -> content rect width
 * @param float ch -> content rect height
 *
 * @attention without a main-axis alignment each child receives the width left after the previous ones as avail_w, so a div fills the rest of the row; a percent width is a share of the space left after the other flow children
 *
 * @return void
 */
void scene__layout_row_flow(gui_node_t* parent, float cx, float cy, float cw, float ch)
{
    gui_style_t*  s         = &parent->resolved;
    float         scale     = scene__scale();
    gui_justify_e main      = _scene_layout_internal__row_main(s);
    boole         shrink    = (boole)(main != GUI_JUSTIFY__START);
    float         gap       = s->gap * scale;
    float         total_w   = 0.0f;
    int64         count     = 0;
    float         offset    = 0.0f;
    float         extra_gap = 0.0f;
    float         x_cursor  = 0.0f;
    gui_node_t*   c         = NULL_POINTER;
    boole         has_pct   = FALSE;
    float         pct_avail = 0.0f;
    float         fixed_total = 0.0f;
    int64         all       = 0;
    // a div with no width of its own fills the space the fixed siblings
    // leave, like a flex: 1 item, however many of them there are and
    // wherever they sit in the row; the percent children take their
    // share of the same space first
    boole         has_fill   = FALSE;
    int64         fill_count = 0;
    float         pct_sum    = 0.0f;
    float         fill_avail = 0.0f;

    // a percent width is a share of the space left after the other flow
    // children (see the column flow); the fixed siblings are measured
    // first, unconstrained, whenever a child asks for one
    for (c = parent->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        if (_scene_layout_internal__in_flow(c) == TRUE && c->resolved.width_pct > 0.0f && c->resolved.size_w <= 0.0f)
        {
            has_pct  = TRUE;
            pct_sum += c->resolved.width_pct / 100.0f;
        }
        else if (_scene_layout_internal__in_flow(c) == TRUE && c->type == GUI_NODE_TYPE__DIV && c->resolved.size_w <= 0.0f)
        {
            has_fill = TRUE;
            fill_count++;
        }
    }

    if (has_pct == TRUE || has_fill == TRUE)
    {
        for (c = parent->first_child; c != NULL_POINTER; c = c->next_sibling)
        {
            float m_l      = 0.0f;
            float m_r      = 0.0f;
            float child_ch = 0.0f;

            if (_scene_layout_internal__in_flow(c) == FALSE)
            {
                continue;
            }

            all++;

            if (c->resolved.width_pct > 0.0f && c->resolved.size_w <= 0.0f)
            {
                continue;
            }

            if (c->type == GUI_NODE_TYPE__DIV && c->resolved.size_w <= 0.0f)
            {
                continue;
            }

            m_l      = c->resolved.margin_l * scale;
            m_r      = c->resolved.margin_r * scale;
            child_ch = ch - (c->resolved.margin_t + c->resolved.margin_b) * scale;

            if (child_ch < 0.0f)
            {
                child_ch = 0.0f;
            }

            scene__measure_node(c, 0.0f, cy, -1.0f, child_ch);

            if (c->bounds.w > cw - (m_l + m_r))
            {
                scene__measure_node(c, 0.0f, cy, cw - (m_l + m_r), child_ch);
            }

            fixed_total += c->bounds.w + m_l + m_r;
        }

        if (all > 1)
        {
            fixed_total += gap * (float)(all - 1);
        }

        pct_avail = cw - fixed_total;

        if (pct_avail < 0.0f)
        {
            pct_avail = 0.0f;
        }

        if (fill_count > 0)
        {
            fill_avail = pct_avail * (1.0f - pct_sum) / (float)fill_count;
            if (fill_avail < 0.0f)
            {
                fill_avail = 0.0f;
            }
        }
    }

    if (shrink == TRUE)
    {
        for (c = parent->first_child; c != NULL_POINTER; c = c->next_sibling)
        {
            float m_l      = 0.0f;
            float m_r      = 0.0f;
            float child_ch = 0.0f;

            if (_scene_layout_internal__in_flow(c) == FALSE)
            {
                continue;
            }

            m_l      = c->resolved.margin_l * scale;
            m_r      = c->resolved.margin_r * scale;
            child_ch = ch - (c->resolved.margin_t + c->resolved.margin_b) * scale;

            if (child_ch < 0.0f)
            {
                child_ch = 0.0f;
            }

            scene__measure_node(c, 0.0f, cy, (c->resolved.width_pct > 0.0f && c->resolved.size_w <= 0.0f) ? pct_avail : -1.0f, child_ch);

            if (c->bounds.w > cw - (m_l + m_r))
            {
                scene__measure_node(c, 0.0f, cy, cw - (m_l + m_r), child_ch);
            }

            total_w += c->bounds.w + m_l + m_r;
            count++;
        }

        if (count > 1)
        {
            total_w += gap * (float)(count - 1);
        }

        _scene_layout_internal__distribute(main, cw - total_w, count, FALSE, &offset, &extra_gap);
    }

    x_cursor = cx + offset;

    for (c = parent->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        float        m_t         = 0.0f;
        float        m_b         = 0.0f;
        float        m_l         = 0.0f;
        float        m_r         = 0.0f;
        float        remaining_w = 0.0f;
        float        child_cy    = 0.0f;
        float        child_ch    = 0.0f;
        float        slack_h     = 0.0f;
        gui_valign_e cross       = GUI_VALIGN__TOP;

        if (_scene_layout_internal__in_flow(c) == FALSE)
        {
            continue;
        }

        m_t = c->resolved.margin_t * scale;
        m_b = c->resolved.margin_b * scale;
        m_l = c->resolved.margin_l * scale;
        m_r = c->resolved.margin_r * scale;

        x_cursor   += m_l;
        remaining_w = cw - (x_cursor - cx);

        if (remaining_w < 0.0f)
        {
            remaining_w = 0.0f;
        }

        child_cy = cy + m_t;
        child_ch = ch - (m_t + m_b);

        if (child_ch < 0.0f)
        {
            child_ch = 0.0f;
        }

        if (c->resolved.width_pct > 0.0f && c->resolved.size_w <= 0.0f)
        {
            // a percent child gets the space the fixed siblings left
            scene__layout_node(c, x_cursor, child_cy, pct_avail, child_ch);
        }
        else if (shrink == FALSE && c->type == GUI_NODE_TYPE__DIV && has_fill == TRUE)
        {
            // a fill div gets its share of what the fixed and percent siblings left
            scene__layout_node(c, x_cursor, child_cy, fill_avail, child_ch);
        }
        else if (shrink == TRUE)
        {
            scene__layout_node(c, x_cursor, child_cy, -1.0f, child_ch);

            if (c->bounds.w > remaining_w)
            {
                scene__layout_node(c, x_cursor, child_cy, remaining_w, child_ch);
            }
        }
        else
        {
            scene__layout_node(c, x_cursor, child_cy, remaining_w, child_ch);
        }

        cross = _scene_layout_internal__row_cross(s, c);

        if (cross != GUI_VALIGN__TOP)
        {
            slack_h = child_ch - c->bounds.h;

            if (slack_h > 0.0f)
            {
                scene__shift_tree(c, 0.0f, (cross == GUI_VALIGN__CENTER) ? slack_h * 0.5f : slack_h);
            }
        }

        // relative / sticky shift, applied after the flow placement
        scene__apply_position_offset(parent, c, cw, ch);

        x_cursor += c->bounds.w + m_r + gap + extra_gap;
    }

    // absolute children anchor to their containing block, fixed children to the viewport
    scene__layout_positioned_children(parent, cx, cy, cw, ch);
}

/**
 * @brief clamps a widget's final size to its min-/max-width and -height (logical pixels times the scale); a zero limit is no limit
 *
 * @param gui_node_t* n -> the node whose resolved style holds the limits
 * @param float* w -> width to clamp, or NULL
 * @param float* h -> height to clamp, or NULL
 * @param float scale -> global UI scale
 *
 * @return void
 */
void scene__apply_min_max(gui_node_t* n, float* w, float* h, float scale)
{
    gui_style_t* s = &n->resolved;

    if (w != NULL_POINTER)
    {
        if (s->min_w > 0.0f && *w < s->min_w * scale)
        {
            *w = s->min_w * scale;
        }

        if (s->max_w > 0.0f && *w > s->max_w * scale)
        {
            *w = s->max_w * scale;
        }
    }

    if (h != NULL_POINTER)
    {
        if (s->min_h > 0.0f && *h < s->min_h * scale)
        {
            *h = s->min_h * scale;
        }

        if (s->max_h > 0.0f && *h > s->max_h * scale)
        {
            *h = s->max_h * scale;
        }
    }
}

/**
 * @brief run the layout pass on the active tree using the given viewport.
 *
 * @param int64 viewport_w -> Viewport width in pixels.
 * @param int64 viewport_h -> Viewport height in pixels.
 *
 * @return void
 */
void scene__layout(int64 viewport_w, int64 viewport_h)
{
    _scene_layout_internal__viewport_w = viewport_w;
    _scene_layout_internal__viewport_h = viewport_h;
    _scene_layout_internal__pass++;
    scene__layout_node(scene__root(), 0.0f, 0.0f, (float)viewport_w, (float)viewport_h);
    _scene_layout_internal__paint_bounds(scene__root(), scene__scale());
}

// The subtree paint bounds the paint walk culls against (gui_node_t
// paint_bounds / paint_bounded). Every draw a node makes stays inside its
// own bounds except a box shadow (offset plus blur), the padding an
// effect group adds around a filter or a text effect, and anything under
// a transform. The margin is the one compositor__begin_node uses for its
// own visibility test (256 logical px) plus the shadow's reach, so a node
// is only skipped when it could not have touched the clip through any
// effect; a transform or a navigation effect makes the node and every
// ancestor unbounded instead.
static void _scene_layout_internal__paint_bounds(gui_node_t* n, float scale)
{
    gui_node_t* c      = NULL_POINTER;
    const char* blur   = NULL_POINTER;
    char        value[256];
    float       margin = 0.0f;
    float       reach  = 0.0f;
    float       right  = 0.0f;
    float       bottom = 0.0f;
    float       cr     = 0.0f;
    float       cb     = 0.0f;

    // filled by motion__value_id (which terminates first) before it is read
    value[0] = 0;

    if (n == NULL_POINTER)
    {
        return;
    }

    margin                 = 256.0f * scale + 8.0f;
    n->subtree_backdrop_px = 0.0f;

    if (motion__has_id(n, MOTION_PROP__BACKDROP_FILTER) == TRUE && motion__value_id(n, MOTION_PROP__BACKDROP_FILTER, value, (int64)sizeof(value)) == TRUE)
    {
        blur = stdlib__strstr(value, "blur(");

        if (blur != NULL_POINTER)
        {
            n->subtree_backdrop_px = motion__number(blur + 5, 0.0f) * scale;
        }
    }

    if (n->resolved.has_shadow == TRUE)
    {
        reach = stdlib__fabsf(n->resolved.shadow_dx) + stdlib__fabsf(n->resolved.shadow_dy) + n->resolved.shadow_blur * 2.0f + 2.0f;

        if (reach > margin)
        {
            margin = reach;
        }
    }

    n->paint_bounds.x = n->bounds.x - margin;
    n->paint_bounds.y = n->bounds.y - margin;
    n->paint_bounds.w = n->bounds.w + margin * 2.0f;
    n->paint_bounds.h = n->bounds.h + margin * 2.0f;
    n->paint_bounded  = (boole)(n->navigation_effect == FALSE && motion__has_id(n, MOTION_PROP__TRANSFORM) == FALSE);

    for (c = n->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        _scene_layout_internal__paint_bounds(c, scale);

        if (c->resolved.display == GUI_DISPLAY__NONE)
        {
            continue;
        }

        if (c->paint_bounded == FALSE)
        {
            n->paint_bounded = FALSE;
        }

        if (c->subtree_backdrop_px > n->subtree_backdrop_px)
        {
            n->subtree_backdrop_px = c->subtree_backdrop_px;
        }

        right  = n->paint_bounds.x + n->paint_bounds.w;
        bottom = n->paint_bounds.y + n->paint_bounds.h;
        cr     = c->paint_bounds.x + c->paint_bounds.w;
        cb     = c->paint_bounds.y + c->paint_bounds.h;

        if (c->paint_bounds.x < n->paint_bounds.x)
        {
            n->paint_bounds.x = c->paint_bounds.x;
        }

        if (c->paint_bounds.y < n->paint_bounds.y)
        {
            n->paint_bounds.y = c->paint_bounds.y;
        }

        if (cr > right)
        {
            right = cr;
        }

        if (cb > bottom)
        {
            bottom = cb;
        }

        n->paint_bounds.w = right - n->paint_bounds.x;
        n->paint_bounds.h = bottom - n->paint_bounds.y;
    }
}

/**
 * @brief read the last viewport size passed to scene__layout. Written each frame; popups / overlays use this to center themselves.
 *
 * @param int64* out_w -> Receives width in pixels. May be NULL.
 * @param int64* out_h -> Receives height in pixels. May be NULL.
 *
 * @return void
 */
void scene__viewport(int64* out_w, int64* out_h)
{
    if (out_w != NULL_POINTER)
    {
        *out_w = _scene_layout_internal__viewport_w;
    }
    if (out_h != NULL_POINTER)
    {
        *out_h = _scene_layout_internal__viewport_h;
    }
}
