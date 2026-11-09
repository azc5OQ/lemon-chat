#include "ui/ui_headers.h"
#include "ui/ui_attachments.h"
#include "widget_registry.h"
#include "renderer.h"
#include "mbedtls/platform_util.h"
#include <stdlib.h>

typedef struct desktop_image_texture_t
{
    uint64 id;
    void *texture;
    int64 width;
    int64 height;
    uint64 bytes;
} desktop_image_texture_t;

static desktop_image_texture_t g_images[DESKTOP_ATTACHMENT_COUNT];
static desktop_attachments_t *g_image_attachments;
static desktop_chat_preferences_t *g_image_preferences;
static widget_vtable_t g_image_original;
static widget_vtable_t g_image_widget;

// declarations
static void _ui_images_internal__draw(gui_node_t *node, float scale);
static void _ui_images_internal__layout(gui_node_t *node, float x, float y, float width, float height, float scale);

/**
 * @brief computes a bounded aspect-correct image box using only GUI-owned texture metadata.
 * @param gui_node_t* node -> scene-owned label carrying a cache index
 * @param float x -> allocated origin
 * @param float y -> allocated origin
 * @param float width -> available physical width, negative during measurement
 * @param float height -> available physical height
 * @param float scale -> device display scale
 * @return void
 */
static void _ui_images_internal__layout(gui_node_t *node, float x, float y, float width, float height, float scale)
{
    int64 index = (int64)(nuint)node->user_data - 1;
    float ratio = 1;
    float maximum_w = 0;
    float maximum_h = 0;
    desktop_image_texture_t *image = NULL;
    if (clib__strcmp(node->klass, "chat-picture") != 0 || index < 0 || index >= DESKTOP_ATTACHMENT_COUNT)
    {
        g_image_original.layout(node, x, y, width, height, scale);
        return;
    }
    image = &g_images[index];
    maximum_w = (float)g_image_preferences->image_width * scale * (float)g_image_preferences->scale_percent / 100;
    maximum_h = (float)g_image_preferences->image_height * scale * (float)g_image_preferences->scale_percent / 100;
    if (width >= 0)
    {
        maximum_w = clib__fminf(maximum_w, width);
    }
    ratio = image->width > 0 && image->height > 0 ? clib__fminf(scale * (float)g_image_preferences->scale_percent / 100, clib__fminf(maximum_w / (float)image->width, maximum_h / (float)image->height)) : 0;
    node->bounds.x = x;
    node->bounds.y = y;
    node->bounds.w = (float)image->width * ratio;
    node->bounds.h = (float)image->height * ratio;
    node->content_w = node->bounds.w;
    node->content_h = node->bounds.h;
}

/**
 * @brief draws an existing GUI-owned texture; image parsing and file access never occur during painting.
 * @param gui_node_t* node -> scene-owned image label
 * @param float scale -> device display scale
 * @return void
 */
static void _ui_images_internal__draw(gui_node_t *node, float scale)
{
    int64 index = (int64)(nuint)node->user_data - 1;
    gui_color_t tint = { 1, 1, 1, 1 };
    if (clib__strcmp(node->klass, "chat-picture") != 0)
    {
        g_image_original.emit_draws(node, scale);
        return;
    }
    if (index >= 0 && index < DESKTOP_ATTACHMENT_COUNT && g_images[index].texture)
    {
        renderer__submit_image(node->bounds, g_images[index].texture, tint);
    }
}

/**
 * @brief extends text drawing after audio icons have registered their own wrapper.
 * @param desktop_attachments_t* attachments -> borrowed stable session manager
 * @param desktop_chat_preferences_t* preferences -> borrowed saved GUI settings
 * @return void
 */
