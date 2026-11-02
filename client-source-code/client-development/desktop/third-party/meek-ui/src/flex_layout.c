#include "types.h"
#include "gui.h"
#include "scene.h"
#include "flex_layout.h"

/**
 * @brief identify a horizontal flex container for intrinsic sizing
 * @param gui_node_t* node -> potential parent
 * @return boole -> TRUE for a horizontal flex container
 */
boole flex_layout__is_row(gui_node_t* node)
{
    return (boole)(node != NULL_POINTER && node->resolved.display == GUI_DISPLAY__FLEX && node->resolved.flex_direction != 2);
}

/**
 * @brief lay out flex children using the established row sizing and placement rules
 * @param gui_node_t* n -> flex container
 * @param float x -> left edge
 * @param float y -> top edge
 * @param float avail_w -> available width
 * @param float avail_h -> available height
 * @param float scale -> UI pixel scale
 * @return void
 */
void flex_layout__row(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale)
{
    (void)avail_h;
    gui_style_t* s = &n->resolved;

    // Speculative measure pass. Each child lays out at (0,0) with
    // the full inner-width so that auto-sized children (Text widgets,
    // checkbox defaults) report their natural size in bounds.w/.h.
    // Sum widths across siblings + gaps for the row's own width;
    // max height for the row's own height.
    // Border eats from BOTH sides of each axis. Subtract 2*bw from the
    // measure-pass child width so children probe into the inner area
    // (matches the real placement done by scene__layout_children_as_row,
    // which applies the same shrink). bw is 0 for unbordered rows.
    // A row hugs its children inside another row or when the parent asks
    // for its max-content width (avail_w < 0); then the children are
    // measured unconstrained (-1) so nested containers report their
    // content width. Otherwise it spans the space offered.
    boole hug         = (boole)(s->size_w <= 0.0f && s->width_pct <= 0.0f && (avail_w < 0.0f || (n->parent != NULL_POINTER && flex_layout__is_row(n->parent) == TRUE)));
    float bw          = scene__border_width(n, scale);
    float candidate_w = (hug == TRUE) ? 0.0f : scene__layout_width(n, avail_w, scale);
    float inner_w     = (hug == TRUE) ? -1.0f : candidate_w - (s->pad_l + s->pad_r) * scale - 2.0f * bw;
    if (hug == FALSE && inner_w < 0.0f)
    {
        inner_w = 0.0f;
    }
    // For the speculative pass we don't yet know the row's height, so
    // pass the parent-supplied avail_w (minus padding) as height hint
    // too -- most children ignore avail_h anyway (Text auto-sizes to
    // line_height, checkbox to 20x20). Children that DO want to fill
    // height get a reasonable upper bound.
    float child_total_w = 0.0f;
    float child_max_h   = 0.0f;
    int64 child_index   = 0;
    {
        gui_node_t* c = n->first_child;
        while (c != NULL_POINTER)
        {
            // display: none children contribute 0 to both width and
            // height (including the gap that would follow them), so
            // the row collapses around the gap exactly as CSS does.
            if (c->resolved.display == GUI_DISPLAY__NONE)
            {
                c = c->next_sibling;
                continue;
            }
            // absolute and fixed children are out of flow: they add
            // neither width, height nor a gap to the auto-sized row (CSS)
            if (c->resolved.position == GUI_POSITION__ABSOLUTE || c->resolved.position == GUI_POSITION__FIXED)
            {
                c = c->next_sibling;
                continue;
            }
            scene__measure_node(c, 0.0f, 0.0f, inner_w, 0.0f);
            child_total_w += c->bounds.w;
            if (child_index > 0)
            {
                child_total_w += s->gap * scale;
            }
            if (c->bounds.h > child_max_h)
            {
                child_max_h = c->bounds.h;
            }
            child_index++;
            c = c->next_sibling;
        }
    }

    // Final sizing. size_w / size_h pins override the measured
    // values; otherwise the width follows the CSS rule for a flex
    // container with an auto width: inside a column (or any container
    // that is not a row) the row spans the parent's inner width, which
    // is what lets horizontal-alignment place the strip inside it;
    // inside another row it hugs its children (summed width + padding,
    // bounded by avail_w). Height is the tallest child + padding.
    // Auto-sizing adds 2*bw on each axis so the border lines have
    // room outside the children, matching the inner-area shrink the
    // placement helper applies.
    // Width priority: explicit pixel (size_w) > percent of parent
    // (width_pct) > the auto rule above.
    float w;
    if (s->size_w > 0.0f)
    {
        w = s->size_w * scale;
    }
    else if (s->width_pct > 0.0f)
    {
        w = (avail_w > 0.0f ? avail_w : 0.0f) * (s->width_pct / 100.0f);
    }
    else if (hug == TRUE)
    {
        w = child_total_w + (s->pad_l + s->pad_r) * scale + 2.0f * bw;
        if (avail_w > 0.0f && w > avail_w)
        {
            w = avail_w;
        }
    }
    else
    {
        w = avail_w;
    }

    // Final height: pixel > percent-of-avail > auto.
    float h;
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
        h = child_max_h + (s->pad_t + s->pad_b) * scale + 2.0f * bw;
    }

    scene__apply_min_max(n, &w, &h, scale);

    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;

    // Real placement pass. The shared helper walks children and lays
    // them out at (cursor_x, cy) with cursor_x advancing by child.w +
    // gap. Same math pattern as scene__layout_children_as_column,
    // just transposed.
    scene__layout_children_as_row(n);
}


