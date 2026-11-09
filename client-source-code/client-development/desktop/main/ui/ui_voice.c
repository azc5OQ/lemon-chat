#include "ui/ui_voice.h"
#include "ui/ui_headers.h"
#include "widget_registry.h"
#include "renderer.h"

static widget_vtable_t g_voice_original;
static widget_vtable_t g_voice_widget;
static void *g_voice_textures[7][2];
static boole g_voice_prepared[7];

// declarations
static void _ui_voice_internal__draw(gui_node_t *node, float scale);
static boole _ui_voice_internal__texture(int64 state);
static float _ui_voice_internal__line(float x, float y, float ax, float ay, float bx, float by, float width);
static float _ui_voice_internal__box(float x, float y, float width, float height, float radius);
static float _ui_voice_internal__glyph(float x, float y, int64 state);
static void _ui_voice_internal__paint(gui_node_t *node, int64 state);

/**
 * @brief measures a rounded line in the fixed logical icon canvas.
 * @param float x -> sample x
 * @param float y -> sample y
 * @param float ax -> first endpoint x
 * @param float ay -> first endpoint y
 * @param float bx -> second endpoint x
 * @param float by -> second endpoint y
 * @param float width -> full stroke width
 * @return float -> signed distance from the stroke edge
 */
static float _ui_voice_internal__line(float x, float y, float ax, float ay, float bx, float by, float width)
{
    float dx = bx - ax;
    float dy = by - ay;
    float length = dx * dx + dy * dy;
    float along = length > 0 ? clib__fmaxf(0, clib__fminf(1, ((x - ax) * dx + (y - ay) * dy) / length)) : 0;
    dx = x - ax - along * dx;
    dy = y - ay - along * dy;
    return clib__sqrtf(dx * dx + dy * dy) - width * 0.5f;
}

/**
 * @brief measures a rounded rectangle without depending on tessellated small geometry.
 * @param float x -> sample x relative to center
 * @param float y -> sample y relative to center
 * @param float width -> half width
 * @param float height -> half height
 * @param float radius -> corner radius
 * @return float -> signed distance from the edge
 */
static float _ui_voice_internal__box(float x, float y, float width, float height, float radius)
{
    float dx = clib__fabsf(x) - width + radius;
    float dy = clib__fabsf(y) - height + radius;
    float outside_x = clib__fmaxf(dx, 0);
    float outside_y = clib__fmaxf(dy, 0);
    return clib__sqrtf(outside_x * outside_x + outside_y * outside_y) + clib__fminf(clib__fmaxf(dx, dy), 0) - radius;
}

/**
 * @brief places small audio badges near the orb edge and retains the separate channel symbol.
 * @param float x -> sample x relative to center
 * @param float y -> sample y relative to center
 * @param int64 state -> cached audio state, or six for the channel hash
 * @return float -> signed glyph distance
 */
static float _ui_voice_internal__glyph(float x, float y, int64 state)
{
    float distance = 32;

    if (state == 3)
    {
        distance = _ui_voice_internal__line(x, y, 3.1f, 5.9f, 5.9f, 3.1f, 1.2f);
    }
    else if (state == 4)
    {
        distance = _ui_voice_internal__line(x, y, 3.3f, 2.9f, 3.3f, 6.1f, 1.15f);
        distance = clib__fminf(distance, _ui_voice_internal__line(x, y, 5.7f, 2.9f, 5.7f, 6.1f, 1.15f));
    }
    else if (state == 5)
    {
        distance = _ui_voice_internal__line(x, y, 2.9f, 4.5f, 6.1f, 4.5f, 1.3f);
    }
    else if (state == 6)
    {
        distance = _ui_voice_internal__line(x, y, -1.7f, -4.6f, -2.6f, 2.4f, 1.25f);
        distance = clib__fminf(distance, _ui_voice_internal__line(x, y, 2.2f, -4.6f, 1.3f, 2.4f, 1.25f));
        distance = clib__fminf(distance, _ui_voice_internal__line(x, y, -4.2f, -2.3f, 4.2f, -2.3f, 1.25f));
        distance = clib__fminf(distance, _ui_voice_internal__line(x, y, -4.2f, 0.6f, 4.2f, 0.6f, 1.25f));
    }
    return distance;
}

