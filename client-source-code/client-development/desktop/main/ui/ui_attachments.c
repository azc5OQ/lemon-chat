#include "ui/ui_layout.h"
#include "ui/ui_headers.h"
#include "ui/ui_attachments.h"
#include "ui/ui_chat_format.h"
#include "ui/ui_theme.h"
#include "ui/session_view.h"
#include "attachments/attachment_file.h"
#include "widgets/widget_filepicker.h"
#include "mbedtls/platform_util.h"

static desktop_session_t *g_attachment_session;
static desktop_session_view_t *g_attachment_view;
static desktop_settings_t *g_attachment_settings;
static desktop_attachment_request_t g_attachment_request;
static uint64 g_attachment_save_id;

// declarations
static void _ui_attachments_internal__text(const char *id, const char *text);
static void _ui_attachments_internal__panel(const char *id, boole visible);
static void _ui_attachments_internal__open(gui_event_t *event);
static void _ui_attachments_internal__close(gui_event_t *event);
static void _ui_attachments_internal__pick(gui_event_t *event);
static void _ui_attachments_internal__submit(gui_event_t *event);
static void _ui_attachments_internal__action(gui_event_t *event);
static void _ui_attachments_internal__settings(gui_event_t *event);
static void _ui_attachments_internal__theme(gui_event_t *event);
static void _ui_attachments_internal__close_settings(gui_event_t *event);
static void _ui_attachments_internal__save_settings(gui_event_t *event);
static void _ui_attachments_internal__values(gui_event_t *event);
static gui_node_t *_ui_attachments_internal__node(gui_node_t *parent, gui_node_type_e type, const char *text, const char *klass, uint64 id);
static const desktop_attachment_t *_ui_attachments_internal__find(uint64 id);

/**
 * @brief finds display metadata by stable ID; no worker-owned payload is exposed.
 * @param uint64 id -> selected attachment
 * @return const desktop_attachment_t* -> borrowed GUI snapshot or NULL
 */
static const desktop_attachment_t *_ui_attachments_internal__find(uint64 id)
{
    int64 index = 0;
    for (index = 0; index < DESKTOP_ATTACHMENT_COUNT; index++)
    {
        if (g_attachment_view->attachments.items[index].id == id)
        {
            return &g_attachment_view->attachments.items[index];
        }
    }
    return NULL;
}

/**
 * @brief copies a plain label and requests a redraw without interpreting markup.
 * @param const char* id -> existing control ID
 * @param const char* text -> copied text
 * @return void
 */
static void _ui_attachments_internal__text(const char *id, const char *text)
{
    gui_node_t *node = scene__find_by_id((char *)id);
    if (node)
    {
        scene__set_text(node, text, clib__strlen(text));
    }
#ifdef _WIN32
    platform_win32__request_render();
#endif
}

/**
 * @brief opens only the floating panel, preserving interaction with chat outside it.
 * @param const char* id -> panel ID
 * @param boole visible -> desired visibility
 * @return void
 */
static void _ui_attachments_internal__panel(const char *id, boole visible)
{
    gui_node_t *node = scene__find_by_id((char *)id);
    if (node)
    {
        clib__string_copy(node->klass, sizeof(node->klass), visible ? "column panel" : "column panel hidden");
        if (visible)
        {
            desktop_ui_layout__raise(node);
        }
    }
#ifdef _WIN32
    platform_win32__request_render();
#endif
}

/**
 * @brief appends a scene-owned control; attachment IDs are values, never borrowed record pointers.
 * @param gui_node_t* parent -> scene-owned row
 * @param gui_node_type_e type -> label or button
 * @param const char* text -> copied plain text
 * @param const char* klass -> copied class
 * @param uint64 id -> stable attachment ID
 * @return gui_node_t* -> scene-owned node or NULL
 */
