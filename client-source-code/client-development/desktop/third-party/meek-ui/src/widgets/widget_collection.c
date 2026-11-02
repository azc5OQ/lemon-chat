// widget_collection.c - <collection> widget.
// Arranges its children in one of three patterns, chosen via the
// style `layout` property:
// grid    fixed number of columns (style `columns: N`), rows grown
// to fit; cell size is `item-width` x `item-height` (falls
// back to the cell's natural size if 0). Children are
// placed left-to-right, top-to-bottom. Typical launcher /
// gallery look.
// list    single-row horizontal strip. Each child gets item-width
// slot (or its own width if 0). No wrap.
// flow    wrap onto as many rows as fit in the available width.
// Useful for tag clouds / pill buttons / similar.
// Children can be any node type -- the collection doesn't care
// whether you put <image>, <div>, <button>, etc. inside it. A
// typical launcher entry is a small <div> holding an <image> +
// <label> so tap targets capture both.
// Gap between cells comes from the standard `gap` style property
// (same one used by <column> / <row>). `pad` is honored too.

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "renderer.h"

static void _collection_internal__layout_grid(gui_node_t* n, float scale)
{
    gui_style_t* s = &n->resolved;
    int   cols       = (s->collection_columns > 0) ? s->collection_columns : 4;
    float pad_t      = s->pad_t * scale;
    float pad_r      = s->pad_r * scale;
    float pad_b      = s->pad_b * scale;
    float pad_l      = s->pad_l * scale;
    float gap        = s->gap * scale;
    // Border eats from the inner content area on both sides of each
    // axis -- if border-width is 4px, inner_w is `bounds.w - 2*4 -
    // pad_l - pad_r`. Without this, child cells would overflow into
    // the border line on the right edge. scene__border_width applies
    // the same three-way gate scene__emit_border uses so a partial
    // declaration doesn't reserve invisible space.
    float border_w   = scene__border_width(n, scale);
    float inner_w    = n->bounds.w - (pad_l + pad_r) - 2.0f * border_w;
    if (inner_w < 0.0f)
    {
        inner_w = 0.0f;
    }

    // Compute cell width: total inner width minus gaps, divided by
    // columns. Style `item-width` overrides when > 0 (fixed cells,
    // may produce empty slack on the right).
    float cell_w;
    if (s->item_width > 0.0f)
    {
        cell_w = s->item_width * scale;
    }
    else
    {
        cell_w = (inner_w - gap * (float)(cols - 1)) / (float)cols;
        if (cell_w < 0.0f)
        {
            cell_w = 0.0f;
        }
    }
    // Rows are item-height tall when given, else as tall as their tallest
    // child, the way a CSS grid sizes auto rows (cell_h 0 hands each child
    // no height to fill, so it takes its own).
    float cell_h = (s->item_height > 0.0f) ? s->item_height * scale : 0.0f;

    // Origin shifts by border_w so children sit inside the line, not
    // under it. Same reasoning as the inner_w shrink above.
    float x0 = n->bounds.x + pad_l + border_w;
    float y0 = n->bounds.y + pad_t + border_w;

    int col_idx = 0;
    float row_y = y0;
    float row_h = 0.0f;
    float max_row_bottom = y0;

    for (gui_node_t* c = n->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        if (c->resolved.display == GUI_DISPLAY__NONE)
        {
            continue;
        }
        float cx = x0 + (float)col_idx * (cell_w + gap);
        float cy = row_y;
        // Each cell gets exactly cell_w x cell_h to work with. The
        // child's own layout decides how to fill it (via its size_*
        // style + layout vtable).
        scene__layout_node(c, cx, cy, cell_w, cell_h);
        if (c->bounds.h > row_h)
        {
            row_h = c->bounds.h;
        }
        if (c->bounds.y + c->bounds.h > max_row_bottom)
        {
            max_row_bottom = c->bounds.y + c->bounds.h;
        }
        // position: relative -- mirror the behaviour in
        // scene__layout_children_as_column / _row: keep the child IN
        // FLOW (don't update max_row_bottom against the shifted
        // position, sibling columns stay where they were), but apply
        // the inset_l/r/t/b shift to the rendered subtree. Lets a
        // runtime reassert mutate `inset_t` on a tile to drive
        // entrance / wobble animations without breaking the grid
        // layout for non-animated siblings.
        scene__apply_position_offset(n, c, cell_w, (cell_h > 0.0f) ? cell_h : row_h);
        col_idx++;
        if (col_idx >= cols)
        {
            col_idx = 0;
            row_y += ((cell_h > 0.0f) ? cell_h : row_h) + gap;
            row_h = 0.0f;
        }
    }

    // Auto-grow height to content if size_h wasn't pinned. The
    // bounds were set by the caller before layout runs; we expand
    // only when size_h == 0. border_w is added so the bottom border
    // sits past the last row, mirroring the top-side `+ border_w`
    // applied to y0.
    // an explicit or percent height stays; only an auto height follows the content
    if (s->size_h <= 0.0f && s->height_pct <= 0.0f)
    {
        n->bounds.h = (max_row_bottom - n->bounds.y) + pad_b + border_w;
    }
}

