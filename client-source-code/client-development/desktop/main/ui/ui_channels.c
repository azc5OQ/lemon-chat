#include "ui/ui_layout.h"
#include "ui/ui_channels.h"
#include "ui/ui_headers.h"
#include "platform/system.h"
#include "clib.h"
#include <stdlib.h>

static desktop_session_t *g_channels_session;
static const desktop_session_view_t *g_channels_view;
static desktop_channel_request_t g_channels_draft;
static desktop_channel_request_t g_channels_delete;
static desktop_channel_t *g_channels_list;
static int64 g_channels_count;
static boole g_channels_open;
static boole g_channels_text_ready;
static uint64 g_channels_access_time;
static uint64 g_channels_submitted;
static uint64 g_channels_result;
static uint64 g_channels_error;
static const char *g_channels_powers[4] = { "channel-talk-power", "channel-message-power", "channel-picture-power", "channel-stream-power" };

// declarations
static boole _ui_channels_internal__text(const char *id, const char *text);
static void _ui_channels_internal__enabled(const char *id, boole enabled);
static void _ui_channels_internal__panel(const char *id, boole visible);
static void _ui_channels_internal__checked(const char *id, boole checked);
static boole _ui_channels_internal__value(const char *id);
static boole _ui_channels_internal__number(const char *id, int64 maximum, int64 *value);
static const desktop_channel_t *_ui_channels_internal__find(int64 id);
static void _ui_channels_internal__select(int64 id);
static void _ui_channels_internal__fill(void);
static void _ui_channels_internal__list(void);
static void _ui_channels_internal__show(gui_event_t *event);
static void _ui_channels_internal__close(gui_event_t *event);
static void _ui_channels_internal__choose(gui_event_t *event);
static void _ui_channels_internal__create(gui_event_t *event);
static void _ui_channels_internal__save(gui_event_t *event);
static void _ui_channels_internal__remove(gui_event_t *event);
static void _ui_channels_internal__confirm(gui_event_t *event);
static void _ui_channels_internal__cancel_delete(gui_event_t *event);

/**
 * @brief copies plain text into a scene-owned control.
 * @param const char* id -> control ID
 * @param const char* text -> bounded display text
 * @return boole -> TRUE when the text was replaced; FALSE retains the previous value
 */
static boole _ui_channels_internal__text(const char *id, const char *text)
{
    gui_node_t *node = scene__find_by_id((char *)id);
    return scene__set_text(node, text, clib__strlen(text));
}

/**
 * @brief keeps unavailable controls in place without overwriting an enabled control's hover state.
 * @param const char* id -> control ID
 * @param boole enabled -> nonzero when the current permission allows editing
 * @return void
 */
static void _ui_channels_internal__enabled(const char *id, boole enabled)
{
    gui_node_t *node = scene__find_by_id((char *)id);

    // enable or disable control
    if (node && !enabled)
    {
        node->state = GUI_NODE_STATE__DISABLED;
    }
    else if (node && node->state == GUI_NODE_STATE__DISABLED)
    {
        node->state = GUI_NODE_STATE__DEFAULT;
    }
}

/**
 * @brief shows a floating editor without a full-window input barrier.
 * @param const char* id -> panel ID
 * @param boole visible -> nonzero to show
 * @return void
 */
static void _ui_channels_internal__panel(const char *id, boole visible)
{
    gui_node_t *node = scene__find_by_id((char *)id);

    // change the visibility
    if (node)
    {
        clib__string_copy(node->klass, sizeof(node->klass), visible ? "column panel" : "column panel hidden");
        if (visible)
        {
            desktop_ui_layout__raise(node);
        }
    }
#ifdef _WIN32
    // request a redraw
    platform_win32__request_render();
#endif
}

/**
 * @brief sets one checkbox while populating a draft, without sending a change.
 * @param const char* id -> checkbox ID
 * @param boole checked -> checkbox value
 * @return void
 */