static gui_node_t *_ui_attachments_internal__node(gui_node_t *parent, gui_node_type_e type, const char *text, const char *klass, uint64 id)
{
    gui_node_t *node = parent ? scene__node_new(type) : NULL;
    if (node)
    {
        scene__set_text(node, text, clib__strlen(text));
        clib__string_copy(node->klass, sizeof(node->klass), klass);
        node->user_data = (void *)(nuint)id;
        if (type == GUI_NODE_TYPE__BUTTON)
        {
            scene__set_on_click(node, "on_attachment_action");
        }
        scene__add_child(parent, node);
    }
    return node;
}

/**
 * @brief displays progress, explicit offer decisions and save controls for one transfer.
 * @param gui_node_t* parent -> scene-owned message row
 * @param uint64 id -> stable attachment ID
 * @return void
 */
void desktop_ui_attachments__append(gui_node_t *parent, uint64 id)
{
    const desktop_attachment_t *attachment = _ui_attachments_internal__find(id);
    gui_node_t *actions = NULL_POINTER;
    char text[384] = { 0 };
    if (!attachment)
    {
        _ui_attachments_internal__node(parent, GUI_NODE_TYPE__TEXT, "Attachment is no longer in memory.", "chat-text muted", 0);
        return;
    }
    if (attachment->picture && attachment->image_width > 0 && attachment->state != DESKTOP_ATTACHMENT_FAILED && attachment->state != DESKTOP_ATTACHMENT_CANCELLED)
    {
        desktop_ui_images__append(parent, attachment);
    }
    clib__string_format(text, sizeof(text), "%s%s%llu KB", attachment->name[0] ? attachment->name : attachment->picture ? "Picture" : "File", " - ", (attachment->size + 1023) / 1024);
    _ui_attachments_internal__node(parent, GUI_NODE_TYPE__TEXT, text, "chat-text", 0);
    clib__string_format(text, sizeof(text), "%s%s", attachment->detail, "");
    if (attachment->state == DESKTOP_ATTACHMENT_UPLOADING || attachment->state == DESKTOP_ATTACHMENT_RECEIVING)
    {
        clib__string_format(text, sizeof(text), "%s - %lld%%", attachment->detail, attachment->progress);
    }
    _ui_attachments_internal__node(parent, GUI_NODE_TYPE__TEXT, text, "chat-text muted", 0);
    actions = _ui_attachments_internal__node(parent, GUI_NODE_TYPE__DIV, "", "row attachment-actions", 0);
    if (attachment->state == DESKTOP_ATTACHMENT_OFFERED)
    {
        _ui_attachments_internal__node(actions, GUI_NODE_TYPE__BUTTON, "Accept", "attachment-accept", id);
        _ui_attachments_internal__node(actions, GUI_NODE_TYPE__BUTTON, "Decline", "attachment-reject", id);
    }
    else if (attachment->state == DESKTOP_ATTACHMENT_READY)
    {
        _ui_attachments_internal__node(actions, GUI_NODE_TYPE__BUTTON, "Save as...", "attachment-save", id);
    }
    else if (attachment->state < DESKTOP_ATTACHMENT_READY)
    {
        _ui_attachments_internal__node(actions, GUI_NODE_TYPE__BUTTON, "Cancel", "attachment-cancel", id);
    }
    mbedtls_platform_zeroize(text, sizeof(text));
}

/**
 * @brief opens a file picker bound to the selected conversation at the time of the click.
 * @param gui_event_t* event -> Picture or File button
 * @return void
 */
