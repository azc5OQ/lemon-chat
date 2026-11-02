// Application input behavior extends the public widget API without changing Meek UI.
#include "ui/ui_headers.h"
#include "widget_registry.h"
#include "ui/ui_input.h"
#include "ui/ui_connection.h"
#include "clib.h"
#include "session/session.h"
#include "settings/settings.h"
#include "platform/memory.h"

static widget_vtable_t g_original_input;
static widget_vtable_t g_desktop_input;
static void (*g_submit)(void);
// declarations
static void _ui_input_internal__layout(gui_node_t* node, float x, float y, float width, float height, float scale);
static void _ui_input_internal__key(gui_node_t* node, int64 key, boole down);
static void _ui_input_internal__destroy(gui_node_t* node);

/**
 * @brief bounds editable text to the receiving application buffers after loading or reloading the scene.
 * @return boole -> TRUE when every field accepts its limit; existing drafts are never truncated
 */
boole desktop_ui_input__limits(void)
{
    static const struct
    {
        const char* id;
        int64 bytes;
    } limits[] =
    {
        {"message-input", DESKTOP_CHAT_CAPACITY - 1},
        {"message-edit-text", DESKTOP_CHAT_CAPACITY - 1},
        {"attachment-filename", 255},
        {"chat-theme", DESKTOP_THEME_NAME_CAPACITY - 1},
        {"chat-image-width", 4},
        {"chat-image-height", 4},
        {"server-address", 253},
        {"server-port", 5},
        {"username", sizeof(((desktop_connection_t*)0)->username) - 1},
        {"identity-string", sizeof(((desktop_connection_t*)0)->identity) - 1},
        {"identity-bits", 4},
        {"bookmark-name", sizeof(((desktop_bookmark_t*)0)->name) - 1},
        {"bookmark-address", sizeof(((desktop_bookmark_t*)0)->address) - 1},
        {"channel-password", sizeof(((desktop_channel_request_t*)0)->password) - 1},
        {"action-value", sizeof(((desktop_action_t*)0)->text) - 1},
        {"channel-edit-name", sizeof(((desktop_channel_t*)0)->name) - 1},
        {"channel-edit-description", sizeof(((desktop_channel_t*)0)->description) - 1},
        {"channel-edit-password", sizeof(((desktop_channel_request_t*)0)->password) - 1},
        {"channel-edit-capacity", 10},
        {"channel-talk-power", 4},
        {"channel-message-power", 4},
        {"channel-picture-power", 4},
        {"channel-stream-power", 4},
        {"game-executable", DESKTOP_GAME_NAME_CAPACITY - 1}
    };
    gui_node_t* node = NULL;
    int64 index = 0;
    boole complete = TRUE;
    for (index = 0; index < (int64)(sizeof(limits) / sizeof(limits[0])); index++)
    {
        node = scene__find_by_id((char*)limits[index].id);
        if (!node || !scene__set_text_limit(node, limits[index].bytes))
        {
            complete = FALSE;
        }
    }
    return desktop_ui_connection__refresh() && complete;
}

/**
 * @brief lets the chat composer fill its allocated row width while retaining upstream text editing.
 * @param gui_node_t* node -> scene-owned input control
 * @param float x -> allocated horizontal position
 * @param float y -> allocated vertical position
 * @param float width -> allocated physical width
 * @param float height -> allocated physical height
 * @param float scale -> current display scale
 * @return void
 */
static void _ui_input_internal__layout(gui_node_t* node, float x, float y, float width, float height, float scale)
{
    g_original_input.layout(node, x, y, width, height, scale);
    if ((clib__strcmp(node->id, "message-input") == 0 || node->resolved.width_pct > 0) && width > 0)
    {
        node->bounds.w = width;
    }
}

/**
 * @brief submits a composer on Enter; other fields and editing keys retain their existing behavior.
 * @param gui_node_t* node -> focused scene-owned input
 * @param int64 key -> Meek UI's portable virtual key code
 * @param boole down -> nonzero for a key press
 * @return void
 */
static void _ui_input_internal__key(gui_node_t* node, int64 key, boole down)
{
    if (clib__strcmp(node->id, "message-input") == 0 && key == 13)
    {
        if (down && g_submit)
        {
            g_submit();
        }
        return;
    }
    if (g_original_input.on_key)
    {
        g_original_input.on_key(node, key, down);
    }
}

/**
 * @brief wipes editable storage before the scene releases its owned buffer.
 * @param gui_node_t* node -> scene-owned input being destroyed
 * @return void
 */
static void _ui_input_internal__destroy(gui_node_t* node)
{
    desktop_memory__erase(node->text, node->text_capacity);
    if (g_original_input.on_destroy)
    {
        g_original_input.on_destroy(node);
    }
}

/**
 * @brief installs the application's composer behavior after platform initialization, on the GUI thread.
 * @param void (*submit)(void) -> callback retained until GUI shutdown
 * @return void
 */
void desktop_ui_input__register(void (*submit)(void))
{
    const widget_vtable_t* original = widget_registry__get(GUI_NODE_TYPE__INPUT);
    if (!original || !original->layout || original == &g_desktop_input)
    {
        return;
    }
    g_submit = submit;
    g_original_input = *original;
    g_desktop_input = *original;
    g_desktop_input.layout = _ui_input_internal__layout;
    g_desktop_input.on_key = _ui_input_internal__key;
    g_desktop_input.on_destroy = _ui_input_internal__destroy;
    widget_registry__register(GUI_NODE_TYPE__INPUT, &g_desktop_input);
}
