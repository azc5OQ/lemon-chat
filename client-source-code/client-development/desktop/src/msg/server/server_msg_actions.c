#include "clib.h"
#include "session/session_internal.h"
#include "msg/server/server_msg.h"

/**
 * @brief accepts permission hints only for the matching identity and current channel.
 * @param desktop_session_t* session -> authenticated worker-owned connection
 * @param const cJSON* message -> borrowed authenticated server message
 * @return void
 */
void server_msg__actions__receive(desktop_session_t* session, const cJSON* message)
{
    const char* type = server_msg__read_string(message, "type");
    const char* text = NULL;
    const cJSON* actions = NULL;
    int64 slot = desktop_session__client(session, server_msg__read_number(message, "client_id"));
    desktop_client_t* client = slot >= 0 ? &session->model.clients[slot] : NULL;
    uint64 length = 0;
    int64 index = 0;
    int64 channel_slot = desktop_session__channel(session, server_msg__read_number(message, "channel_id"));
    desktop_channel_t* channel = channel_slot >= 0 ? &session->model.channels[channel_slot] : NULL;
    const char* rights[6] = {"edit_channel_name", "edit_channel_description", "edit_channel_password", "edit_channel_powers", "edit_channel_options", "delete_channel"};
    int64 capacity = 0;
    const cJSON* feature = NULL;
    const cJSON* role = NULL;
    int64 role_id = 0;
    desktop_server_info_t* info = &session->model.server_info;
    if (clib__strcmp(type, "authentication_status") == 0)
    {
        feature = cJSON_GetObjectItemCaseSensitive(message, "is_chat_picture_allowed");
        session->model.policy_pictures = !cJSON_IsBool(feature) || cJSON_IsTrue(feature);
        feature = cJSON_GetObjectItemCaseSensitive(message, "is_file_upload_allowed");
        session->model.policy_files = !cJSON_IsBool(feature) || cJSON_IsTrue(feature);
        feature = cJSON_GetObjectItemCaseSensitive(message, "is_private_messaging_allowed");
        session->model.policy_private = !cJSON_IsBool(feature) || cJSON_IsTrue(feature);
        capacity = server_msg__read_number(message, "chat_picture_max_size");
        if (capacity > 0)
        {
            session->model.picture_limit = (uint64)capacity;
        }
        capacity = server_msg__read_number(message, "file_upload_max_size");
        if (capacity > 0)
        {
            session->model.file_limit = (uint64)capacity;
        }
        session->model.permission_system = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(message, "is_permission_system_enabled"));
        if (cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(message, "is_idle_mode_allowed")))
        {
            session->model.policy_idle = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(message, "is_idle_mode_allowed"));
        }
        if (cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(message, "is_client_rename_allowed")))
        {
            session->model.policy_rename = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(message, "is_client_rename_allowed"));
        }
    }
    else if (clib__strcmp(type, "admin_status") == 0)
    {
        session->model.administrator = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(message, "is_admin"));
        if (!session->model.permission_system)
        {
            session->model.can_create_channel = session->model.administrator;
        }
    }
    else if (clib__strcmp(type, "group_list") == 0)
    {
        actions = cJSON_GetObjectItemCaseSensitive(message, "access");
        session->model.can_assign_groups = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(actions, "assign_groups"));
        session->model.can_remove_groups = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(actions, "remove_groups"));
        session->model.can_kick = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(actions, "kick"));
        session->model.can_ban = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(actions, "ban"));
        session->model.moderation_power[0] = server_msg__read_number(cJSON_GetObjectItemCaseSensitive(actions, "powers"), "kick_power");
        session->model.moderation_power[1] = server_msg__read_number(cJSON_GetObjectItemCaseSensitive(actions, "powers"), "ban_power");
        session->model.can_create_channel = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(actions, "create_channel"));
        session->model.can_create_temp_channel = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(actions, "create_temp_channel"));
        for (index = 0; index < session->model.client_count; index++)
        {
            session->model.clients[index].access_known = 0;
            session->model.clients[index].access = 0;
            clib__null_memory(session->model.clients[index].assignable_roles, sizeof(session->model.clients[index].assignable_roles));
        }
        for (index = 0; index < session->model.channel_count; index++)
        {
            session->model.channels[index].access_known = 0;
            session->model.channels[index].access = 0;
        }
    }
    else if (clib__strcmp(type, "server_info") == 0)
    {
        info->ready = TRUE;
        info->revision++;
        info->uptime = server_msg__read_number(message, "uptime_seconds");
        info->used_slots = server_msg__read_number(message, "used_slots");
        info->max_slots = server_msg__read_number(message, "max_slots");
        info->features_known = 0;
        info->features_enabled = 0;
        capacity = server_msg__read_number(message, "max_slots");
        session->model.server_capacity = capacity > 0 && capacity <= 2147483647 ? (int)capacity : 0;
        actions = cJSON_GetObjectItemCaseSensitive(message, "features");
        for (index = 0; index < DESKTOP_SERVER_FEATURES; index++)
        {
            feature = cJSON_GetObjectItemCaseSensitive(actions, desktop_server_info__feature(index)->key);
            info->features_known |= cJSON_IsBool(feature) ? 1ULL << index : 0;
            info->features_enabled |= cJSON_IsTrue(feature) ? 1ULL << index : 0;
        }
        if (!session->model.permission_system && cJSON_IsObject(actions))
        {
            session->model.can_create_temp_channel = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(actions, "is_temp_channel_creation_allowed"));
        }
    }
    else if (clib__strcmp(type, "channel_access") == 0 && channel)
    {
        actions = cJSON_GetObjectItemCaseSensitive(message, "rights");
        channel->access_known = cJSON_IsObject(actions);
        channel->access = 0;
        channel->can_temporary = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(message, "can_make_temporary"));
        channel->can_permanent = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(message, "can_make_permanent"));
        for (index = 0; index < 6; index++)
        {
            channel->access |= cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(actions, rights[index])) ? 1U << index : 0;
        }
    }
    else if (clib__strcmp(type, "client_context_access") == 0 && client
        && client->channel == server_msg__read_number(message, "channel_id")
        && clib__strcmp(session->client_keys[slot], server_msg__read_string(message, "identity")) == 0)
    {
        actions = cJSON_GetObjectItemCaseSensitive(message, "actions");
        client->access_known = cJSON_IsObject(actions);
        client->access = 0;
        clib__null_memory(client->assignable_roles, sizeof(client->assignable_roles));
        cJSON_ArrayForEach(role, cJSON_GetObjectItemCaseSensitive(message, "channel_groups"))
        {
            if (cJSON_IsNumber(role) && role->valuedouble >= 0 && role->valuedouble < DESKTOP_TREE_CHANNEL_GROUPS)
            {
                role_id = (int64)role->valuedouble;
                if ((double)role_id == role->valuedouble)
                {
                    client->assignable_roles[role_id / 64] |= 1ULL << (role_id % 64);
                }
            }
        }
        client->access |= cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(actions, "move")) ? DESKTOP_ACCESS_MOVE : 0;
        client->access |= cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(actions, "kick_from_channel")) ? DESKTOP_ACCESS_KICK_CHANNEL : 0;
        client->access |= cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(actions, "mute")) ? DESKTOP_ACCESS_MUTE_CHANNEL : 0;
        client->access |= cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(actions, "stop_stream")) ? DESKTOP_ACCESS_STOP_STREAM : 0;
    }
    else if (clib__strcmp(type, "moderation_result") == 0)
    {
        desktop_session__detail(session, server_msg__read_string(message, "text"), 0);
    }
    else if (clib__strcmp(type, "poke") == 0 && (!client || !client->ignored))
    {
        clib__string_copy(session->model.notice_sender, sizeof(session->model.notice_sender), client ? client->name : "A client");
        text = server_msg__read_string(message, "poke_message");
        length = (uint64)clib__strlen(text);
        if (length >= sizeof(session->model.notice_text))
        {
            length = sizeof(session->model.notice_text) - 1;
            while (length && ((unsigned char)text[length] & 0xc0) == 0x80)
            {
                length--;
            }
        }
        clib__string_copy_n(session->model.notice_text, sizeof(session->model.notice_text), text, length);
        session->model.notice_sequence++;
    }
}