static void _ui_channels_internal__checked(const char *id, boole checked)
{
    gui_node_t *node = scene__find_by_id((char *)id);
    if (node)
    {
        node->value = checked != 0;
    }
}

/**
 * @brief reads one checkbox from the unsaved draft.
 * @param const char* id -> checkbox ID
 * @return boole -> nonzero when checked
 */
static boole _ui_channels_internal__value(const char *id)
{
    gui_node_t *node = scene__find_by_id((char *)id);
    return node && node->value != 0;
}

/**
 * @brief parses an entire unsigned decimal field with a checked bound.
 * @param const char* id -> numeric input ID
 * @param int64 maximum -> highest accepted value
 * @param int64* value -> receives the parsed value only on success
 * @return boole -> nonzero when every character is valid and the result fits
 */
static boole _ui_channels_internal__number(const char *id, int64 maximum, int64 *value)
{
    gui_node_t *node = scene__find_by_id((char *)id);
    int64 index = 0;
    int64 number = 0;
    int64 digit = 0;

    // refuse an empty field
    if (!node || !node->text[0])
    {
        return 0;
    }

    // read every digit
    for (index = 0; node->text[index]; index++)
    {
        digit = node->text[index] - '0';
        if (digit < 0 || digit > 9 || number > (maximum - digit) / 10 || digit > maximum)
        {
            return 0;
        }
        number = number * 10 + digit;
    }

    // store the number
    *value = number;
    return 1;
}

/**
 * @brief finds a channel by its server ID in the GUI snapshot.
 * @param int64 id -> server channel ID
 * @return const desktop_channel_t* -> current borrowed record, or NULL
 */
static const desktop_channel_t *_ui_channels_internal__find(int64 id)
{
    int64 index = 0;

    // search the channel list
    for (index = 0; index < g_channels_view->channel_count; index++)
    {
        if (g_channels_view->channels[index].id == id)
        {
            return &g_channels_view->channels[index];
        }
    }
    return NULL;
}

/**
 * @brief loads all current channel options into an unsaved form.
 * @return void
 */
static void _ui_channels_internal__fill(void)
{
    char text[192] = { 0 };
    int64 index = 0;
    boole creating = g_channels_draft.action == DESKTOP_CHANNEL_CREATE;
    const desktop_channel_t *parent = _ui_channels_internal__find(g_channels_draft.channel.id);

    // fill the form heading
    _ui_channels_internal__text("channel-form-title", creating ? "New child channel" : g_channels_draft.channel.name);
    clib__string_format(text, sizeof(text), creating ? "Inside %s" : "Channel: %s", parent ? parent->name : "Unavailable");
    _ui_channels_internal__text("channel-form-context", text);

    // fill the text fields
    g_channels_text_ready = _ui_channels_internal__text("channel-edit-name", g_channels_draft.channel.name);
    g_channels_text_ready &= _ui_channels_internal__text("channel-edit-description", g_channels_draft.channel.description);
    g_channels_text_ready &= _ui_channels_internal__text("channel-edit-password", "");

    // set the checkboxes
    _ui_channels_internal__checked("channel-replace-password", creating);
    _ui_channels_internal__checked("channel-edit-audio", g_channels_draft.channel.audio);
    _ui_channels_internal__checked("channel-edit-video", g_channels_draft.channel.video);
    _ui_channels_internal__checked("channel-edit-temporary", g_channels_draft.channel.temporary);
    _ui_channels_internal__checked("channel-edit-limited", g_channels_draft.channel.limited);

    // fill the number fields
    clib__string_format(text, sizeof(text), "%lld", g_channels_draft.channel.capacity);
    g_channels_text_ready &= _ui_channels_internal__text("channel-edit-capacity", text);
    for (index = 0; index < 4; index++)
    {
        clib__string_format(text, sizeof(text), "%lld", g_channels_draft.channel.powers[index]);
        g_channels_text_ready &= _ui_channels_internal__text(g_channels_powers[index], text);
    }

    // set button and result
    _ui_channels_internal__text("channel-save", creating ? "Create channel" : "Save changes");
    _ui_channels_internal__text("channel-form-result", g_channels_text_ready ? "Changes apply only when saved." : "Could not load every field. Select the channel again before saving.");
}

