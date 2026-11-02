#include "clib/stdlib.h"
#include "flex_layout.h"
#include "font.h"
#include "gui.h"
#include "scene.h"
#include "third_party/log.h"
#include "types.h"
#include "widget.h"
#include "widget_registry.h"

#define TABLE_COLUMNS 64
#define TABLE_ROWS 256
#define TABLE_CELLS 4096

typedef struct table_cell_t
{
    int colspan;          // 0x0
    int rowspan;          // 0x4
    float content_height; // 0x8
    boole collapsed;
} table_cell_t;

typedef struct table_slot_t
{
    gui_node_t* node; // 0x0
    int row;          // 0x8
    int column;       // 0xC
    int rows;         // 0x10
    int columns;      // 0x14
    float minimum;    // 0x18
    float maximum;    // 0x1C
} table_slot_t;

typedef struct table_grid_t
{
    table_slot_t cells[TABLE_CELLS]; // 0x0
    gui_node_t* rows[TABLE_ROWS];
    int occupied[TABLE_COLUMNS];
    boole constrained[TABLE_COLUMNS];
    float minimum[TABLE_COLUMNS];
    float maximum[TABLE_COLUMNS];
    float widths[TABLE_COLUMNS];
    float heights[TABLE_ROWS];
    float left[TABLE_COLUMNS];
    float top[TABLE_ROWS];
    int row_count;
    int column_count;
    int cell_count;
} table_grid_t;

static void _widget_table_internal__layout(gui_node_t* node, float x, float y, float width, float height, float scale);
static void _widget_table_internal__cell_layout(gui_node_t* node, float x, float y, float width, float height, float scale);
static void _widget_table_internal__cell_draw(gui_node_t* node, float scale);
static boole _widget_table_internal__attribute(gui_node_t* node, char* name, char* value);
static void _widget_table_internal__row_layout(gui_node_t* node, float x, float y, float width, float height, float scale);
static void _widget_table_internal__measure(table_slot_t* slot, float scale);
static float _widget_table_internal__sum(float* values, int start, int count, float gap);
static void _widget_table_internal__grow(float* values, int start, int count, float target, float gap);

/**
 * @brief sum consecutive tracks including spacing between them
 * @return float -> occupied extent
 */
static float _widget_table_internal__sum(float* values, int start, int count, float gap)
{
    int i = 0;
    float total = count > 0 ? (count - 1) * gap : 0.0f;
    for (i = 0; i < count; i++)
    {
        total += values[start + i];
    }
    return total;
}

/**
 * @brief distribute a spanning cell's minimum extent across its tracks
 * @return void
 */
static void _widget_table_internal__grow(float* values, int start, int count, float target, float gap)
{
    int i = 0;
    float difference = target - _widget_table_internal__sum(values, start, count, gap);
    if (difference <= 0.0f || count <= 0)
    {
        return;
    }
    for (i = 0; i < count; i++)
    {
        values[start + i] += difference / count;
    }
}

/**
 * @brief parse cell spans, rejecting invalid or excessive values
 * @return boole -> TRUE when this is a cell span attribute
 */
static boole _widget_table_internal__attribute(gui_node_t* node, char* name, char* value)
{
    table_cell_t* state = scene__widget_state(node);
    int span = 0;
    const char* cursor = value;
    if (stdlib__strcmp(name, "colspan") != 0 && stdlib__strcmp(name, "rowspan") != 0)
    {
        return FALSE;
    }
    while (*cursor >= '0' && *cursor <= '9' && span <= TABLE_ROWS)
    {
        span = span * 10 + (*cursor++ - '0');
    }
    if (*cursor != 0 || span <= 0 || span > (stdlib__strcmp(name, "colspan") == 0 ? TABLE_COLUMNS : TABLE_ROWS))
    {
        log_warn("%s %s", "invalid table span:", value);
        span = 1;
    }
    if (stdlib__strcmp(name, "colspan") == 0)
    {
        state->colspan = span;
    }
    else
    {
        state->rowspan = span;
    }
    return TRUE;
}

/**
 * @brief measure a cell using shared text wrapping or vertical child layout
 * @return void
 */