static void _collection_internal__layout_list(gui_node_t* n, float scale)
{
    gui_style_t* s = &n->resolved;
    float pad_t   = s->pad_t * scale;
    float pad_l   = s->pad_l * scale;
    float pad_r   = s->pad_r * scale;
    float gap     = s->gap * scale;
    // Border-aware inner height + child origin. See widget_collection's
    // grid path comment for the full rationale.
    float bw      = scene__border_width(n, scale);
    float inner_h = n->bounds.h - pad_t - s->pad_b * scale - 2.0f * bw;
    if (inner_h < 0.0f)
    {
        inner_h = 0.0f;
    }

    float cell_w = (s->item_width  > 0.0f) ? s->item_width  * scale : 0.0f;
    float cell_h = (s->item_height > 0.0f) ? s->item_height * scale : inner_h;

    float cx = n->bounds.x + pad_l + bw;
    float cy = n->bounds.y + pad_t + bw;

    for (gui_node_t* c = n->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        if (c->resolved.display == GUI_DISPLAY__NONE)
        {
            continue;
        }
        float w = (cell_w > 0.0f) ? cell_w : (c->resolved.size_w > 0.0f ? c->resolved.size_w * scale : 80.0f);
        scene__layout_node(c, cx, cy, w, cell_h);
        cx += w + gap;
    }

    // Right pad so content doesn't abut the edge after a horizontal
    // scroll. Width isn't auto-grown because lists are usually sized
    // by the parent; the list itself scrolls horizontally if content
    // overflows (pending overflow-x support).
    (void)pad_r;
}

static void _collection_internal__layout_flow(gui_node_t* n, float scale)
{
    gui_style_t* s = &n->resolved;
    float pad_t   = s->pad_t * scale;
    float pad_r   = s->pad_r * scale;
    float pad_b   = s->pad_b * scale;
    float pad_l   = s->pad_l * scale;
    float gap     = s->gap * scale;
    // Border-aware inner width + child origin. See widget_collection's
    // grid path comment for the full rationale.
    float bw      = scene__border_width(n, scale);
    float inner_w = n->bounds.w - (pad_l + pad_r) - 2.0f * bw;
    if (inner_w < 0.0f)
    {
        inner_w = 0.0f;
    }

    float cell_w = (s->item_width  > 0.0f) ? s->item_width  * scale : 0.0f;
    float cell_h = (s->item_height > 0.0f) ? s->item_height * scale : 0.0f;

    float x0 = n->bounds.x + pad_l + bw;
    float y0 = n->bounds.y + pad_t + bw;

    float cursor_x = x0;
    float cursor_y = y0;
    float row_h    = 0.0f;
    float max_bottom = y0;

    for (gui_node_t* c = n->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        if (c->resolved.display == GUI_DISPLAY__NONE)
        {
            continue;
        }
        float w = (cell_w > 0.0f) ? cell_w : (c->resolved.size_w > 0.0f ? c->resolved.size_w * scale : 80.0f);
        float h = (cell_h > 0.0f) ? cell_h : (c->resolved.size_h > 0.0f ? c->resolved.size_h * scale : w);
        // Wrap: if placing this cell would exceed the right edge,
        // move to the next row. The first cell in a row always fits
        // (otherwise nothing would fit, and we'd loop forever).
        if (cursor_x > x0 && cursor_x + w > x0 + inner_w)
        {
            cursor_x = x0;
            cursor_y += row_h + gap;
            row_h = 0.0f;
        }
        scene__layout_node(c, cursor_x, cursor_y, w, h);
        if (h > row_h)
        {
            row_h = h;
        }
        cursor_x += w + gap;
        if (cursor_y + row_h > max_bottom)
        {
            max_bottom = cursor_y + row_h;
        }
    }

    if (s->size_h <= 0.0f && s->height_pct <= 0.0f)
    {
        n->bounds.h = (max_bottom - n->bounds.y) + pad_b + bw;
    }
}

static void collection_layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale)
{
    gui_style_t* s = &n->resolved;
    float w = scene__layout_width(n, avail_w, scale);
    float h = scene__layout_height(n, avail_h, scale);
    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;

    switch (s->collection_layout)
    {
        case GUI_COLLECTION_LAYOUT__LIST: _collection_internal__layout_list(n, scale); break;
        case GUI_COLLECTION_LAYOUT__FLOW: _collection_internal__layout_flow(n, scale); break;
        case GUI_COLLECTION_LAYOUT__GRID:
        default:                  _collection_internal__layout_grid(n, scale); break;
    }
}

static void collection_emit_draws(gui_node_t* n, float scale)
{
    // Standard bg + shadow + gradient treatment. Children draw on
    // top via scene's recursion after this hook returns.
    scene__emit_default_bg(n, scale);
}

static const widget_vtable_t g_collection_vtable = {
    .type_name       = "collection",
    .init_defaults   = NULL_POINTER,
    .apply_attribute = NULL_POINTER,
    .layout          = collection_layout,
    .emit_draws      = collection_emit_draws,
};

void widget_collection__register(void)
{
    widget_registry__register(GUI_NODE_TYPE__COLLECTION, &g_collection_vtable);
}