/**
 * @brief captures a selected channel version and asks the server for its editable fields.
 * @param int64 id -> selected channel ID
 * @return void
 */
static void _ui_channels_internal__select(int64 id)
{
    const desktop_channel_t *channel = _ui_channels_internal__find(id);
    desktop_channel_request_t query = { 0 };
    gui_node_t *fields = scene__find_by_id("channel-editor-fields");

    // refuse a missing channel
    if (!channel)
    {
        return;
    }

    // copy channel into draft
    clib__null_memory(&g_channels_draft, sizeof(g_channels_draft));
    g_channels_draft.action = DESKTOP_CHANNEL_EDIT;
    g_channels_draft.channel = *channel;
    g_channels_submitted = 0;

    // reset the scroll position
    if (fields)
    {
        fields->scroll_y = 0;
        fields->scroll_y_target = 0;
    }

    // request channel access
    query.action = DESKTOP_CHANNEL_ACCESS;
    query.channel = *channel;
    desktop_session__channel_request(g_channels_session, &query);
    g_channels_access_time = desktop_system__milliseconds();

    // update form and list
    _ui_channels_internal__fill();
    _ui_channels_internal__list();
}

/**
 * @brief rebuilds the channel selector using a stable snapshot for its button indices.
 * @return void
 */
static void _ui_channels_internal__list(void)
{
    gui_node_t *parent = scene__find_by_id("channel-editor-list");
    gui_node_t *node = NULL_POINTER;
    gui_node_t *next = NULL_POINTER;
    int64 index = 0;

    // check the parent
    if (!parent)
    {
        return;
    }

    // detach all children
    node = parent->first_child;
    parent->first_child = NULL;
    parent->last_child = NULL;
    parent->child_count = 0;

    // free every child
    while (node)
    {
        next = node->next_sibling;
        node->parent = NULL;
        node->next_sibling = NULL;
        scene__node_free(node);
        node = next;
    }

    // free the old copy
    free(g_channels_list);
    g_channels_list = NULL;
    g_channels_count = 0;

    // copy the channel list
    if (g_channels_view->channel_count > 0)
    {
        g_channels_list = malloc((size_t)g_channels_view->channel_count * sizeof(*g_channels_list));
        if (!g_channels_list)
        {
            _ui_channels_internal__text("channel-form-result", "Not enough memory to display the channel list.");
            return;
        }
        clib__memcpy(g_channels_list, g_channels_view->channels, g_channels_view->channel_count * sizeof(*g_channels_list));
    }
    g_channels_count = g_channels_view->channel_count;

    // add every channel button
    for (index = 0; index < g_channels_count; index++)
    {
        node = scene__node_new(GUI_NODE_TYPE__BUTTON);
        if (node)
        {
            clib__string_copy(node->klass, sizeof(node->klass), g_channels_list[index].id == g_channels_draft.channel.id ? "channel-editor-item selected" : "channel-editor-item");
            scene__set_text(node, g_channels_list[index].name, clib__strlen(g_channels_list[index].name));
            node->user_data = (void *)(nuint)(index + 1);
            scene__set_on_click(node, "on_channel_editor_choose");
            scene__add_child(parent, node);
        }
    }
}

/**
 * @brief opens channel settings on the current channel without joining another channel.
 * @param gui_event_t* event -> unused toolbar event
 * @return void
 */
static void _ui_channels_internal__show(gui_event_t *event)
{
    (void)event;

    // check connection and channels
    if (!g_channels_view->connected || g_channels_view->idle || !g_channels_view->channel_count)
    {
        return;
    }

    // store the sequence numbers
    g_channels_open = 1;
    g_channels_result = g_channels_view->channel_result_sequence;
    g_channels_error = g_channels_view->action_error_sequence;

    // open the channels panel
    _ui_channels_internal__select(g_channels_view->channel >= 0 ? g_channels_view->channel : g_channels_view->channels[0].id);
    _ui_channels_internal__panel("channels-panel", 1);
}