static void _ui_attachments_internal__open(gui_event_t *event)
{
    gui_node_t *picker = scene__find_by_id("attachment-picker");
    gui_node_t *filename = scene__find_by_id("attachment-filename");
    char path[DESKTOP_ATTACHMENT_PATH] = { 0 };
    mbedtls_platform_zeroize(&g_attachment_request, sizeof(g_attachment_request));
    g_attachment_save_id = 0;
    g_attachment_request.peer = desktop_session_view__peer(&g_attachment_request.peer_serial);
    g_attachment_request.channel = g_attachment_view->channel;
    g_attachment_request.picture = event && event->sender && clib__strcmp(event->sender->id, "picture-button") == 0;
    if (picker && desktop_attachment_file__home(path, sizeof(path)))
    {
        widget_filepicker__set_path(picker, path);
    }
    if (filename)
    {
        filename->state = GUI_NODE_STATE__DISABLED;
    }
    _ui_attachments_internal__text("attachment-title", g_attachment_request.picture ? "Send picture" : "Send file");
    _ui_attachments_internal__text("attachment-filename", "");
    _ui_attachments_internal__text("attachment-submit", "Send");
    _ui_attachments_internal__text("attachment-result", "Choose a file, then Send. The destination stays the conversation you opened this from.");
    _ui_attachments_internal__panel("attachment-panel", TRUE);
    mbedtls_platform_zeroize(path, sizeof(path));
}

/**
 * @brief drops an unsent file selection without affecting transfers already accepted by the session.
 * @param gui_event_t* event -> unused event
 * @return void
 */
static void _ui_attachments_internal__close(gui_event_t *event)
{
    (void)event;
    _ui_attachments_internal__panel("attachment-panel", FALSE);
    mbedtls_platform_zeroize(&g_attachment_request, sizeof(g_attachment_request));
    g_attachment_save_id = 0;
    _ui_attachments_internal__text("attachment-filename", "");
}

/**
 * @brief copies the picker event while it is valid; selecting never sends or writes a file.
 * @param gui_event_t* event -> borrowed file-picker event
 * @return void
 */
static void _ui_attachments_internal__pick(gui_event_t *event)
{
    char name[256] = { 0 };
    if (event && !event->file.is_dir && event->file.path)
    {
        desktop_attachment_file__name(name, sizeof(name), event->file.path);
        if (g_attachment_save_id)
        {
            _ui_attachments_internal__text("attachment-result", "Choose a new filename. Existing files are not overwritten.");
        }
        else if (clib__string_copy(g_attachment_request.path, sizeof(g_attachment_request.path), event->file.path))
        {
            _ui_attachments_internal__text("attachment-filename", name);
            _ui_attachments_internal__text("attachment-result", "Ready to send the selected file.");
        }
        else
        {
            mbedtls_platform_zeroize(g_attachment_request.path, sizeof(g_attachment_request.path));
            _ui_attachments_internal__text("attachment-result", "The selected path is too long.");
        }
    }
    else if (!g_attachment_save_id)
    {
        mbedtls_platform_zeroize(g_attachment_request.path, sizeof(g_attachment_request.path));
        _ui_attachments_internal__text("attachment-filename", "");
    }
    mbedtls_platform_zeroize(name, sizeof(name));
}

/**
 * @brief queues one explicit send or save; remote filenames cannot choose the output directory.
 * @param gui_event_t* event -> unused click event
 * @return void
 */
static void _ui_attachments_internal__submit(gui_event_t *event)
{
    gui_node_t *filename = scene__find_by_id("attachment-filename");
    gui_node_t *picker = scene__find_by_id("attachment-picker");
    const char *directory = picker ? widget_filepicker__path(picker) : NULL;
    char name[256] = { 0 };
    char path[DESKTOP_ATTACHMENT_PATH] = { 0 };
    boole queued = FALSE;
    (void)event;
    if (g_attachment_save_id)
    {
        desktop_attachment_file__name(name, sizeof(name), filename ? filename->text : "");
        if (filename && filename->text[0] && directory && clib__strcmp(name, filename->text) == 0 && clib__strlen(directory) + clib__strlen(name) + 2 <= sizeof(path) && clib__string_format(path, sizeof(path), "%s/%s", directory, name) >= 0)
        {
            queued = desktop_attachments__save(desktop_session__attachments(g_attachment_session), g_attachment_save_id, path);
        }
    }
    else
    {
        queued = desktop_session__attach(g_attachment_session, &g_attachment_request);
    }
    if (queued)
    {
        _ui_attachments_internal__close(NULL);
    }
    else
    {
        _ui_attachments_internal__text("attachment-result", "Choose a valid file or filename and check that the destination is still available.");
    }
    mbedtls_platform_zeroize(name, sizeof(name));
    mbedtls_platform_zeroize(path, sizeof(path));
}