/**
 * @brief lay out flex children using the established column sizing and placement rules
 * @param gui_node_t* n -> flex container
 * @param float x -> left edge
 * @param float y -> top edge
 * @param float avail_w -> available width
 * @param float avail_h -> available height
 * @param float scale -> UI pixel scale
 * @return void
 */
void flex_layout__column(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale)
{
    gui_style_t* s = &n->resolved;

    // Width follows the CSS rule for a flex item with an auto width:
    // inside a column the column spans the parent (scene__layout_width's
    // fallback); inside a row, or when the parent asks for the
    // max-content width (avail_w < 0), it hugs its widest child. Explicit
    // pixel and percent widths win. A hugging column measures its
    // children unconstrained (avail_w -1) so nested rows and divs report
    // their content width instead of filling the space offered.
    boole hug = (boole)(s->size_w <= 0.0f && s->width_pct <= 0.0f && (avail_w < 0.0f || (n->parent != NULL_POINTER && flex_layout__is_row(n->parent) == TRUE)));
    float w   = (hug == TRUE) ? 0.0f : scene__layout_width(n, avail_w, scale);

    // Border eats from BOTH sides of each axis. Subtract 2*bw from the
    // measure-pass child width so children probe into the inner area
    // (matches the real placement done by scene__layout_children_as_column,
    // which applies the same shrink). bw is 0 for unbordered columns.
    float bw = scene__border_width(n, scale);
    // the placement pass offers the column's height to its children
    // only when that height is definite (set by the style): a hugging
    // column's height is its children's, and offering it back would let
    // a div grow the column that grows the div (the stress page's hero
    // ended up as tall as the whole page that way)
    boole definite_h = FALSE;
    float place_cx   = 0.0f;
    float place_cy   = 0.0f;
    float place_cw   = 0.0f;
    float place_ch   = 0.0f;

    // speculative measure pass: lay each child out at (0,0) just to
    // read bounds.h (and bounds.w, for the hug case below). real
    // positioning happens via scene__layout_children_as_column below.
    float child_total = 0.0f;
    float child_max_w = 0.0f;
    int64 child_index = 0;
    {
        gui_node_t* c           = n->first_child;
        float     child_avail = (hug == TRUE) ? -1.0f : w - (s->pad_l + s->pad_r) * scale - 2.0f * bw;
        if (hug == FALSE && child_avail < 0.0f)
        {
            child_avail = 0.0f;
        }
        while (c != NULL_POINTER)
        {
            // display: none children take no measured height and no
            // inter-child gap: they must be invisible to the
            // auto-sizing column.
            if (c->resolved.display == GUI_DISPLAY__NONE)
            {
                c = c->next_sibling;
                continue;
            }
            // absolute and fixed children are out of flow: they add
            // neither height nor a gap to the auto-sized column (CSS)
            if (c->resolved.position == GUI_POSITION__ABSOLUTE || c->resolved.position == GUI_POSITION__FIXED)
            {
                c = c->next_sibling;
                continue;
            }
            scene__measure_node(c, 0.0f, 0.0f, child_avail, 0.0f);
            child_total += c->bounds.h;
            if (c->bounds.w + (c->resolved.margin_l + c->resolved.margin_r) * scale > child_max_w)
            {
                child_max_w = c->bounds.w + (c->resolved.margin_l + c->resolved.margin_r) * scale;
            }
            if (child_index > 0)
            {
                child_total += s->gap * scale;
            }
            child_index++;
            c = c->next_sibling;
        }
    }

    if (hug == TRUE)
    {
        w = child_max_w + (s->pad_l + s->pad_r) * scale + 2.0f * bw;
        if (avail_w > 0.0f && w > avail_w)
        {
            w = avail_w;
        }
    }

    // Final height priority: explicit pixel (size_h) > percent of
    // parent (height_pct) > auto-size to children. Without the
    // height_pct branch, a column that declares `height: 100%`
    // silently collapses to the sum of its children -- which
    // breaks "fullscreen overlay" patterns where the column should
    // fill the remaining space even if its children have auto /
    // percent heights of their own (infinite recursion would
    // otherwise loop through scene__layout_children_as_column).
    // Auto-height adds 2*bw so the top/bottom border lines have
    // room outside the children.
    float h;
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
        h = child_total + (s->pad_t + s->pad_b) * scale + 2.0f * bw;
    }

    scene__apply_min_max(n, &w, &h, scale);

    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;
    definite_h = (boole)(s->size_h > 0.0f || (s->height_pct > 0.0f && avail_h > 0.0f));
    place_cx   = x + s->pad_l * scale + bw;
    place_cy   = y + s->pad_t * scale + bw;
    place_cw   = w - (s->pad_l + s->pad_r) * scale - 2.0f * bw;
    place_ch   = definite_h ? h - (s->pad_t + s->pad_b) * scale - 2.0f * bw : 0.0f;
    if (place_cw < 0.0f)
    {
        place_cw = 0.0f;
    }
    if (place_ch < 0.0f)
    {
        place_ch = 0.0f;
    }
    scene__layout_column_flow(n, place_cx, place_cy, place_cw, place_ch, 0.0f, 0.0f, NULL_POINTER, NULL_POINTER);
}

