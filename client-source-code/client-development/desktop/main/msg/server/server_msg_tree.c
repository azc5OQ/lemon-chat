#include "session/session_internal.h"
#include "session/tree_icons.h"
#include "msg/server/server_msg.h"
#include "platform/memory.h"

// declarations
static void _server_msg_tree_internal__country(desktop_client_t *client, const char *code);
static void _server_msg_tree_internal__group(desktop_session_t *session, const cJSON *item, boole channel);
static void _server_msg_tree_internal__icon(desktop_session_t *session, const cJSON *item, boole remove);
static void _server_msg_tree_internal__song(desktop_client_t *client, const cJSON *item);

/**
 * @brief accepts only the server's two-letter country code; empty codes clear hidden flags.
 * @param desktop_client_t* client -> worker-owned client
 * @param const char* code -> borrowed country code
 * @return void
 */
static void _server_msg_tree_internal__country(desktop_client_t *client, const char *code)
{
    int64 index = 0;
    char letter = 0;
    clib__null_memory(client->country, sizeof(client->country));
    if (clib__strlen(code) != 2)
    {
        return;
    }
    for (index = 0; index < 2; index++)
    {
        letter = code[index];
        letter = letter >= 'A' && letter <= 'Z' ? (char)(letter + 'a' - 'A') : letter;
        if (letter < 'a' || letter > 'z')
        {
            clib__null_memory(client->country, sizeof(client->country));
            return;
        }
        client->country[index] = letter;
    }
}

/**
 * @brief replaces a bounded song title, clearing old titles on a stopped stream.
 * @param desktop_client_t* client -> worker-owned client
 * @param const cJSON* item -> initial record or active-stream record
 * @return void
 */
static void _server_msg_tree_internal__song(desktop_client_t *client, const cJSON *item)
{
    const cJSON *streaming = cJSON_GetObjectItemCaseSensitive(item, "is_streaming_song");
    clib__null_memory(client->song, sizeof(client->song));
    if (!cJSON_IsFalse(streaming))
    {
        clib__string_copy(client->song, sizeof(client->song), server_msg__read_string(item, "song_name"));
    }
}

/**
 * @brief reads identity-linked tree decorations without inventing a separate identity scheme.
 * @param desktop_client_t* client -> cleared client being inserted
 * @param const cJSON* item -> borrowed client record
 * @return void
 */
void server_msg__tree__read_client(desktop_client_t *client, const cJSON *item)
{
    const cJSON *group = NULL_POINTER;
    const cJSON *groups = cJSON_GetObjectItemCaseSensitive(item, "group_ids");
    int64 id = 0;
    client->channel_group = server_msg__read_number(item, "channel_group_id");
    _server_msg_tree_internal__country(client, server_msg__read_string(item, "country_iso_code"));
    _server_msg_tree_internal__song(client, item);
    if (cJSON_IsArray(groups))
    {
        cJSON_ArrayForEach(group, groups)
        {
            if (cJSON_IsNumber(group) && group->valuedouble >= 0 && group->valuedouble < DESKTOP_TREE_GROUPS)
            {
                id = (int64)group->valuedouble;
                if ((double)id == group->valuedouble)
                {
                    client->groups[id / 64] |= 1ULL << (id % 64);
                }
            }
        }
    }
}

/**
 * @brief replaces one catalog entry, including the channel-admin built-in icon.
 * @param desktop_session_t* session -> worker-owned session
 * @param const cJSON* item -> borrowed catalog entry
 * @param boole channel -> TRUE for a channel group
 * @return void
 */
static void _server_msg_tree_internal__group(desktop_session_t *session, const cJSON *item, boole channel)
{
    int64 id = server_msg__read_number(item, "group_id");
    const cJSON *icon = cJSON_GetObjectItemCaseSensitive(item, "icon_id");
    desktop_tree_group_t *group = NULL_POINTER;
    const cJSON *access = cJSON_GetObjectItemCaseSensitive(item, "access");
    const cJSON *permissions = cJSON_GetObjectItemCaseSensitive(item, "permissions");
    const cJSON *flags = cJSON_GetObjectItemCaseSensitive(permissions, "flags");
    const char *protections[2] = { "kick_protection", "ban_protection" };
    int64 index = 0;
    if (id < 0 || id >= (channel ? DESKTOP_TREE_CHANNEL_GROUPS : DESKTOP_TREE_GROUPS))
    {
        return;
    }
    group = channel ? &session->model.channel_groups[id] : &session->model.groups[id];
    clib__null_memory(group, sizeof(*group));
    group->exists = TRUE;
    group->icon = -1;
    group->default_guest = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "is_default_guest"));
    group->can_assign = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(access, "assign_groups"));
    group->can_remove = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(access, "remove_groups"));
    for (index = 0; index < 2; index++)
    {
        group->protection[index] = server_msg__read_number(permissions, protections[index]);
        group->negate_protection[index] = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(flags, protections[index]), "negate"));
    }
    clib__string_copy(group->name, sizeof(group->name), server_msg__read_string(item, channel ? "name" : "group_name"));
    if (channel)
    {
        group->icon = cJSON_IsNumber(icon) ? (icon->valuedouble == -2 ? -2 : server_msg__read_number(item, "icon_id")) : -1;
    }
    else if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "has_icon")))
    {
        group->icon = server_msg__read_number(item, "group_linked_icon_id");
    }
}