/**
 * @brief dispatches an explicit transfer decision or opens a Save as picker.
 * @param gui_event_t* event -> scene-owned action with a stable attachment ID
 * @return void
 */
static void _ui_attachments_internal__action(gui_event_t *event)
{
    uint64 id = event && event->sender ? (uint64)(nuint)event->sender->user_data : 0;
    const desktop_attachment_t *attachment = _ui_attachments_internal__find(id);
    desktop_attachments_t *attachments = desktop_session__attachments(g_attachment_session);
    gui_node_t *filename = scene__find_by_id("attachment-filename");
    if (!attachment)
    {
        return;
    }
    if (clib__strcmp(event->sender->klass, "attachment-save") == 0)
    {
        _ui_attachments_internal__open(NULL);
        g_attachment_save_id = id;
        _ui_attachments_internal__text("attachment-title", "Save attachment");
        _ui_attachments_internal__text("attachment-submit", "Save");
        _ui_attachments_internal__text("attachment-filename", attachment->name);
        _ui_attachments_internal__text("attachment-result", "Choose a folder and a new filename. Existing files are not overwritten.");
        if (filename)
        {
            filename->state = GUI_NODE_STATE__DEFAULT;
        }
    }
    else if (clib__strcmp(event->sender->klass, "attachment-accept") == 0)
    {
        desktop_attachments__decide(attachments, id, TRUE);
    }
    else if (clib__strcmp(event->sender->klass, "attachment-reject") == 0)
    {
        desktop_attachments__decide(attachments, id, FALSE);
    }
    else
    {
        desktop_attachments__cancel(attachments, id);
    }
}

/**
 * @brief updates the draft slider label; saved chat scale is unchanged until Save changes.
 * @param gui_event_t* event -> unused change event
 * @return void
 */
static void _ui_attachments_internal__values(gui_event_t *event)
{
    gui_node_t *scale = scene__find_by_id("chat-scale");
    char text[64] = { 0 };
    (void)event;
    if (scale)
    {
        clib__string_format(text, sizeof(text), "Chat size: %lld%%", (int64)scale->value / 5 * 5);
        _ui_attachments_internal__text("chat-scale-label", text);
    }
}

/**
 * @brief opens local display and receiving settings with a fresh draft of saved values.
 * @param gui_event_t* event -> unused click event
 * @return void
 */
static void _ui_attachments_internal__settings(gui_event_t *event)
{
    const char *ids[5] = { "chat-scale", "block-channel-pictures", "block-channel-files", "block-direct-pictures", "block-direct-files" };
    desktop_chat_preferences_t *preferences = &g_attachment_settings->chat;
    int64 values[5] = { 0 };
    int64 index = 0;
    gui_node_t *node = NULL_POINTER;
    char text[32] = { 0 };
    (void)event;
    values[0] = preferences->scale_percent;
    values[1] = preferences->block_channel_pictures;
    values[2] = preferences->block_channel_files;
    values[3] = preferences->block_direct_pictures;
    values[4] = preferences->block_direct_files;
    for (index = 0; index < 5; index++)
    {
        node = scene__find_by_id((char *)ids[index]);
        if (node)
        {
            node->value = (float)values[index];
        }
    }
    clib__string_format(text, sizeof(text), "%lld", preferences->image_width);
    _ui_attachments_internal__text("chat-image-width", text);
    clib__string_format(text, sizeof(text), "%lld", preferences->image_height);
    _ui_attachments_internal__text("chat-image-height", text);
    _ui_attachments_internal__text("chat-theme", g_attachment_settings->theme[0] ? g_attachment_settings->theme : "classic.css");
    node = scene__find_by_id("chat-theme-preset");
    if (node)
    {
        node->value = !g_attachment_settings->theme[0] || clib__strcmp(g_attachment_settings->theme, "classic.css") == 0 ? 0 : clib__strcmp(g_attachment_settings->theme, "slate.css") == 0 ? 1 : clib__strcmp(g_attachment_settings->theme, "amber.css") == 0 ? 2 : clib__strcmp(g_attachment_settings->theme, "ivory.css") == 0 ? 3 : clib__strcmp(g_attachment_settings->theme, "midnight.css") == 0 ? 4 : clib__strcmp(g_attachment_settings->theme, "win95.css") == 0 ? 5 : 6;
    }
    _ui_attachments_internal__theme(NULL);
    _ui_attachments_internal__text("chat-settings-result", "Changes apply when saved.");
    _ui_attachments_internal__values(NULL);
    _ui_attachments_internal__panel("chat-settings-panel", TRUE);
}