static void _widget_table_internal__cell_layout(gui_node_t* node, float x, float y, float width, float height, float scale)
{
    table_cell_t* state = scene__widget_state(node);
    const widget_vtable_t* text = widget_registry__get(GUI_NODE_TYPE__TEXT);
    float half = state->collapsed ? scene__border_width(node, scale) / (2 * scale) : 0;
    // border_width is snapped to physical pixels by the shared helpers.
    // Offset their box inset instead of turning a 1px stroke into .5px,
    // which the normal border helper would snap back to 1px.
    node->resolved.pad_l -= half;
    node->resolved.pad_r -= half;
    node->resolved.pad_t -= half;
    node->resolved.pad_b -= half;
    if (node->first_child == NULL_POINTER)
    {
        text->layout(node, x, y, width, 0.0f, scale);
    }
    else
    {
        flex_layout__column(node, x, y, width, height, scale);
    }
    state->content_height = node->bounds.h;
    node->resolved.pad_l += half;
    node->resolved.pad_r += half;
    node->resolved.pad_t += half;
    node->resolved.pad_b += half;
}

/**
 * @brief draw the cell background and vertically aligned text using the label renderer
 * @return void
 */
static void _widget_table_internal__cell_draw(gui_node_t* node, float scale)
{
    table_cell_t* state = scene__widget_state(node);
    const widget_vtable_t* text = widget_registry__get(GUI_NODE_TYPE__TEXT);
    float extra = node->bounds.h - state->content_height;
    float top = node->resolved.pad_t;
    float bottom = node->resolved.pad_b;
    gui_rect_t bounds = node->bounds;
    if (state->collapsed)
    {
        // Cell geometry contains half of each shared border. Paint the full
        // stroke across that edge; neighboring strokes overlap exactly.
        float half = scene__border_width(node, scale) * 0.5f;
        node->bounds.x -= half;
        node->bounds.y -= half;
        node->bounds.w += 2 * half;
        node->bounds.h += 2 * half;
    }
    if (node->first_child != NULL_POINTER)
    {
        scene__emit_default_bg(node, scale);
        node->bounds = bounds;
        return;
    }
    if (extra > 0.0f && node->resolved.table_vertical_align != 1)
    {
        node->resolved.pad_t += extra / scale * (node->resolved.table_vertical_align == 3 ? 1.0f : 0.5f);
        node->resolved.pad_b = bottom;
    }
    text->emit_draws(node, scale);
    node->resolved.pad_t = top;
    node->resolved.pad_b = bottom;
    node->bounds = bounds;
}

/**
 * @brief provide a harmless standalone row layout; tables assign real row geometry
 * @return void
 */
static void _widget_table_internal__row_layout(gui_node_t* node, float x, float y, float width, float height, float scale)
{
    flex_layout__row(node, x, y, width, height, scale);
}

/**
 * @brief measure intrinsic cell widths, respecting unbreakable words and padding
 * @return void
 */
static void _widget_table_internal__measure(table_slot_t* slot, float scale)
{
    gui_node_t* node = slot->node;
    gui_node_t* previous = font__set_node(node);
    gui_font_t* font = NULL_POINTER;
    const char* start = node->text;
    const char* cursor = start;
    float word = 0.0f;
    table_cell_t* state = scene__widget_state(node);
    float padding = (node->resolved.pad_l + node->resolved.pad_r) * scale + (state->collapsed ? 1.0f : 2.0f) * scene__border_width(node, scale);
    scene__measure_node(node, 0.0f, 0.0f, -1.0f, 0.0f);
    slot->maximum = node->bounds.w;
    slot->minimum = padding;
    if (node->first_child == NULL_POINTER)
    {
        font = font__at(node->resolved.font_family, (node->resolved.font_size > 0.0f ? node->resolved.font_size : 14.0f) * scale);
        while (*cursor != 0)
        {
            while (*cursor != 0 && *cursor != ' ' && *cursor != '\n' && *cursor != '\t')
            {
                cursor++;
            }
            word = font__measure_n(font, (char*)start, cursor - start) + padding;
            if (word > slot->minimum)
            {
                slot->minimum = word;
            }
            if (*cursor != 0)
            {
                cursor++;
            }
            start = cursor;
        }
    }
    else
    {
        scene__measure_node(node, 0.0f, 0.0f, 1.0f, 0.0f);
        slot->minimum = node->bounds.w;
    }
    if (node->resolved.size_w * scale > slot->minimum)
    {
        slot->minimum = node->resolved.size_w * scale;
    }
    if (slot->maximum < slot->minimum)
    {
        slot->maximum = slot->minimum;
    }
    font__set_node(previous);
}