/**
 * @brief prepares bounded gradient and detail masks once, shared by every row and theme.
 * @param int64 state -> zero through five for audio, six for the channel symbol
 * @return boole -> TRUE when both owned textures are ready
 */
static boole _ui_voice_internal__texture(int64 state)
{
    ubyte pixels[2][64 * 64 * 4] = { 0 };
    int64 x = 0;
    int64 y = 0;
    int64 offset = 0;
    float px = 0;
    float py = 0;
    float distance = 0;
    float edge = 0;
    float shade = 0;
    float glyph = 0;
    float highlight = 0;
    float alpha = 0;
    float surface = 0;
    float badge = 0;
    if (g_voice_prepared[state])
    {
        return g_voice_textures[state][0] && g_voice_textures[state][1];
    }
    g_voice_prepared[state] = TRUE;
    for (y = 0; y < 64; y++)
    {
        for (x = 0; x < 64; x++)
        {
            px = ((float)x + 0.5f) * (24.0f / 64) - 12;
            py = ((float)y + 0.5f) * (24.0f / 64) - 12;
            distance = clib__sqrtf(px * px + py * py) - 9.6f;
            if (state == 6)
            {
                distance = _ui_voice_internal__box(px, py + 1.2f, 9.5f, 7.1f, 3);
                edge = clib__fmaxf(clib__fmaxf(-5.8f - px, px + 0.8f), clib__fmaxf(3.3f - py, py + 0.76f * px - 4.1f));
                distance = clib__fminf(distance, edge);
            }
            alpha = clib__fmaxf(0, clib__fminf(1, 0.5f - distance * (64.0f / 24)));
            edge = clib__fmaxf(0, clib__fminf(1, -distance - 0.25f));
            shade = 0.62f + edge * (0.24f - py * 0.018f);
            glyph = clib__fmaxf(0, clib__fminf(1, 0.5f - _ui_voice_internal__glyph(px, py, state) * (64.0f / 24)));
            highlight = clib__fmaxf(0, clib__fminf(1, (-py - 1.5f) / 8)) * 0.27f * edge;
            if (state < 6)
            {
                // Shade the sphere
                surface = clib__sqrtf(clib__fmaxf(0, 1 - (px * px + py * py) / (9.6f * 9.6f)));
                shade = clib__fmaxf(0.32f, clib__fminf(1, 0.38f + surface * 0.56f - px * 0.012f - py * 0.018f));
                highlight = clib__fmaxf(0, 1 - ((px + 3.1f) * (px + 3.1f) / 24 + (py + 3.8f) * (py + 3.8f) / 13));
                highlight = highlight * highlight * 0.72f * edge;

                // Separate the badge
                if (state >= 3)
                {
                    badge = clib__sqrtf((px - 4.5f) * (px - 4.5f) + (py - 4.5f) * (py - 4.5f)) - 3.2f;
                    badge = clib__fmaxf(0, clib__fminf(1, 0.5f - badge * (64.0f / 24)));
                    shade *= 1 - badge * 0.42f;
                    highlight *= 1 - badge;
                }
            }
            if (state == 0)
            {
                alpha *= 1 - edge * 0.84f;
                highlight *= 0.4f;
            }
            if (state == 6)
            {
                alpha *= clib__fmaxf(glyph, 1 - edge * 0.83f);
                glyph = 0;
                highlight *= 0.5f;
            }
            offset = (y * 64 + x) * 4;
            pixels[0][offset] = pixels[0][offset + 1] = pixels[0][offset + 2] = (ubyte)(clib__fminf(1, shade) * 255 + 0.5f);
            pixels[0][offset + 3] = (ubyte)(alpha * 255 + 0.5f);
            pixels[1][offset] = pixels[1][offset + 1] = pixels[1][offset + 2] = 255;
            pixels[1][offset + 3] = (ubyte)(clib__fmaxf(glyph, highlight) * alpha * 255 + 0.5f);
        }
    }
    g_voice_textures[state][0] = renderer__create_texture_rgba(pixels[0], 64, 64);
    g_voice_textures[state][1] = renderer__create_texture_rgba(pixels[1], 64, 64);
    clib__null_memory(pixels, sizeof(pixels));
    return g_voice_textures[state][0] && g_voice_textures[state][1];
}