/**
 * @brief changes only the draft filename when a built-in theme is selected.
 * @param gui_event_t* event -> select change; saved appearance changes only on Save
 * @return void
 */
static void _ui_attachments_internal__theme(gui_event_t *event)
{
    const char *names[6] = { "classic.css", "slate.css", "amber.css", "ivory.css", "midnight.css", "win95.css" };
    gui_node_t *select = scene__find_by_id("chat-theme-preset");
    gui_node_t *custom = scene__find_by_id("chat-theme-custom");
    int64 index = select ? (int64)select->value : -1;
    (void)event;
    if (index >= 0 && index < 6)
    {
        _ui_attachments_internal__text("chat-theme", names[index]);
    }
    if (custom)
    {
        clib__string_copy(custom->klass, sizeof(custom->klass), index == 6 ? "column theme-file-fields" : "column theme-file-fields hidden");
    }
}

/**
 * @brief closes the unsaved local settings draft.
 * @param gui_event_t* event -> unused click event
 * @return void
 */
static void _ui_attachments_internal__close_settings(gui_event_t *event)
{
    (void)event;
    _ui_attachments_internal__panel("chat-settings-panel", FALSE);
}

/**
 * @brief validates dimensions and commits settings before applying them to the running client.
 * @param gui_event_t* event -> unused click event
 * @return void
 */
static void _ui_attachments_internal__save_settings(gui_event_t *event)
{
    desktop_settings_t settings = *g_attachment_settings;
    const char *ids[7] = { "chat-image-width", "chat-image-height", "chat-scale", "block-channel-pictures", "block-channel-files", "block-direct-pictures", "block-direct-files" };
    gui_node_t *controls[7] = { 0 };
    int64 index = 0;
    int64 digit = 0;
    int64 values[2] = { 0 };
    boole valid = TRUE;
    gui_node_t *theme = scene__find_by_id("chat-theme");
    (void)event;
    for (index = 0; index < 7; index++)
    {
        controls[index] = scene__find_by_id((char *)ids[index]);
        valid &= controls[index] != NULL;
    }
    for (index = 0; valid && index < 2; index++)
    {
        valid = controls[index]->text[0] != 0 && clib__strlen(controls[index]->text) <= 4;
        for (digit = 0; valid && controls[index]->text[digit]; digit++)
        {
            valid = controls[index]->text[digit] >= '0' && controls[index]->text[digit] <= '9';
            values[index] = values[index] * 10 + controls[index]->text[digit] - '0';
        }
        valid &= values[index] >= 64 && values[index] <= 1024;
    }
    if (valid)
    {
        settings.chat.image_width = values[0];
        settings.chat.image_height = values[1];
        settings.chat.scale_percent = (int64)controls[2]->value / 5 * 5;
        settings.chat.block_channel_pictures = controls[3]->value != 0;
        settings.chat.block_channel_files = controls[4]->value != 0;
        settings.chat.block_direct_pictures = controls[5]->value != 0;
        settings.chat.block_direct_files = controls[6]->value != 0;
        valid = settings.chat.scale_percent >= 75 && settings.chat.scale_percent <= 150;
    }
    if (!valid)
    {
        _ui_attachments_internal__text("chat-settings-result", "Image dimensions must be between 64 and 1024 pixels; chat size between 75 and 150 percent.");
    }
    else if (!theme || !clib__string_copy(settings.theme, sizeof(settings.theme), theme->text) || !desktop_ui_theme__save(&settings))
    {
        desktop_ui_attachments__apply();
        _ui_attachments_internal__text("chat-settings-result", "Could not load the theme or save settings. Use a valid .css file in assets/themes (under 64 KB). Changes were not applied.");
    }
    else
    {
        *g_attachment_settings = settings;
        desktop_ui_attachments__apply();
        _ui_attachments_internal__text("chat-settings-result", "Settings saved");
    }
    mbedtls_platform_zeroize(&settings, sizeof(settings));
}

