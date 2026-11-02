#include "ui/ui_effects.h"
#include "ui/ui_headers.h"
#include "widget_registry.h"

static const gui_node_type_e g_effect_types[4] =
{
    GUI_NODE_TYPE__DIV, GUI_NODE_TYPE__BUTTON, GUI_NODE_TYPE__INPUT, GUI_NODE_TYPE__SELECT
};
static widget_vtable_t g_effect_original[4];
static widget_vtable_t g_effect_widgets[4];
static boole g_effect_registered;

// declarations
static void _ui_effects_internal__draw(gui_node_t* node, float scale);

/**
 * @brief positions a theme's radial border highlight at the pointer without allocations or a background timer.
 * @param gui_node_t* node -> borrowed GUI node; only resolved drawing properties are changed
 * @param float scale -> physical pixels per logical unit
 * @return void
 */
void desktop_ui_effects__border(gui_node_t* node, float scale)
{
    gui_gradient_t* gradient = &node->resolved.border_gradient;
    int64 x = 0;
    int64 y = 0;
    if (!node->resolved.has_border_gradient || gradient->kind != GUI_GRADIENT_KIND__RADIAL || gradient->stop_count < 2 || scale <= 0)
    {
        return;
    }
    if (!scene__pointer_position(&x, &y))
    {
        gradient->stops[0].color = gradient->stops[gradient->stop_count - 1].color;
        return;
    }
    gradient->center_x = ((float)x - node->bounds.x) / scale;
    gradient->center_y = ((float)y - node->bounds.y) / scale;
    gradient->center_x_px = TRUE;
    gradient->center_y_px = TRUE;
    gradient->circle = TRUE;
    if (gradient->size != GUI_GRADIENT_SIZE__EXPLICIT)
    {
        gradient->size = GUI_GRADIENT_SIZE__EXPLICIT;
        gradient->radius_x = 150;
        gradient->radius_y = 150;
    }
}

/**
 * @brief adds pointer border lighting while retaining upstream widget drawing and popup behavior.
 * @param gui_node_t* node -> scene-owned control
 * @param float scale -> physical pixels per logical unit
 * @return void
 */
static void _ui_effects_internal__draw(gui_node_t* node, float scale)
{
    int64 index = 0;
    desktop_ui_effects__border(node, scale);
    for (index = 0; index < 4; index++)
    {
        if (node->type == g_effect_types[index] && g_effect_original[index].emit_draws)
        {
            g_effect_original[index].emit_draws(node, scale);
            break;
        }
    }
}

/**
 * @brief wraps existing public widget drawing hooks on the GUI thread; themes opt in with radial border colours.
 * @return void
 */
void desktop_ui_effects__register(void)
{
    const widget_vtable_t* original = NULL;
    int64 index = 0;
    if (g_effect_registered)
    {
        return;
    }
    for (index = 0; index < 4; index++)
    {
        original = widget_registry__get(g_effect_types[index]);
        if (original && original->emit_draws)
        {
            g_effect_original[index] = *original;
            g_effect_widgets[index] = *original;
            g_effect_widgets[index].emit_draws = _ui_effects_internal__draw;
            widget_registry__register(g_effect_types[index], &g_effect_widgets[index]);
        }
    }
    g_effect_registered = TRUE;
}