/**
 * @brief paints the shaded theme colour, highlight and small state badge from cached textures.
 * @param gui_node_t* node -> scene-owned audio indicator
 * @param int64 state -> validated cached icon state
 * @return void
 */
static void _ui_voice_internal__paint(gui_node_t *node, int64 state)
{
    gui_rect_t rect = { 0 };
    gui_color_t detail = { 1, 1, 1, node->resolved.font_color.a };
    rect.w = rect.h = clib__fminf(node->bounds.w, node->bounds.h);
    rect.x = node->bounds.x + (node->bounds.w - rect.w) * 0.5f;
    rect.y = node->bounds.y + (node->bounds.h - rect.h) * 0.5f;
    if (rect.w <= 0)
    {
        return;
    }
    if (_ui_voice_internal__texture(state))
    {
        renderer__submit_image(rect, g_voice_textures[state][0], node->resolved.font_color);
        renderer__submit_image(rect, g_voice_textures[state][1], detail);
    }
}

/**
 * @brief draws the matching channel symbol using the same bounded, antialiased texture cache.
 * @param gui_node_t* node -> scene-owned channel decoration
 * @return void
 */
void desktop_ui_voice__draw_channel(gui_node_t *node)
{
    if (node)
    {
        if (node->resolved.has_background_image)
        {
            scene__emit_default_bg(node, scene__scale());
            return;
        }
        _ui_voice_internal__paint(node, 6);
    }
}

/**
 * @brief resolves the audio state without allocating or animating per client.
 * @param gui_node_t* node -> scene-owned audio indicator or ordinary text
 * @param float scale -> display scale
 * @return void
 */
static void _ui_voice_internal__draw(gui_node_t *node, float scale)
{
    int64 state = (int64)node->value;
    if (clib__strncmp(node->klass, "voice-icon", 10) != 0)
    {
        if (g_voice_original.emit_draws)
        {
            g_voice_original.emit_draws(node, scale);
        }
        return;
    }
    if (node->resolved.has_background_image)
    {
        scene__emit_default_bg(node, scale);
    }
    else
    {
        _ui_voice_internal__paint(node, state >= 0 && state <= 5 ? state : 0);
    }
}

/**
 * @brief releases owned state masks while the renderer remains alive.
 * @return void
 */
void desktop_ui_voice__shutdown(void)
{
    int64 index = 0;
    int64 layer = 0;
    for (index = 0; index < 7; index++)
    {
        for (layer = 0; layer < 2; layer++)
        {
            if (g_voice_textures[index][layer])
            {
                renderer__destroy_texture(g_voice_textures[index][layer]);
                g_voice_textures[index][layer] = NULL;
            }
        }
        g_voice_prepared[index] = FALSE;
    }
}

/**
 * @brief registers application graphics through Meek UI's public widget API, without changing library code.
 * @return void
 */
void desktop_ui_voice__register(void)
{
    const widget_vtable_t *original = widget_registry__get(GUI_NODE_TYPE__TEXT);
    if (!original || original == &g_voice_widget)
    {
        return;
    }
    g_voice_original = *original;
    g_voice_widget = *original;
    g_voice_widget.emit_draws = _ui_voice_internal__draw;
    widget_registry__register(GUI_NODE_TYPE__TEXT, &g_voice_widget);
}

/**
 * @brief appends a fixed-size graphical audio indicator, leaving row clicks to its parent.
 * @param gui_node_t* parent -> scene-owned member row
 * @param int64 audio -> protocol state: 1 speaking, 2 ready, 3 microphone off, 4 deafened
 * @param boole muted -> TRUE when this listener has locally muted the member
 * @return void
 */
void desktop_ui_voice__append(gui_node_t *parent, int64 audio, boole muted)
{
    gui_node_t *icon = NULL;
    const char *klass = muted ? "voice-icon-muted" : audio == 1 ? "voice-icon-active" : audio >= 3 ? "voice-icon-disabled" : "voice-icon";
    if (!parent)
    {
        return;
    }
    icon = scene__node_new(GUI_NODE_TYPE__TEXT);
    if (!icon)
    {
        return;
    }
    icon->value = muted ? 5 : audio >= 1 && audio <= 4 ? (float)audio : 0;
    clib__string_format(icon->klass, sizeof(icon->klass), "%s voice-state-%lld", klass, (int64)icon->value);
    scene__add_child(parent, icon);
}