/**
 * @brief opens the selected channel's existing draft or confirmation from its context menu.
 * @param int64 channel -> current server channel ID
 * @param desktop_channel_action_e action -> create, edit, or remove
 * @return void
 */
void desktop_ui_channels__open(int64 channel, desktop_channel_action_e action)
{
    // check connection and channel
    if (!g_channels_view->connected || g_channels_view->idle || !_ui_channels_internal__find(channel))
    {
        return;
    }

    // store the sequence numbers
    g_channels_open = TRUE;
    g_channels_result = g_channels_view->channel_result_sequence;
    g_channels_error = g_channels_view->action_error_sequence;

    // open the channels panel
    _ui_channels_internal__select(channel);
    _ui_channels_internal__panel("channels-panel", TRUE);

    // open create or remove
    if (action == DESKTOP_CHANNEL_CREATE)
    {
        _ui_channels_internal__create(NULL);
    }
    else if (action == DESKTOP_CHANNEL_REMOVE)
    {
        _ui_channels_internal__remove(NULL);
    }
}

/**
 * @brief closes the editor and clears its password draft.
 * @param gui_event_t* event -> unused close event
 * @return void
 */
static void _ui_channels_internal__close(gui_event_t *event)
{
    (void)event;

    // free the channel copy
    g_channels_open = 0;
    free(g_channels_list);
    g_channels_list = NULL;
    g_channels_count = 0;

    // clear drafts and password
    clib__null_memory(&g_channels_draft, sizeof(g_channels_draft));
    clib__null_memory(&g_channels_delete, sizeof(g_channels_delete));
    _ui_channels_internal__text("channel-edit-password", "");

    // close both panels
    _ui_channels_internal__panel("channels-panel", 0);
    _ui_channels_internal__panel("channel-delete-panel", 0);
}

void desktop_ui_channels__shutdown(void)
{
    // free the channel copy
    free(g_channels_list);
    g_channels_list = NULL;
    g_channels_count = 0;
    g_channels_open = FALSE;
}

/**
 * @brief selects the channel represented by the stable selector snapshot.
 * @param gui_event_t* event -> channel button with one-based snapshot index
 * @return void
 */
static void _ui_channels_internal__choose(gui_event_t *event)
{
    nuint index = event && event->sender ? (nuint)event->sender->user_data : 0;
    if (index && index <= g_channels_count && !g_channels_submitted)
    {
        _ui_channels_internal__select(g_channels_list[index - 1].id);
    }
}

/**
 * @brief starts an unsaved child channel under the selected permanent channel.
 * @param gui_event_t* event -> unused create button event
 * @return void
 */
static void _ui_channels_internal__create(gui_event_t *event)
{
    const desktop_channel_t *parent = _ui_channels_internal__find(g_channels_draft.channel.id);
    (void)event;

    // check parent and permission
    if (!parent || parent->temporary || g_channels_submitted || (!g_channels_view->can_create_channel && !g_channels_view->can_create_temp_channel))
    {
        return;
    }

    // start a new draft
    clib__null_memory(&g_channels_draft, sizeof(g_channels_draft));
    g_channels_draft.action = DESKTOP_CHANNEL_CREATE;
    g_channels_draft.channel.id = parent->id;
    g_channels_draft.channel.revision = parent->revision;
    g_channels_draft.channel.audio = 1;
    g_channels_draft.channel.video = 1;
    g_channels_draft.channel.temporary = !g_channels_view->can_create_channel;

    // fill the form
    _ui_channels_internal__fill();
}

/**
 * @brief validates the whole form and queues one explicit channel save.
 * @param gui_event_t* event -> unused save event
 * @return void
 */