/**
 * @brief lay out a shared table grid with intrinsic or fixed widths and spanning cells
 * @return void
 */
static void _widget_table_internal__layout(gui_node_t* node, float x, float y, float width, float height, float scale)
{
    table_grid_t* grid = scene__widget_state(node);
    table_cell_t* cell_state = NULL_POINTER;
    table_slot_t* slot = NULL_POINTER;
    gui_node_t* row = node->first_child;
    gui_node_t* cell = NULL_POINTER;
    gui_node_t* child = NULL_POINTER;
    int r = 0;
    int c = 0;
    int i = 0;
    int j = 0;
    int span = 0;
    float gap_x = (node->resolved.border_spacing_set == TRUE ? node->resolved.border_spacing_x : 2.0f) * scale;
    float gap_y = (node->resolved.border_spacing_set == TRUE ? node->resolved.border_spacing_y : 2.0f) * scale;
    float border = scene__border_width(node, scale);
    float minimum = 0.0f;
    float maximum = 0.0f;
    float requested = 0.0f;
    float available = 0.0f;
    float total = 0.0f;
    float extra = 0.0f;
    float fraction = 0.0f;
    float cell_width = 0.0f;
    float cell_height = 0.0f;
    float fixed_total = 0.0f;
    float outer_x = 0.0f;
    float outer_y = 0.0f;
    int unspecified = 0;
    boole collapse = (boole)(node->resolved.border_collapse == 2);
    boole fixed = (boole)(node->resolved.table_layout == 2 && (node->resolved.size_w > 0.0f || node->resolved.width_pct > 0.0f));
    (void)height;
    stdlib__null_memory(grid, sizeof(*grid));
    for (; row != NULL_POINTER && r < TABLE_ROWS; row = row->next_sibling)
    {
        if (row->type != GUI_NODE_TYPE__TR || row->resolved.display == GUI_DISPLAY__NONE)
        {
            continue;
        }
        grid->rows[r] = row;
        c = 0;
        for (cell = row->first_child; cell != NULL_POINTER; cell = cell->next_sibling)
        {
            if ((cell->type != GUI_NODE_TYPE__TD && cell->type != GUI_NODE_TYPE__TH) || cell->resolved.display == GUI_DISPLAY__NONE)
            {
                continue;
            }
            while (c < TABLE_COLUMNS && grid->occupied[c] > r)
            {
                c++;
            }
            if (c >= TABLE_COLUMNS || grid->cell_count >= TABLE_CELLS)
            {
                log_warn("%s", "table exceeds supported grid capacity");
                break;
            }
            cell_state = scene__widget_state(cell);
            cell_state->collapsed = collapse;
            span = cell_state->colspan > 0 ? cell_state->colspan : 1;
            if (span > TABLE_COLUMNS - c)
            {
                span = TABLE_COLUMNS - c;
            }
            slot = &grid->cells[grid->cell_count++];
            slot->node = cell;
            slot->row = r;
            slot->column = c;
            slot->columns = span;
            slot->rows = cell_state->rowspan > 0 ? cell_state->rowspan : 1;
            for (j = c; j < c + span; j++)
            {
                grid->occupied[j] = r + slot->rows;
            }
            c += span;
            if (c > grid->column_count)
            {
                grid->column_count = c;
            }
            if (collapse == TRUE && scene__border_width(cell, scale) > border)
            {
                border = scene__border_width(cell, scale);
            }
        }
        r++;
    }
    grid->row_count = r;
    if (collapse == TRUE)
    {
        gap_x = 0.0f;
        gap_y = 0.0f;
        outer_x = border * 0.5f;
        outer_y = border * 0.5f;
    }
    else
    {
        outer_x = gap_x + border + node->resolved.pad_l * scale;
        outer_y = gap_y + border + node->resolved.pad_t * scale;
    }
    for (i = 0; i < grid->cell_count; i++)
    {
        slot = &grid->cells[i];
        if (slot->rows > r - slot->row)
        {
            slot->rows = r - slot->row;
        }
        _widget_table_internal__measure(slot, scale);
        if (slot->columns == 1)
        {
            _widget_table_internal__grow(grid->minimum, slot->column, 1, slot->minimum, gap_x);
            _widget_table_internal__grow(grid->maximum, slot->column, 1, slot->maximum, gap_x);
            if (slot->node->resolved.size_w > 0 || slot->node->resolved.width_pct > 0)
            {
                grid->constrained[slot->column] = TRUE;
            }
        }
    }
    // Measure ordinary cells first. An early spanning header must not expand
    // an explicitly sized column that already fits its later body cells.
    for (i = 0; i < grid->cell_count; i++)
    {
        slot = &grid->cells[i];
        if (slot->columns > 1)
        {
            _widget_table_internal__grow(grid->minimum, slot->column, slot->columns, slot->minimum, gap_x);
            _widget_table_internal__grow(grid->maximum, slot->column, slot->columns, slot->maximum, gap_x);
        }
    }
    minimum = _widget_table_internal__sum(grid->minimum, 0, grid->column_count, gap_x);
    maximum = _widget_table_internal__sum(grid->maximum, 0, grid->column_count, gap_x);
    requested = node->resolved.size_w > 0.0f ? node->resolved.size_w * scale : (node->resolved.width_pct > 0.0f ? width * node->resolved.width_pct / 100.0f : 0.0f);
    available = requested > 0.0f ? requested - 2.0f * outer_x : (width > 0.0f ? width - 2.0f * outer_x : maximum);
    if (requested <= 0.0f && available > maximum)
    {
        available = maximum;
    }
    if (fixed == FALSE && available < minimum)
    {
        available = minimum;
    }
    if (available < 0.0f)
    {
        available = 0.0f;
    }
    for (c = 0; c < grid->column_count; c++)
    {
        fraction = maximum > minimum ? (available - minimum) / (maximum - minimum) : 0.0f;
        if (fraction > 1.0f)
        {
            fraction = 1.0f;
        }
        grid->widths[c] = grid->minimum[c] + (grid->maximum[c] - grid->minimum[c]) * fraction;
        if (fixed == TRUE)
        {
            grid->widths[c] = 0.0f;
        }
    }
    if (fixed == TRUE)
    {
        for (i = 0; i < grid->cell_count && grid->cells[i].row == 0; i++)
        {
            slot = &grid->cells[i];
            extra = slot->node->resolved.size_w * scale;
            if (slot->node->resolved.width_pct > 0.0f)
            {
                extra = available * slot->node->resolved.width_pct / 100.0f;
            }
            if (extra > 0.0f)
            {
                _widget_table_internal__grow(grid->widths, slot->column, slot->columns, extra, gap_x);
            }
        }
        fixed_total = _widget_table_internal__sum(grid->widths, 0, grid->column_count, gap_x);
        for (c = 0; c < grid->column_count; c++)
        {
            if (grid->widths[c] <= 0.0f)
            {
                unspecified++;
            }
        }
        for (c = 0; c < grid->column_count; c++)
        {
            if (grid->widths[c] <= 0.0f && unspecified > 0)
            {
                grid->widths[c] = (available - fixed_total) / unspecified;
            }
            if (grid->widths[c] < 0.0f)
            {
                grid->widths[c] = 0.0f;
            }
        }
    }
    if (!fixed)
    {
        float spare = available - _widget_table_internal__sum(grid->widths, 0, grid->column_count, gap_x);
        float weight = 0;
        int flexible = 0;
        for (c = 0; c < grid->column_count; c++)
        {
            if (!grid->constrained[c])
            {
                weight += grid->maximum[c];
                flexible++;
            }
        }
        if (flexible > 0 && spare > 0)
        {
            for (c = 0; c < grid->column_count; c++)
            {
                if (!grid->constrained[c])
                {
                    grid->widths[c] += spare * (weight > 0 ? grid->maximum[c] / weight : 1.0f / flexible);
                }
            }
        }
    }
    _widget_table_internal__grow(grid->widths, 0, grid->column_count, available, gap_x);
    total = _widget_table_internal__sum(grid->widths, 0, grid->column_count, gap_x);
    for (i = 0; i < grid->cell_count; i++)
    {
        slot = &grid->cells[i];
        cell = slot->node;
        cell_width = _widget_table_internal__sum(grid->widths, slot->column, slot->columns, gap_x);
        extra = cell->resolved.size_w;
        fraction = cell->resolved.width_pct;
        cell->resolved.size_w = cell_width / scale;
        cell->resolved.width_pct = 0.0f;
        scene__layout_node(cell, 0.0f, 0.0f, cell_width, 0.0f);
        cell->resolved.size_w = extra;
        cell->resolved.width_pct = fraction;
        _widget_table_internal__grow(grid->heights, slot->row, slot->rows, cell->bounds.h, gap_y);
    }
    for (c = 0; c < grid->column_count; c++)
    {
        grid->left[c] = x + outer_x + (c > 0 ? _widget_table_internal__sum(grid->widths, 0, c, gap_x) + gap_x : 0.0f);
    }
    for (r = 0; r < grid->row_count; r++)
    {
        grid->top[r] = y + outer_y + (r > 0 ? _widget_table_internal__sum(grid->heights, 0, r, gap_y) + gap_y : 0.0f);
        grid->rows[r]->bounds = (gui_rect_t){x + outer_x, grid->top[r], total, grid->heights[r]};
    }
    for (i = 0; i < grid->cell_count; i++)
    {
        slot = &grid->cells[i];
        cell = slot->node;
        cell_height = _widget_table_internal__sum(grid->heights, slot->row, slot->rows, gap_y);
        extra = cell_height - cell->bounds.h;
        scene__shift_tree(cell, grid->left[slot->column] - cell->bounds.x, grid->top[slot->row] - cell->bounds.y);
        cell->bounds.h = cell_height;
        if (extra > 0.0f && cell->resolved.table_vertical_align != 1)
        {
            for (child = cell->first_child; child != NULL_POINTER; child = child->next_sibling)
            {
                scene__shift_tree(child, 0.0f, extra * (cell->resolved.table_vertical_align == 3 ? 1.0f : 0.5f));
            }
        }
    }
    node->bounds = (gui_rect_t){x, y, total + 2.0f * outer_x, _widget_table_internal__sum(grid->heights, 0, grid->row_count, gap_y) + 2.0f * outer_y};
}

static const widget_vtable_t g_table_vtable = {.type_name = "table", .layout = _widget_table_internal__layout, .state_size = sizeof(table_grid_t)};
static const widget_vtable_t g_tr_vtable = {.type_name = "tr", .layout = _widget_table_internal__row_layout};
static const widget_vtable_t g_td_vtable = {.type_name = "td", .layout = _widget_table_internal__cell_layout, .emit_draws = _widget_table_internal__cell_draw, .apply_attribute = _widget_table_internal__attribute, .state_size = sizeof(table_cell_t)};
static const widget_vtable_t g_th_vtable = {.type_name = "th", .layout = _widget_table_internal__cell_layout, .emit_draws = _widget_table_internal__cell_draw, .apply_attribute = _widget_table_internal__attribute, .state_size = sizeof(table_cell_t)};

/**
 * @brief register the four HTML table elements
 * @return void
 */
void widget_table__register(void)
{
    widget_registry__register(GUI_NODE_TYPE__TABLE, &g_table_vtable);
    widget_registry__register(GUI_NODE_TYPE__TR, &g_tr_vtable);
    widget_registry__register(GUI_NODE_TYPE__TD, &g_td_vtable);
    widget_registry__register(GUI_NODE_TYPE__TH, &g_th_vtable);
}
