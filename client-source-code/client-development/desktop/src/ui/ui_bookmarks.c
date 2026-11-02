#include "ui/ui_layout.h"
#include "ui/ui_connection.h"
#include "ui/ui_bookmarks.h"
#include "ui/ui_headers.h"
#include "network/endpoint.h"

static desktop_settings_t* g_bookmarks_settings;
static int64 g_bookmarks_selected = -1;

// declarations
static void _ui_bookmarks_internal__text(const char* id, const char* text);
static void _ui_bookmarks_internal__panel(const char* id, boole visible);
static void _ui_bookmarks_internal__list(void);
static void _ui_bookmarks_internal__choose(gui_event_t* event);
static void _ui_bookmarks_internal__show(gui_event_t* event);
static void _ui_bookmarks_internal__close(gui_event_t* event);
static void _ui_bookmarks_internal__add(gui_event_t* event);
static void _ui_bookmarks_internal__save(gui_event_t* event);
static void _ui_bookmarks_internal__remove(gui_event_t* event);
static void _ui_bookmarks_internal__use(gui_event_t* event);

/**
 * @brief copies plain text into a scene control.
 * @param const char* id -> control ID
 * @param const char* text -> copied display text
 * @return void
 */
static void _ui_bookmarks_internal__text(const char* id, const char* text)
{
    gui_node_t* node = scene__find_by_id((char*)id);
    if (node)
    {
        scene__set_text(node, text, clib__strlen(text));
    }
}

/**
 * @brief opens a floating panel without blocking the remaining window.
 * @param const char* id -> panel ID
 * @param boole visible -> requested visibility
 * @return void
 */