static void _ui_channels_internal__save(gui_event_t *event)
{
    desktop_channel_request_t request = g_channels_draft;
    gui_node_t *name = scene__find_by_id("channel-edit-name");
    gui_node_t *description = scene__find_by_id("channel-edit-description");
    gui_node_t *password = scene__find_by_id("channel-edit-password");
    int64 index = 0;
    (void)event;

    // check fields were loaded
    if (!g_channels_text_ready)
    {
        _ui_channels_internal__text("channel-form-result", "Could not load every field. Select the channel again before saving.");
        return;
    }

    // check the name
    if (g_channels_submitted || !name || !description || !password || !name->text[0] || !clib__string_copy(request.channel.name, sizeof(request.channel.name), name->text) || clib__utf8_string_length_check_max_length(name->text, 50) < 0)
    {
        _ui_channels_internal__text("channel-form-result", "Enter a name up to 50 characters that fits in 127 UTF-8 bytes.");
        return;
    }

    // copy the description
    if (!clib__string_copy(request.channel.description, sizeof(request.channel.description), description->text))
    {
        _ui_channels_internal__text("channel-form-result", "The description must fit in 999 UTF-8 bytes.");
        return;
    }

    // check the password
    request.replace_password = _ui_channels_internal__value("channel-replace-password");
    if (request.replace_password && (!clib__string_copy(request.password, sizeof(request.password), password->text) || clib__utf8_string_length_check_max_length(password->text, 30) < 0))
    {
        _ui_channels_internal__text("channel-form-result", "Use a password no longer than 30 characters.");
        return;
    }

    // read the checkboxes
    request.channel.audio = _ui_channels_internal__value("channel-edit-audio");
    request.channel.video = _ui_channels_internal__value("channel-edit-video");
    request.channel.temporary = _ui_channels_internal__value("channel-edit-temporary");
    request.channel.limited = _ui_channels_internal__value("channel-edit-limited");

    // check the member limit
    if (!_ui_channels_internal__number("channel-edit-capacity", 2147483647, &request.channel.capacity) || (request.channel.limited && !request.channel.capacity))
    {
        _ui_channels_internal__text("channel-form-result", "Enter a positive member limit, or untick the limit and use 0.");
        return;
    }

    // check the power values
    for (index = 0; index < 4; index++)
    {
        if (!_ui_channels_internal__number(g_channels_powers[index], 1000, &request.channel.powers[index]))
        {
            _ui_channels_internal__text("channel-form-result", "Power requirements must be whole numbers from 0 to 1000.");
            return;
        }
    }

    // compare with server limit
    if (g_channels_view->server_capacity > 0 && request.channel.capacity > g_channels_view->server_capacity)
    {
        _ui_channels_internal__text("channel-form-result", "The channel member limit cannot exceed the server's slot limit.");
        return;
    }

    // send the request
    if (desktop_session__channel_request(g_channels_session, &request))
    {
        g_channels_submitted = desktop_system__milliseconds();
        g_channels_error = g_channels_view->action_error_sequence;
        _ui_channels_internal__text("channel-form-result", "Waiting for the server to confirm the changes...");
        _ui_channels_internal__text("channel-edit-password", "");
    }
    else
    {
        _ui_channels_internal__text("channel-form-result", "Channel changed or disconnected. Select it again before saving.");
    }

    // clear the request copy
    clib__null_memory(&request, sizeof(request));
}

/**
 * @brief captures a channel deletion and asks for explicit confirmation.
 * @param gui_event_t* event -> unused delete event
 * @return void
 */
static void _ui_channels_internal__remove(gui_event_t *event)
{
    const desktop_channel_t *channel = _ui_channels_internal__find(g_channels_draft.channel.id);
    char text[192] = { 0 };
    (void)event;

    // check the delete permission
    if (!channel || !channel->id || !(channel->access & DESKTOP_CHANNEL_DELETE) || g_channels_submitted)
    {
        return;
    }

    // store the delete request
    clib__null_memory(&g_channels_delete, sizeof(g_channels_delete));
    g_channels_delete.action = DESKTOP_CHANNEL_REMOVE;
    g_channels_delete.channel = *channel;

    // show the delete question
    clib__string_format(text, sizeof(text), "Delete channel %s?", channel->name);
    _ui_channels_internal__text("channel-delete-question", text);
    _ui_channels_internal__panel("channel-delete-panel", 1);
}