/**
 * @brief applies saved display scale only to the conversation and forwards receiving choices to the session.
 * @return void
 */
void desktop_ui_attachments__apply(void)
{
    desktop_chat_preferences_t *preferences = &g_attachment_settings->chat;
    if (preferences->image_width < 64 || preferences->image_width > 1024)
    {
        preferences->image_width = 480;
    }
    if (preferences->image_height < 64 || preferences->image_height > 1024)
    {
        preferences->image_height = 360;
    }
    if (preferences->scale_percent < 75 || preferences->scale_percent > 150)
    {
        preferences->scale_percent = 100;
    }
    if (!desktop_ui_theme__scale(preferences->scale_percent))
    {
        _ui_attachments_internal__text("chat-settings-result", "This theme has no room for the chat size rules.");
    }
    desktop_attachments__preferences(desktop_session__attachments(g_attachment_session), preferences);
    desktop_ui_chat_format__apply();
#ifdef _WIN32
    platform_win32__request_render();
#endif
}

/**
 * @brief registers attachment controls against stable GUI state and the session-owned manager.
 * @param desktop_session_t* session -> borrowed session
 * @param desktop_session_view_t* view -> borrowed GUI snapshot
 * @param desktop_settings_t* settings -> borrowed saved settings
 * @return void
 */
void desktop_ui_attachments__initialize(desktop_session_t *session, desktop_session_view_t *view, desktop_settings_t *settings)
{
    g_attachment_session = session;
    g_attachment_view = view;
    g_attachment_settings = settings;
    scene__register_handler("on_attachment_open", _ui_attachments_internal__open);
    scene__register_handler("on_attachment_close", _ui_attachments_internal__close);
    scene__register_handler("on_attachment_pick", _ui_attachments_internal__pick);
    scene__register_handler("on_attachment_submit", _ui_attachments_internal__submit);
    scene__register_handler("on_attachment_action", _ui_attachments_internal__action);
    scene__register_handler("on_chat_settings", _ui_attachments_internal__settings);
    scene__register_handler("on_chat_theme", _ui_attachments_internal__theme);
    scene__register_handler("on_chat_settings_close", _ui_attachments_internal__close_settings);
    scene__register_handler("on_chat_settings_save", _ui_attachments_internal__save_settings);
    scene__register_handler("on_chat_settings_values", _ui_attachments_internal__values);
    desktop_ui_images__initialize(desktop_session__attachments(session), &settings->chat);
}

/**
 * @brief releases GPU ownership before shutdown and clears retained file selections.
 * @return void
 */
void desktop_ui_attachments__shutdown(void)
{
    desktop_ui_images__shutdown();
    mbedtls_platform_zeroize(&g_attachment_request, sizeof(g_attachment_request));
    g_attachment_save_id = 0;
    g_attachment_session = NULL;
    g_attachment_view = NULL;
    g_attachment_settings = NULL;
}