void desktop_ui_images__initialize(desktop_attachments_t *attachments, desktop_chat_preferences_t *preferences)
{
    const widget_vtable_t *original = widget_registry__get(GUI_NODE_TYPE__TEXT);
    g_image_attachments = attachments;
    g_image_preferences = preferences;
    if (original && original != &g_image_widget)
    {
        g_image_original = *original;
        g_image_widget = *original;
        g_image_widget.layout = _ui_images_internal__layout;
        g_image_widget.emit_draws = _ui_images_internal__draw;
        widget_registry__register(GUI_NODE_TYPE__TEXT, &g_image_widget);
    }
}

/**
 * @brief uploads a worker-decoded preview once and releases its temporary CPU copy after the synchronous upload.
 * @param gui_node_t* parent -> scene-owned chat row
 * @param const desktop_attachment_t* attachment -> copied snapshot
 * @return void
 */
void desktop_ui_images__append(gui_node_t *parent, const desktop_attachment_t *attachment)
{
    desktop_image_texture_t *image = NULL;
    gui_node_t *node = NULL;
    ubyte *pixels = NULL;
    int64 index = 0;
    int64 free_slot = -1;
    uint64 total = 0;
    for (index = 0; index < DESKTOP_ATTACHMENT_COUNT; index++)
    {
        total += g_images[index].bytes;
        if (!g_images[index].texture)
        {
            free_slot = index;
        }
        if (g_images[index].id == attachment->id && g_images[index].texture)
        {
            image = &g_images[index];
        }
    }
    if (!image && free_slot >= 0 && total + (uint64)attachment->image_width * (uint64)attachment->image_height * 4 <= 32ULL * 1024 * 1024)
    {
        image = &g_images[free_slot];
        pixels = desktop_attachments__image(g_image_attachments, attachment->id, &image->width, &image->height);
        if (pixels)
        {
            image->bytes = (uint64)image->width * (uint64)image->height * 4;
            image->texture = renderer__create_texture_rgba(pixels, (int)image->width, (int)image->height);
            mbedtls_platform_zeroize(pixels, image->bytes);
            free(pixels);
            if (image->texture)
            {
                image->id = attachment->id;
            }
            else
            {
                clib__null_memory(image, sizeof(*image));
            }
        }
    }
    if (image && image->texture && parent)
    {
        node = scene__node_new(GUI_NODE_TYPE__TEXT);
        if (node)
        {
            clib__string_copy(node->klass, sizeof(node->klass), "chat-picture");
            node->user_data = (void *)(nuint)(image - g_images + 1);
            scene__add_child(parent, node);
        }
    }
}

/**
 * @brief releases textures whose records were evicted or cleared, including a reconnect with no new pictures.
 * @param const desktop_attachments_view_t* view -> latest copied manager snapshot; GUI thread only
 * @return void
 */
void desktop_ui_images__refresh(const desktop_attachments_view_t *view)
{
    int64 index = 0;
    int64 item = 0;
    boole retained = FALSE;
    for (index = 0; index < DESKTOP_ATTACHMENT_COUNT; index++)
    {
        retained = FALSE;
        for (item = 0; item < DESKTOP_ATTACHMENT_COUNT; item++)
        {
            retained |= view->items[item].id == g_images[index].id && view->items[item].image_width > 0 && view->items[item].state != DESKTOP_ATTACHMENT_FAILED && view->items[item].state != DESKTOP_ATTACHMENT_CANCELLED;
        }
        if (!retained && g_images[index].texture)
        {
            renderer__destroy_texture(g_images[index].texture);
            mbedtls_platform_zeroize(&g_images[index], sizeof(g_images[index]));
        }
    }
}

/**
 * @brief releases every application-owned texture while the renderer is still alive.
 * @return void
 */
void desktop_ui_images__shutdown(void)
{
    int64 index = 0;
    for (index = 0; index < DESKTOP_ATTACHMENT_COUNT; index++)
    {
        if (g_images[index].texture)
        {
            renderer__destroy_texture(g_images[index].texture);
        }
    }
    mbedtls_platform_zeroize(g_images, sizeof(g_images));
    g_image_attachments = NULL;
    g_image_preferences = NULL;
}