/**
 * @brief deletes only the channel version captured by the confirmation.
 * @param gui_event_t* event -> unused confirmation event
 * @return void
 */
static void _ui_channels_internal__confirm(gui_event_t *event)
{
    (void)event;

    // send the delete request
    if (g_channels_delete.action == DESKTOP_CHANNEL_REMOVE && desktop_session__channel_request(g_channels_session, &g_channels_delete))
    {
        g_channels_submitted = desktop_system__milliseconds();
        g_channels_error = g_channels_view->action_error_sequence;
        _ui_channels_internal__text("channel-form-result", "Waiting for the server to confirm deletion...");
    }
    else
    {
        _ui_channels_internal__text("channel-form-result", "The channel changed. Select it again before deleting.");
    }

    // close the confirmation
    _ui_channels_internal__cancel_delete(NULL);
}

/**
 * @brief closes the deletion confirmation without changing the editor draft.
 * @param gui_event_t* event -> unused cancellation event
 * @return void
 */
static void _ui_channels_internal__cancel_delete(gui_event_t *event)
{
    (void)event;
    clib__null_memory(&g_channels_delete, sizeof(g_channels_delete));
    _ui_channels_internal__panel("channel-delete-panel", 0);
}

/**
 * @brief applies fresh permissions without replacing unsaved fields and handles explicit save replies.
 * @return void
 */