/**
 * @brief decodes outside the publication lock and copies only completed fixed-size icon pixels under it.
 * @param desktop_session_t* session -> worker-owned session with a GUI reader
 * @param const cJSON* item -> authenticated icon record
 * @param boole remove -> TRUE to clear a deleted icon
 * @return void
 */
static void _server_msg_tree_internal__icon(desktop_session_t *session, const cJSON *item, boole remove)
{
    ubyte pixels[DESKTOP_TREE_ICON_BYTES] = { 0 };
    int64 id = server_msg__read_number(item, "icon_id");
    boole valid = FALSE;
    if (id < 0 || id >= DESKTOP_TREE_GROUPS)
    {
        return;
    }
    valid = !remove && desktop_tree_icons__decode(server_msg__read_string(item, "base64_icon"), pixels, sizeof(pixels));
    desktop_mutex__lock(session->mutex);
    session->tree_icons_revision = 0;
    clib__memcpy(session->tree_icons[id], pixels, sizeof(pixels));
    session->tree_icon_present[id] = valid;
    desktop_mutex__unlock(session->mutex);
    desktop_memory__erase(pixels, sizeof(pixels));
}

/**
 * @brief returns an isolated icon copy rather than exposing worker-owned pixels to the renderer.
 * @param desktop_session_t* session -> lifetime-stable session
 * @param int64 id -> server icon ID
 * @param uint64 revision -> expected published tree revision
 * @param ubyte* pixels -> caller-owned pixel buffer
 * @param uint64 capacity -> actual buffer capacity
 * @return boole -> TRUE if this snapshot's icon is available
 */
boole desktop_session__tree_icon(desktop_session_t *session, int64 id, uint64 revision, ubyte *pixels, uint64 capacity)
{
    boole valid = FALSE;
    if (!pixels || capacity < DESKTOP_TREE_ICON_BYTES)
    {
        return FALSE;
    }
    clib__null_memory(pixels, capacity);
    if (!session || id < 0 || id >= DESKTOP_TREE_GROUPS)
    {
        return FALSE;
    }
    desktop_mutex__lock(session->mutex);
    valid = session->tree_icons_revision == revision && session->tree_icon_present[id];
    if (valid)
    {
        clib__memcpy(pixels, session->tree_icons[id], DESKTOP_TREE_ICON_BYTES);
    }
    desktop_mutex__unlock(session->mutex);
    return valid;
}

/**
 * @brief updates flags, badges and music titles after the main dispatcher has updated membership.
 * @param desktop_session_t* session -> authenticated worker-owned session
 * @param const cJSON* message -> borrowed decoded server event
 * @return void
 */
