#include "session/session_internal.h"
#include "msg/server/server_msg.h"

// Read the old channel/group state before the normal handlers update it.
void server_msg__sounds__receive(desktop_session_t *session, const cJSON *message)
{
    const char *type = server_msg__read_string(message, "type");
    const char *reason = server_msg__read_string(message, "reason");
    int64 id = server_msg__read_number(message, "client_id");
    int64 channel = server_msg__read_number(message, "channel_id");
    int64 slot = desktop_session__client(session, id);
    int64 group = server_msg__read_number(message, "group_id");
    desktop_client_t *client = slot >= 0 ? &session->model.clients[slot] : NULL;
    desktop_sound_t sound = DESKTOP_SOUND_COUNT;

    // skip when not connected
    if (!session->model.connected)
    {
        return;
    }

    // choose the sound
    if (clib__strcmp(type, "client_connect") == 0 && !client && channel == session->model.channel)
    {
        sound = DESKTOP_SOUND_USER_ENTERED_YOUR_CHANNEL;
    }
    else if (clib__strcmp(type, "client_disconnect") == 0 && client && client->channel == session->model.channel)
    {
        sound = clib__strcmp(reason, "ban") == 0 ? DESKTOP_SOUND_USER_IN_YOUR_CHANNEL_WAS_BANNED_FROM_THE_SERVER : DESKTOP_SOUND_USER_DISCONNECTED_FROM_YOUR_CHANNEL;
    }
    else if (clib__strcmp(type, "channel_join") == 0 && client && channel != client->channel && clib__strcmp(reason, "visibility") != 0)
    {
        // compare with own channel
        if (id == session->model.local_id)
        {
            sound = DESKTOP_SOUND_CHANNEL_SWITCHED;
        }
        else if (channel == session->model.channel)
        {
            sound = clib__strcmp(reason, "kick") == 0 ? DESKTOP_SOUND_USER_WAS_KICKED_TO_YOUR_CHANNEL : clib__strcmp(reason, "move") == 0 ? DESKTOP_SOUND_USER_WAS_MOVED_TO_YOUR_CHANNEL : DESKTOP_SOUND_USER_JOINED_YOUR_CHANNEL;
        }
        else if (client->channel == session->model.channel)
        {
            sound = clib__strcmp(reason, "kick") == 0 ? DESKTOP_SOUND_USER_WAS_KICKED_OUT_OF_YOUR_CHANNEL : clib__strcmp(reason, "move") == 0 ? DESKTOP_SOUND_USER_WAS_MOVED_OUT_OF_YOUR_CHANNEL : DESKTOP_SOUND_USER_LEFT_YOUR_CHANNEL;
        }
    }
    else if (clib__strcmp(type, "client_going_to_idle_mode") == 0 && client && !client->idle && id != session->model.local_id && client->channel == session->model.channel)
    {
        // use the leave sound
        sound = DESKTOP_SOUND_USER_LEFT_YOUR_CHANNEL;
    }
    else if (clib__strcmp(type, "client_coming_back_from_idle_mode") == 0 && client && client->idle && id != session->model.local_id && channel == session->model.channel)
    {
        // use the join sound
        sound = DESKTOP_SOUND_USER_JOINED_YOUR_CHANNEL;
        desktop_voice__stop_sound(session->voice, DESKTOP_SOUND_CALL);
    }
    else if (clib__strcmp(type, "channel_create") == 0)
    {
        sound = DESKTOP_SOUND_CHANNEL_CREATED;
    }
    else if (clib__strcmp(type, "channel_edit") == 0)
    {
        sound = DESKTOP_SOUND_CHANNEL_EDITED;
    }
    else if (clib__strcmp(type, "channel_delete") == 0)
    {
        sound = DESKTOP_SOUND_CHANNEL_DELETED;
    }
    else if (client && id == session->model.local_id && group >= 0 && group < DESKTOP_TREE_GROUPS && clib__strcmp(type, "group_add_to_client") == 0 && !(client->groups[group / 64] & (1ULL << (group % 64))))
    {
        sound = DESKTOP_SOUND_SERVER_GROUP_ASSIGNED;
    }
    else if (client && id == session->model.local_id && group >= 0 && group < DESKTOP_TREE_GROUPS && clib__strcmp(type, "remove_group_from_client") == 0 && (client->groups[group / 64] & (1ULL << (group % 64))))
    {
        sound = DESKTOP_SOUND_SERVER_GROUP_REVOKED;
    }
    else if (clib__strcmp(type, "poke") == 0 && (!client || !client->ignored))
    {
        sound = DESKTOP_SOUND_POKE;
    }
    else if (clib__strcmp(type, "access_denied") == 0)
    {
        sound = DESKTOP_SOUND_INSUFFICIENT_PERMISSIONS;
    }
    else if (clib__strcmp(type, "server_settings_saved") == 0)
    {
        sound = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(message, "success")) ? DESKTOP_SOUND_YOU_EDITED_THE_SERVER : DESKTOP_SOUND_ERROR;
    }
    else if (clib__strcmp(type, "moderation_result") == 0 && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(message, "succeeded")))
    {
        sound = DESKTOP_SOUND_ERROR;
    }

    // play the sound
    if (sound != DESKTOP_SOUND_COUNT)
    {
        desktop_voice__sound(session->voice, sound);
    }
}