static void _ui_bookmarks_internal__panel(const char* id, boole visible)
{
    gui_node_t* node = scene__find_by_id((char*)id);
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
 * @brief rebuilds the bounded saved-address list and its selection.
 * @return void
 */
static void _ui_bookmarks_internal__list(void)
{
    gui_node_t* parent = scene__find_by_id("bookmark-list");
    gui_node_t* node = NULL;
    gui_node_t* next = NULL;
    int64 index = 0;
    if (!parent)
    {
        return;
    }
    node = parent->first_child;
    parent->first_child = NULL;
    parent->last_child = NULL;
    parent->child_count = 0;
    while (node)
    {
        next = node->next_sibling;
        node->parent = NULL;
        node->next_sibling = NULL;
        scene__node_free(node);
        node = next;
    }
    for (index = 0; index < g_bookmarks_settings->bookmark_count; index++)
    {
        node = scene__node_new(GUI_NODE_TYPE__BUTTON);
        if (node)
        {
            scene__set_text(node, g_bookmarks_settings->bookmarks[index].name, clib__strlen(g_bookmarks_settings->bookmarks[index].name));
            clib__string_copy(node->klass, sizeof(node->klass), index == g_bookmarks_selected ? "bookmark-row selected" : "bookmark-row");
            node->user_data = (void*)(nint)(index + 1);
            scene__set_on_click(node, "on_bookmark_choose");
            scene__add_child(parent, node);
        }
    }
    node = scene__find_by_id("bookmark-remove");
    if (node)
    {
        node->state = g_bookmarks_selected < 0 ? GUI_NODE_STATE__DISABLED : GUI_NODE_STATE__DEFAULT;
    }
#ifdef _WIN32
    platform_win32__request_render();
#endif
}

/**
 * @brief loads a saved entry as an editable draft without applying changes.
 * @param gui_event_t* event -> selected saved row
 * @return void
 */
static void _ui_bookmarks_internal__choose(gui_event_t* event)
{
    int64 index = event && event->sender ? (nint)event->sender->user_data - 1 : -1;
    if (index < 0 || index >= g_bookmarks_settings->bookmark_count)
    {
        return;
    }
    g_bookmarks_selected = index;
    _ui_bookmarks_internal__text("bookmark-name", g_bookmarks_settings->bookmarks[index].name);
    _ui_bookmarks_internal__text("bookmark-address", g_bookmarks_settings->bookmarks[index].address);
    _ui_bookmarks_internal__text("bookmark-result", "Changes apply when saved.");
    _ui_bookmarks_internal__list();
}

/**
 * @brief prepares a new bookmark using the current connection address.
 * @param gui_event_t* event -> unused button event
 * @return void
 */
static void _ui_bookmarks_internal__add(gui_event_t* event)
{
    char address[1024] = {0};
    (void)event;
    if (!desktop_ui_connection__address(address, sizeof(address)))
    {
        clib__string_copy(address, sizeof(address), g_bookmarks_settings->address);
    }
    g_bookmarks_selected = -1;
    _ui_bookmarks_internal__text("bookmark-name", "");
    _ui_bookmarks_internal__text("bookmark-address", address);
    _ui_bookmarks_internal__text("bookmark-result", "Enter a name and choose Save changes.");
    _ui_bookmarks_internal__list();
}

/**
 * @brief opens the bookmark editor with a fresh draft.
 * @param gui_event_t* event -> unused toolbar event
 * @return void
 */
static void _ui_bookmarks_internal__show(gui_event_t* event)
{
    _ui_bookmarks_internal__add(event);
    _ui_bookmarks_internal__panel("bookmarks-panel", TRUE);
}

/**
 * @brief closes the editor without saving its draft.
 * @param gui_event_t* event -> unused close event
 * @return void
 */
static void _ui_bookmarks_internal__close(gui_event_t* event)
{
    (void)event;
    _ui_bookmarks_internal__panel("bookmarks-panel", FALSE);
}

/**
 * @brief saves a validated draft before replacing the current settings.
 * @param gui_event_t* event -> explicit save button event
 * @return void
 */
static void _ui_bookmarks_internal__save(gui_event_t* event)
{
    desktop_settings_t settings = *g_bookmarks_settings;
    desktop_bookmark_t bookmark = {0};
    gui_node_t* name = scene__find_by_id("bookmark-name");
    gui_node_t* address = scene__find_by_id("bookmark-address");
    int64 index = g_bookmarks_selected;
    (void)event;
    if (!name || !address || !clib__string_copy(bookmark.name, sizeof(bookmark.name), name->text)
        || !clib__string_copy(bookmark.address, sizeof(bookmark.address), address->text)
        || !desktop_settings__valid_bookmark(&bookmark))
    {
        _ui_bookmarks_internal__text("bookmark-result", "Use a name up to 63 bytes and a valid ws:// or wss:// address.");
        goto label_finished;
    }
    if (index < 0)
    {
        if (settings.bookmark_count >= DESKTOP_BOOKMARK_LIMIT)
        {
            _ui_bookmarks_internal__text("bookmark-result", "All 16 bookmark slots are used. Edit or remove an entry.");
            goto label_finished;
        }
        index = settings.bookmark_count++;
    }
    settings.bookmarks[index] = bookmark;
    if (!desktop_settings__save(&settings))
    {
        _ui_bookmarks_internal__text("bookmark-result", "Could not save bookmarks. Your saved entries are unchanged.");
        goto label_finished;
    }
    *g_bookmarks_settings = settings;
    g_bookmarks_selected = index;
    _ui_bookmarks_internal__list();
    _ui_bookmarks_internal__text("bookmark-result", "Bookmark saved.");
label_finished:
    clib__null_memory(&settings, sizeof(settings));
}

/**
 * @brief explicitly removes the selected saved entry while preserving the others.
 * @param gui_event_t* event -> remove button event
 * @return void
 */
static void _ui_bookmarks_internal__remove(gui_event_t* event)
{
    desktop_settings_t settings = *g_bookmarks_settings;
    int64 index = g_bookmarks_selected;
    (void)event;
    if (index < 0 || index >= settings.bookmark_count)
    {
        goto label_finished;
    }
    for (; index + 1 < settings.bookmark_count; index++)
    {
        settings.bookmarks[index] = settings.bookmarks[index + 1];
    }
    settings.bookmark_count--;
    clib__null_memory(&settings.bookmarks[settings.bookmark_count], sizeof(desktop_bookmark_t));
    if (!desktop_settings__save(&settings))
    {
        _ui_bookmarks_internal__text("bookmark-result", "Could not save the removal. Your bookmark is unchanged.");
        goto label_finished;
    }
    *g_bookmarks_settings = settings;
    _ui_bookmarks_internal__add(NULL);
    _ui_bookmarks_internal__text("bookmark-result", "Bookmark removed.");
label_finished:
    clib__null_memory(&settings, sizeof(settings));
}

/**
 * @brief fills Connections without changing identity or sending a network request.
 * @param gui_event_t* event -> use-address button event
 * @return void
 */
static void _ui_bookmarks_internal__use(gui_event_t* event)
{
    desktop_bookmark_t bookmark = {0};
    gui_node_t* address = scene__find_by_id("bookmark-address");
    char current[1024] = {0};
    desktop_endpoint_t previous = {0};
    desktop_endpoint_t selected = {0};
    boole different = FALSE;
    (void)event;
    clib__string_copy(bookmark.name, sizeof(bookmark.name), "Connection");
    if (!address || !clib__string_copy(bookmark.address, sizeof(bookmark.address), address->text)
        || !desktop_settings__valid_bookmark(&bookmark))
    {
        _ui_bookmarks_internal__text("bookmark-result", "Enter a valid ws:// or wss:// address.");
        return;
    }
    different = !desktop_ui_connection__address(current, sizeof(current)) || !desktop_endpoint__parse(current, &previous)
        || !desktop_endpoint__parse(bookmark.address, &selected) || previous.port != selected.port
        || clib__strcmp(previous.host, selected.host) != 0;
    if (!desktop_ui_connection__load_address(bookmark.address))
    {
        _ui_bookmarks_internal__text("bookmark-result", "Could not load this address. Check its hostname and port.");
        return;
    }
    if (different)
    {
        desktop_ui_connection__clear_keys();
    }
    _ui_bookmarks_internal__text("login-result", "Address selected. Enter server encryption keys if needed, then choose Connect.");
    _ui_bookmarks_internal__panel("bookmarks-panel", FALSE);
    _ui_bookmarks_internal__panel("connection-panel", TRUE);
}

/**
 * @brief registers bookmark handlers before the native layout is parsed.
 * @param desktop_settings_t* settings -> GUI-owned saved preferences
 * @return void
 */
void desktop_ui_bookmarks__initialize(desktop_settings_t* settings)
{
    g_bookmarks_settings = settings;
    scene__register_handler("on_show_bookmarks", _ui_bookmarks_internal__show);
    scene__register_handler("on_close_bookmarks", _ui_bookmarks_internal__close);
    scene__register_handler("on_bookmark_choose", _ui_bookmarks_internal__choose);
    scene__register_handler("on_bookmark_add", _ui_bookmarks_internal__add);
    scene__register_handler("on_bookmark_save", _ui_bookmarks_internal__save);
    scene__register_handler("on_bookmark_remove", _ui_bookmarks_internal__remove);
    scene__register_handler("on_bookmark_use", _ui_bookmarks_internal__use);
}