void server_msg__tree__receive(desktop_session_t *session, const cJSON *message)
{
    const char *type = server_msg__read_string(message, "type");
    const cJSON *item = NULL_POINTER;
    const cJSON *items = NULL_POINTER;
    const cJSON *feature = NULL_POINTER;
    int64 slot = desktop_session__client(session, server_msg__read_number(message, "client_id"));
    desktop_client_t *client = slot >= 0 ? &session->model.clients[slot] : NULL;
    int64 id = server_msg__read_number(message, "group_id");
    int64 index = 0;
    boole catalog = FALSE;
    if (clib__strcmp(type, "authentication_status") == 0)
    {
        session->model.policy_song_titles = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(message, "is_music_bot_marquee_visible_to_everyone"));
    }
    else if (clib__strcmp(type, "server_info") == 0)
    {
        feature = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(message, "features"), "is_music_bot_marquee_visible_to_everyone");
        if (cJSON_IsBool(feature))
        {
            session->model.policy_song_titles = cJSON_IsTrue(feature);
        }
    }
    else if (clib__strcmp(type, "group_list") == 0)
    {
        clib__null_memory(session->model.groups, sizeof(session->model.groups));
        clib__null_memory(session->model.channel_groups, sizeof(session->model.channel_groups));
        items = cJSON_GetObjectItemCaseSensitive(message, "groups");
        cJSON_ArrayForEach(item, items)
        {
            _server_msg_tree_internal__group(session, item, FALSE);
        }
        items = cJSON_GetObjectItemCaseSensitive(message, "channel_groups");
        cJSON_ArrayForEach(item, items)
        {
            _server_msg_tree_internal__group(session, item, TRUE);
        }
        catalog = TRUE;
    }
    else if (clib__strcmp(type, "group_add") == 0)
    {
        _server_msg_tree_internal__group(session, message, FALSE);
        catalog = TRUE;
    }
    else if (id >= 0 && id < DESKTOP_TREE_GROUPS && clib__strcmp(type, "group_delete") == 0)
    {
        clib__null_memory(&session->model.groups[id], sizeof(session->model.groups[id]));
        for (index = 0; index < session->model.client_count; index++)
        {
            session->model.clients[index].groups[id / 64] &= ~(1ULL << (id % 64));
        }
        catalog = TRUE;
    }
    else if (id >= 0 && id < DESKTOP_TREE_GROUPS && clib__strcmp(type, "group_icon_changed") == 0)
    {
        session->model.groups[id].icon = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(message, "has_icon")) ? server_msg__read_number(message, "group_linked_icon_id") : -1;
        catalog = TRUE;
    }
    else if (id >= 0 && id < DESKTOP_TREE_GROUPS && client && clib__strcmp(type, "group_add_to_client") == 0)
    {
        client->groups[id / 64] |= 1ULL << (id % 64);
    }
    else if (id >= 0 && id < DESKTOP_TREE_GROUPS && client && clib__strcmp(type, "remove_group_from_client") == 0)
    {
        client->groups[id / 64] &= ~(1ULL << (id % 64));
    }
    else if (clib__strcmp(type, "icon_list") == 0)
    {
        desktop_mutex__lock(session->mutex);
        session->tree_icons_revision = 0;
        clib__null_memory(session->tree_icons, sizeof(session->tree_icons));
        clib__null_memory(session->tree_icon_present, sizeof(session->tree_icon_present));
        desktop_mutex__unlock(session->mutex);
        items = cJSON_GetObjectItemCaseSensitive(message, "icons");
        cJSON_ArrayForEach(item, items)
        {
            _server_msg_tree_internal__icon(session, item, FALSE);
        }
        catalog = TRUE;
    }
    else if (clib__strcmp(type, "icon_add") == 0 || clib__strcmp(type, "icon_delete") == 0)
    {
        _server_msg_tree_internal__icon(session, message, clib__strcmp(type, "icon_delete") == 0);
        catalog = TRUE;
    }
    else if (clib__strcmp(type, "channel_icon_changed") == 0)
    {
        slot = desktop_session__channel(session, server_msg__read_number(message, "channel_id"));
        if (slot >= 0)
        {
            session->model.channels[slot].icon = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(message, "has_channel_icon")) ? server_msg__read_number(message, "channel_icon_id") : -1;
        }
    }
    else if (client && clib__strcmp(type, "client_country_code_changed") == 0)
    {
        _server_msg_tree_internal__country(client, server_msg__read_string(message, "country_iso_code"));
    }
    else if (client && (clib__strcmp(type, "channel_group_changed") == 0 || clib__strcmp(type, "channel_join") == 0 || clib__strcmp(type, "client_coming_back_from_idle_mode") == 0))
    {
        if (client->channel == server_msg__read_number(message, "channel_id"))
        {
            client->channel_group = server_msg__read_number(message, "channel_group_id");
            if (clib__strcmp(type, "channel_group_changed") != 0)
            {
                _server_msg_tree_internal__song(client, message);
            }
        }
    }
    else if (client && clib__strcmp(type, "start_song_stream") == 0)
    {
        _server_msg_tree_internal__song(client, message);
    }
    else if (client && clib__strcmp(type, "stop_song_stream") == 0)
    {
        clib__null_memory(client->song, sizeof(client->song));
    }
    else if (clib__strcmp(type, "current_channel_active_microphone_usage") == 0)
    {
        items = cJSON_GetObjectItemCaseSensitive(message, "clients");
        cJSON_ArrayForEach(item, items)
        {
            slot = desktop_session__client(session, server_msg__read_number(item, "client_id"));
            if (slot >= 0 && session->model.clients[slot].channel == session->model.channel)
            {
                _server_msg_tree_internal__song(&session->model.clients[slot], item);
            }
        }
    }
    for (index = 0; index < session->model.client_count; index++)
    {
        client = &session->model.clients[index];
        if (client->idle || (!session->model.policy_song_titles && client->channel != session->model.channel))
        {
            clib__null_memory(client->song, sizeof(client->song));
        }
    }
    if (catalog)
    {
        session->model.tree_revision = ++session->next_tree_revision;
        desktop_mutex__lock(session->mutex);
        session->tree_icons_revision = session->model.tree_revision;
        desktop_mutex__unlock(session->mutex);
    }
}