void desktop_ui_channels__update(void)
{
    const desktop_channel_t *current = NULL_POINTER;
    uint64 access = 0;
    int64 index = 0;
    boole creating = g_channels_draft.action == DESKTOP_CHANNEL_CREATE;
    boole editable = 0;
    uint64 now = desktop_system__milliseconds();
    desktop_channel_request_t query = { 0 };

    // update the channels button
    _ui_channels_internal__enabled("channels-button", g_channels_view->connected && !g_channels_view->idle);

    // panel is not open
    if (!g_channels_open)
    {
        return;
    }

    // close when disconnected
    if (!g_channels_view->connected)
    {
        _ui_channels_internal__close(NULL);
        return;
    }

    // rebuild a changed list
    if (g_channels_count != g_channels_view->channel_count || (g_channels_count > 0 && clib__memcmp(g_channels_list, g_channels_view->channels, g_channels_count * sizeof(*g_channels_list))))
    {
        _ui_channels_internal__list();
    }

    // show the server reply
    if (g_channels_submitted && g_channels_view->channel_result_sequence != g_channels_result)
    {
        g_channels_submitted = 0;
        _ui_channels_internal__select(g_channels_view->channel_result_action == DESKTOP_CHANNEL_REMOVE ? 0 : g_channels_view->channel_result_id);
        _ui_channels_internal__text("channel-form-result", g_channels_view->detail);
    }
    g_channels_result = g_channels_view->channel_result_sequence;

    // show the server error
    if (g_channels_submitted && g_channels_view->action_error_sequence != g_channels_error)
    {
        g_channels_submitted = 0;
        _ui_channels_internal__text("channel-form-result", g_channels_view->detail);
    }

    // report the missing reply
    if (g_channels_submitted && now - g_channels_submitted >= 10000)
    {
        g_channels_submitted = 0;
        _ui_channels_internal__text("channel-form-result", "The server did not confirm the change. Review the channel before trying again.");
    }

    // calculate the edit access
    current = _ui_channels_internal__find(g_channels_draft.channel.id);
    creating = g_channels_draft.action == DESKTOP_CHANNEL_CREATE;
    editable = current && current->revision == g_channels_draft.channel.revision && !g_channels_view->idle && !g_channels_submitted;
    access = editable ? (creating ? (g_channels_view->can_create_channel || g_channels_view->can_create_temp_channel ? 31 : 0) : current->access) : 0;

    // enable allowed fields
    _ui_channels_internal__enabled("channel-edit-name", (access & DESKTOP_CHANNEL_NAME) != 0);
    _ui_channels_internal__enabled("channel-edit-description", access & DESKTOP_CHANNEL_DESCRIPTION);
    _ui_channels_internal__enabled("channel-replace-password", (creating || g_channels_draft.channel.id != 0) && (access & DESKTOP_CHANNEL_PASSWORD));
    _ui_channels_internal__enabled("channel-edit-password", _ui_channels_internal__value("channel-replace-password") && (access & DESKTOP_CHANNEL_PASSWORD));
    _ui_channels_internal__enabled("channel-edit-audio", (access & DESKTOP_CHANNEL_OPTIONS) != 0);
    _ui_channels_internal__enabled("channel-edit-video", (access & DESKTOP_CHANNEL_OPTIONS) != 0);
    _ui_channels_internal__enabled("channel-edit-limited", (creating || g_channels_draft.channel.id != 0) && (access & DESKTOP_CHANNEL_OPTIONS));
    _ui_channels_internal__enabled("channel-edit-capacity", _ui_channels_internal__value("channel-edit-limited") && (access & DESKTOP_CHANNEL_OPTIONS));
    _ui_channels_internal__enabled("channel-edit-temporary", !creating && editable && current->id != 0 && (g_channels_draft.channel.temporary ? current->can_permanent : current->can_temporary));
    for (index = 0; index < 4; index++)
    {
        _ui_channels_internal__enabled(g_channels_powers[index], (access & DESKTOP_CHANNEL_POWERS) != 0);
    }

    // enable allowed buttons
    _ui_channels_internal__enabled("channel-save", g_channels_text_ready && editable && ((access & 31) || (!creating && (current->can_temporary || current->can_permanent))));
    _ui_channels_internal__enabled("channel-add", editable && !current->temporary && (g_channels_view->can_create_channel || g_channels_view->can_create_temp_channel));
    _ui_channels_internal__enabled("channel-remove", !creating && editable && current->id != 0 && (access & DESKTOP_CHANNEL_DELETE));

    // report a changed channel
    if (!current || current->revision != g_channels_draft.channel.revision)
    {
        _ui_channels_internal__text("channel-form-result", "This channel changed. Select it again to load its current settings.");
    }

    // request access again
    if (current && !g_channels_view->idle && now - g_channels_access_time >= 5000)
    {
        query.action = DESKTOP_CHANNEL_ACCESS;
        query.channel = *current;
        if (desktop_session__channel_request(g_channels_session, &query))
        {
            g_channels_access_time = now;
        }
    }
}

/**
 * @brief registers channel-management handlers before layout parsing.
 * @param desktop_session_t* session -> application-owned connection
 * @param const desktop_session_view_t* view -> stable GUI snapshot storage
 * @return void
 */
void desktop_ui_channels__initialize(desktop_session_t *session, const desktop_session_view_t *view)
{
    // store the arguments
    g_channels_session = session;
    g_channels_view = view;

    // register the handlers
    scene__register_handler("on_show_channels", _ui_channels_internal__show);
    scene__register_handler("on_close_channels", _ui_channels_internal__close);
    scene__register_handler("on_channel_editor_choose", _ui_channels_internal__choose);
    scene__register_handler("on_channel_create", _ui_channels_internal__create);
    scene__register_handler("on_channel_save", _ui_channels_internal__save);
    scene__register_handler("on_channel_delete", _ui_channels_internal__remove);
    scene__register_handler("on_channel_delete_confirm", _ui_channels_internal__confirm);
    scene__register_handler("on_channel_delete_cancel", _ui_channels_internal__cancel_delete);
}
