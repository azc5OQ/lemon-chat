#include "../../clib/clib_vector.h"
// This file checks requests to create, edit, join or leave channels and to choose a new channel maintainer.
#include "client_msg.h"
#include "../../core/permissions.h"
#include "../../core/channel.h"
#include "../../core/channel_groups.h"
#include "../server/server_msg.h"
#include "../../core/audio_channel.h"
#include "../../video_websocket.h"
#include "../../core/video_channel.h"
#include "../../base.h"
#include "../../util.h"
#include "../../memory_manager.h"
#include "../../clib/clib.h"
#include "../../../third-party/rxi-log/log.h"
#include "../../../third-party/eteran-cvector/cvector.h"

// declarations
static boole _client_msg_channel_internal__is_json_create_channel_request_valid(cJSON *json_root, uint64 client_id);
static boole _client_msg_channel_internal__is_json_edit_channel_request_valid(cJSON *json_root, uint64 client_id);
static boole _client_msg_channel_internal__is_json_join_channel_request_format_valid(cJSON *json_root, uint64 client_id);
static boole _client_msg_channel_internal__is_json_delete_request_format_valid(cJSON *json_root, uint64 client_id);
static boole _client_msg_channel_internal__is_go_to_idle_mode_request_valid(cJSON *json_root);
static boole _client_msg_channel_internal__is_process_come_back_from_idle_mode_request_valid(cJSON *json_root);
static boole _client_msg_channel_internal__can_edit(client_t *sender, channel_t *channel, cJSON *message);

/**
 * @brief Rejects fractional and out-of-range channel identifiers before array indexing.
 */
static boole _client_msg_channel_internal__is_channel_id_valid(cJSON *value)
{
    uint64 channel_id = 0;

    return (boole)(g_server_settings.max_channel_count > 0 && util__json_read_uint64(value, g_server_settings.max_channel_count - 1, &channel_id) == TRUE);
}

/**
 * @brief Validates optional channel settings and preserves omitted capacity fields.
 * @param channel NULL for validation only, otherwise the channel or permission-check candidate to update.
 */
static boole _client_msg_channel_internal__read_optional_options(cJSON *message, channel_t *channel)
{
    cJSON *limit = cJSON_GetObjectItemCaseSensitive(message, "is_client_limit_active");
    cJSON *capacity = cJSON_GetObjectItemCaseSensitive(message, "max_client_count");
    cJSON *video = cJSON_GetObjectItemCaseSensitive(message, "is_video_stream_enabled");
    cJSON *temporary = cJSON_GetObjectItemCaseSensitive(message, "is_temp_channel");
    uint64 maximum_clients = 0;

    if (limit != NULL_POINTER &&
        cJSON_IsBool(limit) == FALSE ||
        video != NULL_POINTER &&
        cJSON_IsBool(video) == FALSE ||
        temporary != NULL_POINTER &&
        cJSON_IsBool(temporary) == FALSE ||
        capacity != NULL_POINTER &&
        util__json_read_uint64(capacity, g_server_settings.max_client_count, &maximum_clients) == FALSE)
    {
        return FALSE;
    }
    if (channel != NULL_POINTER)
    {
        if (channel->is_root_channel == TRUE)
        {
            channel->is_client_limit_active = FALSE;
            channel->max_client_count = 0;
        }
        else
        {
            if (limit != NULL_POINTER)
            {
                channel->is_client_limit_active = (boole)cJSON_IsTrue(limit);
            }
            if (capacity != NULL_POINTER)
            {
                channel->max_client_count = maximum_clients;
            }
        }
    }
    return TRUE;
}

/**
 * @brief Checks every changed field before applying a channel edit.
 * @param sender Authenticated sender with the clients lock held.
 * @param channel Existing channel with the channels lock held.
 * @param message Validated edit request.
 */
static boole _client_msg_channel_internal__can_edit(client_t *sender, channel_t *channel, cJSON *message)
{
    boole status0 = FALSE;
    cJSON *value = NULL_POINTER;
    channel_t proposed = *channel;

    value = cJSON_GetObjectItemCaseSensitive(message, "channel_name");
    if (clib__strcmp(value->valuestring, channel->name) != 0 && !channel_groups__is_allowed(sender, channel, PERMISSION_EDIT_CHANNEL_NAME))
    {
        return FALSE;
    }
    value = cJSON_GetObjectItemCaseSensitive(message, "channel_description");
    if (clib__strcmp(value->valuestring, channel->description) != 0 && !channel_groups__is_allowed(sender, channel, PERMISSION_EDIT_CHANNEL_DESCRIPTION))
    {
        return FALSE;
    }
    value = cJSON_GetObjectItemCaseSensitive(message, "channel_password");
    status0 = ((value != NULL_POINTER) && (!channel->is_root_channel)) && (!channel_groups__is_allowed(sender, channel, PERMISSION_EDIT_CHANNEL_PASSWORD));
    if (status0 == TRUE)
    {
        return FALSE;
    }
    channel__read_required_powers(message, &proposed);
    status0 = (proposed.required_talk_power != channel->required_talk_power || proposed.required_message_power != channel->required_message_power || proposed.required_picture_power != channel->required_picture_power || proposed.required_stream_power != channel->required_stream_power) && (!channel_groups__is_allowed(sender, channel, PERMISSION_EDIT_CHANNEL_POWERS));
    if (status0 == TRUE)
    {
        return FALSE;
    }
    value = cJSON_GetObjectItemCaseSensitive(message, "is_audio_enabled");
    proposed.is_audio_enabled = (boole)cJSON_IsTrue(value);
    value = cJSON_GetObjectItemCaseSensitive(message, "is_video_stream_enabled");
    if (cJSON_IsBool(value))
    {
        proposed.is_video_stream_enabled = (boole)cJSON_IsTrue(value);
    }
    if (_client_msg_channel_internal__read_optional_options(message, &proposed) == FALSE)
    {
        return FALSE;
    }
    status0 = (proposed.is_audio_enabled != channel->is_audio_enabled || proposed.is_video_stream_enabled != channel->is_video_stream_enabled || proposed.is_client_limit_active != channel->is_client_limit_active || proposed.max_client_count != channel->max_client_count) && (!channel_groups__is_allowed(sender, channel, PERMISSION_EDIT_CHANNEL_OPTIONS));
    if (status0 == TRUE)
    {
        return FALSE;
    }
    return TRUE;
}

/**
 * @brief processes an admin request to set (or clear) a channel's icon. an icon_id in the request
 *        assigns that icon (which must exist); a request without icon_id clears the channel's icon.
 *        broadcasts the change so every client updates its channel row live. only an authenticated
 *        admin may do this.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__channel__process_set_icon(cJSON *json_root, uint64 sender_client_id)
{
    client_t *sender_client = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_channel_id = NULL_POINTER;
    cJSON *json_icon_id = NULL_POINTER;
    uint64 channel_id = 0;
    uint64 icon_id = 0;
    boole wants_icon = FALSE;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
    json_icon_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "icon_id");

    if (g_server_settings.max_channel_count == 0 || util__json_read_uint64(json_channel_id, g_server_settings.max_channel_count - 1, &channel_id) == FALSE)
    {
        return;
    }

    // Omission explicitly clears the icon; a malformed supplied value never does.
    wants_icon = (boole)(json_icon_id != NULL_POINTER);
    if (wants_icon == TRUE && util__json_read_uint64(json_icon_id, MAX_ICONS - 1, &icon_id) == FALSE)
    {
        return;
    }

    // lock order: clients -> channels -> icons
    clib__write_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    if (g_server_settings.is_permission_system_enabled == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], "Server groups and icons are disabled.");
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    clib__write_lock(&g_channels_global_rwlock_guard);
    clib__read_lock(&g_icons_global_rwlock_guard);

    sender_client = &g_clients_array[sender_client_id];

    if (channel_id >= g_server_settings.max_channel_count)
    {
        goto label_client_msg__process_set_channel_icon_end;
    }
    if (channel_groups__is_allowed(sender_client, &g_channel_array[channel_id], PERMISSION_EDIT_CHANNEL_OPTIONS) == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(sender_client, NULL_POINTER);
        goto label_client_msg__process_set_channel_icon_end;
    }

    if (g_channel_array[channel_id].is_existing == FALSE)
    {
        goto label_client_msg__process_set_channel_icon_end;
    }

    if (wants_icon == TRUE)
    {
        // the assigned icon must exist (an icon id equals its index)
        if (icon_id >= MAX_ICONS || g_icons_array[icon_id].is_existing == FALSE)
        {
            goto label_client_msg__process_set_channel_icon_end;
        }

        g_channel_array[channel_id].has_channel_icon = TRUE;
        g_channel_array[channel_id].icon_id = icon_id;
    }
    else
    {
        // no icon_id in the request -> clear the channel's icon
        g_channel_array[channel_id].has_channel_icon = FALSE;
        g_channel_array[channel_id].icon_id = 0;
    }

    server_msg__channel__send_icon_changed_event_to_all_clients(channel_id, g_channel_array[channel_id].has_channel_icon, g_channel_array[channel_id].icon_id);

label_client_msg__process_set_channel_icon_end:
    clib__unlock(&g_icons_global_rwlock_guard);
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @attention
 *
 * @return boole
 */
static boole _client_msg_channel_internal__is_json_create_channel_request_valid(cJSON *json_root, uint64 client_id)
{
    cJSON *json_parent_channel_id = NULL_POINTER;
    cJSON *json_channel_name = NULL_POINTER;
    cJSON *json_channel_description = NULL_POINTER;
    cJSON *json_channel_password = NULL_POINTER;
    cJSON *json_is_audio_enabled = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;
    int64 status = 0;

    // existence of "message" already checked before
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (channel__read_required_powers(json_message_object, NULL_POINTER) == FALSE || _client_msg_channel_internal__read_optional_options(json_message_object, NULL_POINTER) == FALSE)
    {
        return FALSE;
    }

    // json_message_object exists, continue validating received json
    json_parent_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "parent_channel_id");
    if (_client_msg_channel_internal__is_channel_id_valid(json_parent_channel_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(json_parent_channel_id) \n");
        return FALSE;
    }

    json_channel_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_name");
    if (cJSON_IsString(json_channel_name) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(channel_name) \n");
        return FALSE;
    }

    if (json_channel_name->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " channel_name->valuestring == NULL \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_channel_name->valuestring) == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " clib__utf8_string_length(channel_name->valuestring) \n");
        return FALSE;
    }

    json_channel_description = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_description");
    if (cJSON_IsString(json_channel_description) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(channel_description) \n");
        return FALSE;
    }

    if (json_channel_description->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " channel_description->valuestring == NULL \n");
        return FALSE;
    }

    json_channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");
    if (cJSON_IsString(json_channel_password) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_channel_password) \n");
        return FALSE;
    }

    if (json_channel_password->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_channel_password->valuestring == NULL \n");
        return FALSE;
    }

    json_is_audio_enabled = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_audio_enabled");
    if (cJSON_IsBool(json_is_audio_enabled) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsBool(json_is_audio_enabled) \n");
        return FALSE;
    }

    status = clib__utf8_string_length_check_max_length(json_channel_name->valuestring, 50);

    if (status == -1)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "clib__utf8_string_length_check_max_length(json_channel_name->valuestring, 50); \n");
        return FALSE;
    }

    status = clib__utf8_string_length_check_max_length(json_channel_description->valuestring, CHANNEL_DESCRIPTION_MAX_LENGTH - 1);

    if (status == -1)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "clib__utf8_string_length_check_max_length(json_channel_description->valuestring, CHANNEL_DESCRIPTION_MAX_LENGTH - 1); \n");
        return FALSE;
    }

    status = clib__utf8_string_length_check_max_length(json_channel_password->valuestring, 30);

    if (status == -1)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "clib__utf8_string_length_check_max_length(json_channel_password->valuestring, 30); \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief This function processes create channel request message
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @attention this function aquires read and write locks for channel_array and clients_array
 *
 * @return void
 */
void client_msg__channel__process_create_request(cJSON *json_root, uint64 sender_client_id)
{
    boole is_channel_create_allowed = FALSE;
    boole is_parent_channel_id_existing = FALSE;
    boole creating_temp_channel = FALSE;
    boole parent_is_temp_channel = FALSE;
    cJSON *json_parent_channel_id = NULL_POINTER;
    cJSON *json_channel_description = NULL_POINTER;
    cJSON *json_channel_password = NULL_POINTER;
    cJSON *json_channel_name = NULL_POINTER;
    cJSON *json_is_audio_enabled = NULL_POINTER;
    cJSON *json_is_video_stream_enabled = NULL_POINTER;
    boole is_password_used = FALSE;
    boole is_channel_created_successfully = FALSE;
    cJSON *json_message_object = NULL_POINTER;
    uint64 i = 0;
    uint64 created_channel_index = 0;
    uint64 existing_channel_count = 0;
    uint64 channel_creator_client_id = 0;
    char creator_identity[IDENTITY_HASH_MAX_LENGTH] = { 0 };
    channel_t *channel = NULL_POINTER;

    // check timestamp first
    // check if json is valid
    // check access rights
    // check if parent channel id is valid, if parent channel is is_existing
    // finally, create channel
    if (base__is_request_allowed_based_on_spam_protection(sender_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_create_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    if (_client_msg_channel_internal__is_json_create_channel_request_valid(json_root, sender_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_create_request client_msg__is_json_create_channel_request_valid == FALSE \n");
        return;
    }

    // Keep the clients lock until creation finishes so group changes cannot alter this decision.
    is_channel_create_allowed = FALSE;
    clib__write_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    if (g_clients_array[sender_client_id].is_authenticated == TRUE && g_clients_array[sender_client_id].is_existing == TRUE)
    {
        if (permissions__is_allowed(&g_clients_array[sender_client_id], PERMISSION_CREATE_CHANNEL) == TRUE)
        {
            is_channel_create_allowed = TRUE;
        }
        else if ((g_server_settings.is_permission_system_enabled == TRUE || g_server_settings.is_temp_channel_creation_allowed == TRUE) && g_clients_array[sender_client_id].is_temp_admin_channel == FALSE && permissions__is_allowed(&g_clients_array[sender_client_id], PERMISSION_CREATE_TEMP_CHANNEL) == TRUE)
        {
            is_channel_create_allowed = TRUE;
            creating_temp_channel = TRUE;
        }
    }

    if (is_channel_create_allowed == TRUE)
    {
        // what is done here:
        // checking if parent channel exists,
        // creating the child channel
        // both need to be wrapper within same write lock because they are tied to one another,
        clib__write_lock(&g_channels_global_rwlock_guard);

        // Count occupied channels, not their IDs: deleting a channel leaves a reusable hole.
        // The clients and channels locks also serialize this check with creates and admin saves.
        for (i = 0; i < g_server_settings.max_channel_count; i++)
        {
            if (g_channel_array[i].is_existing == TRUE)
            {
                existing_channel_count++;
            }
        }
        if (existing_channel_count >= g_server_settings.channel_count_limit)
        {
            clib__unlock(&g_channels_global_rwlock_guard);
            server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], "The server's maximum channel count has been reached.");
            clib__unlock(&g_clients_global_rwlock_guard);
            return;
        }
        json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
        json_parent_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "parent_channel_id");
        json_channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");
        json_channel_description = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_description");
        json_channel_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_name");
        json_is_audio_enabled = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_audio_enabled");

        for (i = 0; i < g_server_settings.max_channel_count; i++)
        {
            if (g_channel_array[i].is_existing == FALSE)
            {
                continue;
            }

            if (g_channel_array[i].channel_id == (int64)json_parent_channel_id->valuedouble)
            {
                is_parent_channel_id_existing = TRUE;
                parent_is_temp_channel = g_channel_array[i].is_temp_channel;
                break;
            }
        }

        if (is_parent_channel_id_existing == TRUE && parent_is_temp_channel == TRUE)
        {
            // no channel (not even an admin's) may be created under a temp channel
            DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_create_request parent is a temp channel, refusing to create a child under it \n");
        }
        else if (is_parent_channel_id_existing == TRUE)
        {
            DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_create_request is_parent_channel_id_existing == TRUE \n");

            for (i = 0; i < g_server_settings.max_channel_count; i++)
            {
                channel = &g_channel_array[i];

                if (channel->is_existing == FALSE)
                {
                    // everything is in order, now create the channel
                    // set is_channel_created_successfully to TRUE so message about channel creation will be sent to all clients
                    // will be initiated later in code
                    channel->is_existing = TRUE;
                    channel->channel_id = i; // a channel's id is its index in the channel array
                    channel->parent_channel_id = (int64)json_parent_channel_id->valuedouble;
                    channel->is_root_channel = FALSE;
                    clib__string_copy(channel->name, sizeof(channel->name), json_channel_name->valuestring);
                    clib__null_memory(channel->password, CHANNEL_PASSWORD_MAX_LENGTH);
                    if (clib__utf8_string_length(json_channel_password->valuestring) > 0)
                    {
                        util__hash_password_to_base64(json_channel_password->valuestring, channel->password, CHANNEL_PASSWORD_MAX_LENGTH);
                    }
                    clib__string_copy(channel->description, sizeof(channel->description), json_channel_description->valuestring);
                    channel->maintainer_id = -1;
                    channel->is_channel_maintainer_present = FALSE;
                    channel->maintainer_generation++;
                    is_password_used = (boole)(clib__utf8_string_length(json_channel_password->valuestring) > 0);
                    channel->is_using_password = is_password_used;
                    channel->is_audio_enabled = (boole)cJSON_IsTrue(json_is_audio_enabled);
                    channel->required_talk_power = 0;
                    channel->required_message_power = 0;
                    channel->required_picture_power = 0;
                    channel->required_stream_power = 0;
                    channel__read_required_powers(json_message_object, channel);

                    // optional in the message (older clients never send it): a new channel streams unless told otherwise
                    json_is_video_stream_enabled = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_video_stream_enabled");
                    channel->is_video_stream_enabled = (boole)(cJSON_IsBool(json_is_video_stream_enabled) ? cJSON_IsTrue(json_is_video_stream_enabled) : TRUE);
                    channel->is_video_stream_active = FALSE;
                    channel->video_streamer_client_id = 0;
                    channel->is_temp_channel = creating_temp_channel;
                    channel->is_client_limit_active = FALSE;
                    channel->max_client_count = 0;
                    _client_msg_channel_internal__read_optional_options(json_message_object, channel);

                    DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__channel__process_create_request is_parent_channel_id_existing ", i, "\n");
                    is_channel_created_successfully = TRUE;
                    created_channel_index = i;
                    channel_creator_client_id = sender_client_id;
                    if (g_server_settings.is_permission_system_enabled)
                    {
                        util__hash_password_to_base64(g_clients_array[sender_client_id].public_key, creator_identity, sizeof(creator_identity));
                        channel_groups__set(channel, creator_identity, CHANNEL_GROUP_ADMIN);
                    }

                    break;
                }
            }
        }
        else
        {
            DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_create_request is_parent_channel_id_existing == FALSE \n");
        }
        clib__unlock(&g_channels_global_rwlock_guard);

        // okay, channel created successfully, acquire read lock for channels and clients
        if (is_channel_created_successfully == TRUE)
        {
            // announce the new channel to everyone first, so it exists in every client's UI before the
            // temp creator is moved into it below
            clib__read_lock(&g_channels_global_rwlock_guard);

            server_msg__channel__send_create_message_to_all_clients(&g_channel_array[created_channel_index], &g_clients_array[channel_creator_client_id]);

            clib__unlock(&g_channels_global_rwlock_guard);

            // a temp channel: mark the creator as its admin/owner (so a second temp channel is refused, and
            // so the join / disconnect / delete paths know whose departure destroys it) and move them into
            // it right away, the same server-side way the delete path moves clients to root
            if (creating_temp_channel == TRUE)
            {
                clib__write_lock(&g_channels_global_rwlock_guard);

                g_clients_array[channel_creator_client_id].is_temp_admin_channel = TRUE;
                g_clients_array[channel_creator_client_id].temp_channel_id = created_channel_index;
                channel__move_client_into_channel(channel_creator_client_id, created_channel_index, NULL_POINTER);

                clib__unlock(&g_channels_global_rwlock_guard);
                server_msg__administration__send_permission_update_to_single_client(&g_clients_array[channel_creator_client_id]);
            }
        }
    }
    else
    {
        if (g_clients_array[sender_client_id].is_authenticated == TRUE && g_clients_array[sender_client_id].is_existing == TRUE)
        {
            server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], NULL_POINTER);
        }
    }
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @attention
 *
 * @return boole
 */
static boole _client_msg_channel_internal__is_json_edit_channel_request_valid(cJSON *json_root, uint64 client_id)
{
    cJSON *json_channel_id = NULL_POINTER;
    cJSON *json_channel_name = NULL_POINTER;
    cJSON *json_channel_description = NULL_POINTER;
    cJSON *json_channel_password = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_is_audio_enabled = NULL_POINTER;
    cJSON *json_type = NULL_POINTER;
    int64 status = 0;

    // existence of "message" already checked before
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (channel__read_required_powers(json_message_object, NULL_POINTER) == FALSE || _client_msg_channel_internal__read_optional_options(json_message_object, NULL_POINTER) == FALSE)
    {
        return FALSE;
    }

    json_type = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_temp_channel");
    if (json_type != NULL_POINTER && !cJSON_IsBool(json_type))
    {
        return FALSE;
    }

    // json_message_object exists, continue validating received json
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
    if (_client_msg_channel_internal__is_channel_id_valid(json_channel_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(channel_id) \n");
        return FALSE;
    }

    // the root channel IS editable (renaming it, description, audio): only deleting it is
    // forbidden, and that goes through a different request
    if (!(json_channel_id->valuedouble >= 0 && json_channel_id->valuedouble < g_server_settings.max_channel_count) || json_channel_id->valuedouble != json_channel_id->valueint)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_channel_id->valueint < 0 || (json_channel_id->valueint >= (g_server_settings.max_channel_count - 1)) \n");
        return FALSE;
    }

    json_channel_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_name");
    if (cJSON_IsString(json_channel_name) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(channel_name) \n");
        return FALSE;
    }

    if (json_channel_name->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " channel_name->valuestring == NULL \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_channel_name->valuestring) == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " clib__utf8_string_length(channel_name->valuestring) \n");
        return FALSE;
    }

    json_channel_description = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_description");
    if (cJSON_IsString(json_channel_description) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(channel_description) \n");
        return FALSE;
    }

    if (json_channel_description->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " channel_description->valuestring == NULL \n");
        return FALSE;
    }

    json_channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");
    if (json_channel_password != NULL_POINTER && cJSON_IsString(json_channel_password) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_channel_password) \n");
        return FALSE;
    }

    if (json_channel_password != NULL_POINTER && json_channel_password->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_channel_password->valuestring == NULL \n");
        return FALSE;
    }

    json_is_audio_enabled = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_audio_enabled");
    if (cJSON_IsBool(json_is_audio_enabled) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsBool(json_is_audio_enabled) \n");
        return FALSE;
    }

    status = clib__utf8_string_length_check_max_length(json_channel_name->valuestring, 50);

    if (status == -1)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "clib__utf8_string_length_check_max_length(json_channel_name->valuestring, 50); \n");
        return FALSE;
    }

    status = clib__utf8_string_length_check_max_length(json_channel_description->valuestring, CHANNEL_DESCRIPTION_MAX_LENGTH - 1);

    if (status == -1)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "clib__utf8_string_length_check_max_length(json_channel_description->valuestring, CHANNEL_DESCRIPTION_MAX_LENGTH - 1); \n");
        return FALSE;
    }

    if (json_channel_password != NULL_POINTER)
    {
        status = clib__utf8_string_length_check_max_length(json_channel_password->valuestring, 30);
        if (status == -1)
        {
            return FALSE;
        }
    }

    return TRUE;
}

/**
 * @brief This function processes edit channel request message
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @attention this function aquires read and write locks for channel_array and clients_array
 *
 * @return void
 */
void client_msg__channel__process_edit_request(cJSON *json_root, uint64 sender_client_id)
{
    boole is_channel_edit_allowed = FALSE;
    cJSON *json_channel_id = NULL_POINTER;
    cJSON *json_channel_description = NULL_POINTER;
    cJSON *json_channel_password = NULL_POINTER;
    cJSON *json_channel_name = NULL_POINTER;
    boole is_password_used = FALSE;
    boole is_channel_edited_successfully = FALSE;
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_is_audio_enabled = NULL_POINTER;
    cJSON *json_is_video_stream_enabled = NULL_POINTER;
    boole should_stop_video_stream = FALSE;
    uint64 channel_index_to_edit = 0;
    channel_t *channel = NULL_POINTER;
    cJSON *json_type = NULL_POINTER;
    cstring type_error = NULL_POINTER;
    boole type_changed = FALSE;
    uint64 member_id = 0;

    if (base__is_request_allowed_based_on_spam_protection(sender_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_edit_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    if (_client_msg_channel_internal__is_json_edit_channel_request_valid(json_root, sender_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_edit_request client_msg__is_json_edit_channel_request_valid == FALSE \n");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
    clib__write_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    clib__write_lock(&g_channels_global_rwlock_guard);
    if (_client_msg_channel_internal__is_channel_id_valid(json_channel_id) == FALSE)
    {
        clib__unlock(&g_channels_global_rwlock_guard);
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    channel = &g_channel_array[json_channel_id->valueint];
    is_channel_edit_allowed = (boole)(base__is_client_valid(sender_client_id) && channel->is_existing && _client_msg_channel_internal__can_edit(&g_clients_array[sender_client_id], channel, json_message_object));

    json_type = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_temp_channel");
    if (is_channel_edit_allowed && json_type != NULL_POINTER)
    {
        type_error = channel__type_change_error(&g_clients_array[sender_client_id], channel, (boole)cJSON_IsTrue(json_type));
        is_channel_edit_allowed = type_error == NULL_POINTER;
    }

    if (is_channel_edit_allowed == TRUE)
    {
        json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
        json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
        json_channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");
        json_channel_description = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_description");
        json_channel_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_name");
        json_is_audio_enabled = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_audio_enabled");

        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_edit_request got here \n");

        channel_index_to_edit = json_channel_id->valueint;

        channel = &g_channel_array[channel_index_to_edit];

        if (channel->is_existing == TRUE)
        {
            type_changed = json_type != NULL_POINTER && channel->is_temp_channel != (boole)cJSON_IsTrue(json_type);
            if (type_changed)
            {
                channel__set_temporary(&g_clients_array[sender_client_id], channel, (boole)cJSON_IsTrue(json_type));
            }
            clib__null_memory(channel->name, CHANNEL_NAME_MAX_LENGTH);
            clib__null_memory(channel->description, CHANNEL_DESCRIPTION_MAX_LENGTH);

            clib__string_copy(channel->name, sizeof(channel->name), json_channel_name->valuestring);

            // Keep the password when the edit request does not include a replacement.
            if (json_channel_password != NULL_POINTER || channel->is_root_channel == TRUE)
            {
                clib__null_memory(channel->password, CHANNEL_PASSWORD_MAX_LENGTH);
                is_password_used = (boole)(channel->is_root_channel == FALSE && json_channel_password != NULL_POINTER && clib__utf8_string_length(json_channel_password->valuestring) > 0);
                if (is_password_used == TRUE)
                {
                    util__hash_password_to_base64(json_channel_password->valuestring, channel->password, CHANNEL_PASSWORD_MAX_LENGTH);
                }
                channel->is_using_password = is_password_used;
            }
            clib__string_copy(channel->description, sizeof(channel->description), json_channel_description->valuestring);

            channel->is_audio_enabled = (boole)cJSON_IsTrue(json_is_audio_enabled);
            channel__read_required_powers(json_message_object, channel);

            // the video toggle is optional in the message (older clients never send it) and then keeps its
            // value. switching it off under a running stream ends that stream, after the locks below
            json_is_video_stream_enabled = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_video_stream_enabled");
            if (cJSON_IsBool(json_is_video_stream_enabled))
            {
                channel->is_video_stream_enabled = (boole)cJSON_IsTrue(json_is_video_stream_enabled);
                should_stop_video_stream = (boole)(channel->is_video_stream_enabled == FALSE && channel->is_video_stream_active == TRUE);
            }

            // the root channel is always unlimited: every client starts there on login, and a
            // capacity (like a password) would leave people with nowhere to go
            if (channel->is_root_channel == TRUE)
            {
                channel->is_client_limit_active = FALSE;
                channel->max_client_count = 0;
            }
            else
            {
                _client_msg_channel_internal__read_optional_options(json_message_object, channel);
            }

            DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_edit_request channel is_existing TRUE \n");
            is_channel_edited_successfully = TRUE;
        }

        clib__unlock(&g_channels_global_rwlock_guard);
        audio_channel__apply_permission_changes();
        video_channel__apply_permission_changes();

        // the toggle went off under a running stream: end it, with the write locks in lock order
        if (should_stop_video_stream == TRUE)
        {
            clib__write_lock(&g_channels_global_rwlock_guard);
            video_channel__process_channel_toggle_changed(channel_index_to_edit);
            clib__unlock(&g_channels_global_rwlock_guard);
        }

        // channel edited successfully, acquire read lock for channels and clients
        if (is_channel_edited_successfully == TRUE)
        {
            clib__read_lock(&g_channels_global_rwlock_guard);

            server_msg__channel__send_edit_message_to_all_clients(&g_channel_array[channel_index_to_edit], &g_clients_array[sender_client_id]);
            if (type_changed)
            {
                for (member_id = 0; member_id < g_server_settings.max_client_count; member_id++)
                {
                    if (base__is_client_valid(member_id) && g_clients_array[member_id].channel_id == channel_index_to_edit)
                    {
                        server_msg__moderation__send_channel_access(&g_clients_array[member_id], channel);
                        server_msg__moderation__send_channel_group_changed(&g_clients_array[member_id]);
                    }
                }
            }

            clib__unlock(&g_channels_global_rwlock_guard);
            if (type_changed)
            {
                server_msg__administration__send_permission_updates_to_all_clients();
            }
        }
    }
    else
    {
        clib__unlock(&g_channels_global_rwlock_guard);
        if (g_clients_array[sender_client_id].is_authenticated == TRUE && g_clients_array[sender_client_id].is_existing == TRUE)
        {
            server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], type_error);
        }
    }
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @return boole
 */
static boole _client_msg_channel_internal__is_json_join_channel_request_format_valid(cJSON *json_root, uint64 client_id)
{
    cJSON *json_channel_id = NULL_POINTER;
    cJSON *json_channel_password = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;

    // existence of "message" has already been checked at this point
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    // json_message_object exists, continue validating received json
    json_channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");
    if (cJSON_IsString(json_channel_password) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_channel_password) \n");
        return FALSE;
    }

    if (json_channel_password->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_channel_password->valuestring == NULL \n");
        return FALSE;
    }

    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
    if (_client_msg_channel_internal__is_channel_id_valid(json_channel_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(json_channel_id) \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief This function processes join channel request
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__channel__process_join_request(cJSON *json_root, uint64 sender_client_id)
{
    cJSON *json_channel_password = NULL_POINTER;
    cJSON *json_channel_id = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;
    channel_t *new_channel = NULL_POINTER;
    channel_t *old_channel = NULL_POINTER;
    client_t *client_that_is_joining_channel = NULL_POINTER;
    boole is_maintainer_found = FALSE;
    boole is_authenticated = FALSE;
    boole is_existing = FALSE;
    boole channel_join_announced = FALSE;
    boole ownership_changed = FALSE;

    if (base__is_request_allowed_based_on_spam_protection(sender_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_join_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    if (_client_msg_channel_internal__is_json_join_channel_request_format_valid(json_root, sender_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_join_request client_msg__is_json_join_channel_request_format_valid == FALSE \n");
        return;
    }

    // Keep membership changes and their announcements in the same order for every client.
    clib__write_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    clib__write_lock(&g_channels_global_rwlock_guard);

    is_authenticated = g_clients_array[sender_client_id].is_authenticated;
    is_existing = g_clients_array[sender_client_id].is_existing;

    if (is_existing == FALSE || is_authenticated == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_join_request is_existing == FALSE || is_authenticated == FALSE \n");
        goto client_msg__process_join_channel_request_end;
    }

    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_join_request got here \n");
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
    json_channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");

    if (_client_msg_channel_internal__is_channel_id_valid(json_channel_id) == FALSE)
    {
        goto client_msg__process_join_channel_request_end;
    }
    new_channel = &g_channel_array[json_channel_id->valueint];

    if (new_channel->is_existing == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_join_request channel not is_existing \n");
        goto client_msg__process_join_channel_request_end;
    }

    client_that_is_joining_channel = &g_clients_array[sender_client_id];
    old_channel = &g_channel_array[client_that_is_joining_channel->channel_id];

    if (client_that_is_joining_channel->channel_id == json_channel_id->valueint)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_join_request client is already in that channel \n");
        goto client_msg__process_join_channel_request_end;
    }

    if (channel__check_channel_access(client_that_is_joining_channel, new_channel, json_channel_password->valuestring, FALSE) == FALSE)
    {
        goto client_msg__process_join_channel_request_end;
    }

    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request_continue  \n");

    // a streamer leaving ends its stream in the old channel, a viewer leaving drops off the streamer's count
    video_channel__process_client_leaving_channel(client_that_is_joining_channel, old_channel->channel_id);

    // change channel in client struct
    client_that_is_joining_channel->channel_id = json_channel_id->valueint;
    client_that_is_joining_channel->has_pending_maintainer_reset_vote = FALSE; // channel changed - a pending reset vote belongs to the old channel

    // if the client owns this temp channel and is now leaving it, destroy it: any remaining members move
    // to root and the channel is freed. old_channel is then zeroed, so the maintainer transfer code below
    // naturally takes the no-old-maintainer path
    if (old_channel->is_temp_channel == TRUE && client_that_is_joining_channel->is_temp_admin_channel == TRUE && client_that_is_joining_channel->temp_channel_id == old_channel->channel_id)
    {
        client_that_is_joining_channel->is_temp_admin_channel = FALSE;
        client_that_is_joining_channel->temp_channel_id = 0;
        ownership_changed = TRUE;

        // This client may become root's maintainer when the temporary channel closes.
        server_msg__channel__send_join_message_to_all_clients(client_that_is_joining_channel, new_channel, NULL_POINTER);
        channel_join_announced = TRUE;
        channel__destroy_temp_channel(old_channel->channel_id);
    }

    is_maintainer_found = channel__replace_departed_maintainer(old_channel, sender_client_id);
    if (channel_join_announced == FALSE)
    {
        server_msg__channel__send_join_message_to_all_clients(client_that_is_joining_channel, new_channel, NULL_POINTER);
    }
    channel__send_hidden_members(client_that_is_joining_channel, new_channel);
    if (is_maintainer_found == TRUE)
    {
        server_msg__channel__send_maintainer_id_to_clients_in_same_channel(old_channel, old_channel->maintainer_id);
    }

    audio_channel__process_client_channel_join(client_that_is_joining_channel);

    // a stream running in the new channel: its streamer is asked whether the newcomer may watch
    video_channel__process_client_joined_channel(client_that_is_joining_channel);

    // Tell the arriving client who maintains the channel's keys.
    channel__welcome_client(client_that_is_joining_channel);

    server_msg__audio__send_active_microphone_usage_for_current_channel_to_single_client(client_that_is_joining_channel, client_that_is_joining_channel->channel_id);

client_msg__process_join_channel_request_end:
    clib__unlock(&g_channels_global_rwlock_guard);
    audio_channel__apply_permission_changes();
    video_channel__apply_permission_changes();
    if (ownership_changed == TRUE)
    {
        server_msg__administration__send_permission_update_to_single_client(client_that_is_joining_channel);
    }
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @return boole
 */
static boole _client_msg_channel_internal__is_json_delete_request_format_valid(cJSON *json_root, uint64 client_id)
{
    cJSON *json_channel_id = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;

    // existence of "message" has already been checked at this point
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    // json_message_object exists, continue validating received json
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
    if (_client_msg_channel_internal__is_channel_id_valid(json_channel_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(json_channel_id) \n");
        return FALSE;
    }

    if (json_channel_id->valueint == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " root channel cannot be deleted \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief This function processes delete channel request
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @note hmmm
 *
 * @return void
 */
void client_msg__channel__process_delete_request(cJSON *json_root, uint64 sender_client_id)
{
    boole is_authenticated = FALSE;
    boole is_existing = FALSE;
    cJSON *json_channel_id = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;
    uint64 *channel_ids_to_delete = NULL_POINTER;
    uint64 channels_to_delete_count = 0;
    client_t *client_to_move_maybe = NULL_POINTER;
    uint64 channel_id_to_delete = 0;
    uint64 client_count_in_channel = 0;
    uint64 index_of_new_maintainer = 0;
    boole is_channel_delete_allowed = FALSE;
    uint64 marked_channel_index = 0;
    uint64 client_id = 0;
    uint64 *owners_to_update = NULL_POINTER;
    uint64 owner_index = 0;

    if (base__is_request_allowed_based_on_spam_protection(sender_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_delete_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    if (_client_msg_channel_internal__is_json_delete_request_format_valid(json_root, sender_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_delete_request _client_msg_channel_internal__is_json_delete_request_format_valid == FALSE \n");
        return;
    }

    // add admin rights check later
    clib__write_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    clib__write_lock(&g_channels_global_rwlock_guard);

    is_authenticated = g_clients_array[sender_client_id].is_authenticated;
    is_existing = g_clients_array[sender_client_id].is_existing;

    if (is_existing == FALSE || is_authenticated == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_delete_request is_existing == FALSE || is_authenticated == FALSE \n");
        goto label_client_msg__process_delete_channel_request_end;
    }

    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_delete_request got here \n");
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");

    if (_client_msg_channel_internal__is_channel_id_valid(json_channel_id) == FALSE)
    {
        goto label_client_msg__process_delete_channel_request_end;
    }
    if (g_channel_array[json_channel_id->valueint].is_existing == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_delete_request channel does not exist \n");
        goto label_client_msg__process_delete_channel_request_end;
    }

    is_channel_delete_allowed = channel_groups__is_allowed(&g_clients_array[sender_client_id], &g_channel_array[json_channel_id->valueint], PERMISSION_DELETE_CHANNEL);

    if (is_channel_delete_allowed == TRUE)
    {
        channel_ids_to_delete = (uint64 *)memorymanager__allocate(g_server_settings.max_channel_count * sizeof(uint64), MEMALLOC_MARKED_CHANNEL_INDICES);
        if (channel_ids_to_delete == NULL_POINTER)
        {
            goto label_client_msg__process_delete_channel_request_end;
        }
        channel_ids_to_delete[channels_to_delete_count] = json_channel_id->valueint;
        channels_to_delete_count += 1;

        // json_channel_id->valueint channel id to start marking other child channels from
        // channels_to_delete_count count of marked channels to delete
        // array that stores ids of channels that should be deleted
        channel__mark_channels_for_deletion(json_channel_id->valueint, &channels_to_delete_count, channel_ids_to_delete);
        for (marked_channel_index = 0; marked_channel_index < channels_to_delete_count; marked_channel_index++)
        {
            if (channel_groups__is_allowed(&g_clients_array[sender_client_id], &g_channel_array[channel_ids_to_delete[marked_channel_index]], PERMISSION_DELETE_CHANNEL) == FALSE)
            {
                server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], NULL_POINTER);
                goto label_client_msg__process_delete_channel_request_end;
            }
        }

        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__channel__process_delete_request channel ids to delete count ", channels_to_delete_count, "\n");

        owners_to_update = clib__vector_reserve(NULL, sizeof(*owners_to_update), g_server_settings.max_client_count);
        if (owners_to_update == NULL_POINTER)
        {
            goto label_client_msg__process_delete_channel_request_end;
        }

        // at this point the channels are marked, loop through marked channel ids, but do not delete them yet, just find out if there are clients there
        for (marked_channel_index = 0; marked_channel_index < channels_to_delete_count; marked_channel_index++)
        {
            channel_id_to_delete = channel_ids_to_delete[marked_channel_index];
            client_count_in_channel = channel__get_client_count_for_channel(channel_id_to_delete);

            DBG_CLIENT_MESSAGE log_info("%s %llu %s %llu %s", "client_msg__channel__process_delete_request client count in channel ", channel_id_to_delete, " is ", client_count_in_channel, "\n");

            for (client_id = 0; client_id < g_server_settings.max_client_count; client_id++)
            {
                client_to_move_maybe = &g_clients_array[client_id];

                if (client_to_move_maybe->is_existing == FALSE)
                {
                    continue;
                }
                if (client_to_move_maybe->is_authenticated == FALSE)
                {
                    continue;
                }

                // Clear the owner's role even when an administrator deletes their channel.
                if (client_to_move_maybe->is_temp_admin_channel == TRUE && client_to_move_maybe->temp_channel_id == channel_id_to_delete)
                {
                    client_to_move_maybe->is_temp_admin_channel = FALSE;
                    client_to_move_maybe->temp_channel_id = 0;
                    cvector_push_back(owners_to_update, client_id);
                }
                if (client_to_move_maybe->channel_id != channel_id_to_delete)
                {
                    continue;
                }

                // client found
                DBG_CLIENT_MESSAGE log_info("%s %lld %s", "client_msg__channel__process_delete_request moving client ", client_to_move_maybe->client_id, "to root channel \n");

                video_channel__process_client_leaving_channel(client_to_move_maybe, client_to_move_maybe->channel_id);

                client_to_move_maybe->channel_id = ROOT_CHANNEL_ID;
                client_to_move_maybe->has_pending_maintainer_reset_vote = FALSE; // channel changed - a pending reset vote belongs to the old channel

                // keep the webrtc peer's channel in sync, otherwise the audio relay keeps skipping this
                // client on the channel-mismatch check after the move to root
                audio_channel__process_client_channel_join(client_to_move_maybe);
                video_channel__process_client_joined_channel(client_to_move_maybe);

                server_msg__channel__send_join_message_to_all_clients(client_to_move_maybe, &g_channel_array[ROOT_CHANNEL_ID], "move");

                channel__welcome_client(client_to_move_maybe);

                server_msg__audio__send_active_microphone_usage_for_current_channel_to_single_client(client_to_move_maybe, ROOT_CHANNEL_ID);
            }
        }

        // clients are moved, now delete channels
        for (marked_channel_index = 0; marked_channel_index < channels_to_delete_count; marked_channel_index++)
        {
            channel_id_to_delete = channel_ids_to_delete[marked_channel_index];

            channel_groups__clear(&g_channel_array[channel_id_to_delete]);
            clib__null_memory(&g_channel_array[channel_id_to_delete], sizeof(channel_t));
            server_msg__channel__send_delete_message_to_all_clients(channel_id_to_delete, sender_client_id);
        }

        // if there is no maintainer in root channel, find new maintainer now
        if (g_channel_array[ROOT_CHANNEL_ID].is_channel_maintainer_present == FALSE)
        {
            if (channel__find_new_maintainer_for_channel(&index_of_new_maintainer, ROOT_CHANNEL_ID, 0, FALSE) == TRUE)
            {
                channel__set_maintainer(&g_channel_array[ROOT_CHANNEL_ID], index_of_new_maintainer);
                server_msg__channel__send_maintainer_id_to_clients_in_same_channel(&g_channel_array[ROOT_CHANNEL_ID], g_channel_array[ROOT_CHANNEL_ID].maintainer_id);
            }
        }
    }
    else
    {
        if (g_clients_array[sender_client_id].is_authenticated == TRUE && g_clients_array[sender_client_id].is_existing == TRUE)
        {
            server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], NULL_POINTER);
        }
    }

label_client_msg__process_delete_channel_request_end:

    memorymanager__free((nuint)channel_ids_to_delete);

    clib__unlock(&g_channels_global_rwlock_guard);
    audio_channel__apply_permission_changes();
    video_channel__apply_permission_changes();
    for (owner_index = 0; owner_index < cvector_size(owners_to_update); owner_index++)
    {
        server_msg__administration__send_permission_update_to_single_client(&g_clients_array[owners_to_update[owner_index]]);
    }
    cvector_free(owners_to_update);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes one reset_channel_maintainer vote, the last-resort recovery path for a channel
 *        whose announced maintainer never delivered usable channel keys
 *
 *        that happens with a faulty or modified client, or a connection that half-works. every
 *        keyless client keeps re-sending this vote every few seconds, and once MORE THAN HALF of
 *        the channel's clients have voted against the CURRENT maintainer generation, the server
 *        deposes the maintainer, picks a new one (excluding the deposed client), bumps the
 *        generation and broadcasts the new maintainer id - upon which the new maintainer's client
 *        distributes fresh channel keys exactly like after any other maintainer change.
 *
 *        the request carries NO payload - the client only says "i want the current maintainer of my
 *        channel replaced". all bookkeeping is server-internal: the vote is stamped with the
 *        channel's CURRENT maintainer_generation when it is recorded, so a vote recorded against a
 *        previous maintainer can never count against the newly appointed one, and votes expire by
 *        themselves when the voter switches channel or the generation moves on - no vote cleanup
 *        anywhere else. music bots are skipped on both sides of the count, they never hold channel
 *        keys and never vote, so counting them would only inflate the quorum denominator.
 *
 * @param cJSON* json_root -> the parsed client request (unused beyond the already-checked type)
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__channel__process_reset_maintainer_request(cJSON *json_root, uint64 sender_client_id)
{
    boole status0 = FALSE;
    uint64 channel_id = 0;
    channel_t *channel = NULL_POINTER;
    client_t *sender = NULL_POINTER;
    client_t *client_in_loop = NULL_POINTER;
    uint64 channel_member_count = 0;
    uint64 vote_count = 0;
    uint64 deposed_maintainer_id = 0;
    uint64 new_maintainer_index = 0;
    boole is_maintainer_found = FALSE;
    uint64 i = 0;

    // same per-client cooldown gate as every other request. the client re-votes every few seconds
    // for as long as it stays keyless, so a vote swallowed by the cooldown is simply retried
    if (base__is_request_allowed_based_on_spam_protection(sender_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_reset_maintainer_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    clib__write_lock(&g_channels_global_rwlock_guard);

    sender = &g_clients_array[sender_client_id];

    if (sender->is_existing == FALSE || sender->is_authenticated == FALSE || sender->is_idle == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_reset_maintainer_request is_existing == FALSE || is_authenticated == FALSE || is_idle == TRUE \n");
        goto label_reset_channel_maintainer_end;
    }

    // the vote always applies to the channel the sender is CURRENTLY in on the server's books;
    // the request carries no channel id, so a modified client cannot vote into someone else's channel
    channel_id = sender->channel_id;
    if (channel_id >= (uint64)g_server_settings.max_channel_count)
    {
        goto label_reset_channel_maintainer_end;
    }

    channel = &g_channel_array[channel_id];

    if (channel->is_existing == FALSE)
    {
        goto label_reset_channel_maintainer_end;
    }

    // nobody to depose
    if (channel->is_channel_maintainer_present == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_reset_maintainer_request channel has no maintainer \n");
        goto label_reset_channel_maintainer_end;
    }

    // the maintainer cannot vote itself out
    if (channel->maintainer_id == sender_client_id)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_reset_maintainer_request sender is the maintainer \n");
        goto label_reset_channel_maintainer_end;
    }

    // record the vote, stamped with the CURRENT generation - re-voting just overwrites the same
    // values. if the maintainer changes before quorum, the stamp stops matching and the vote no longer counts
    sender->has_pending_maintainer_reset_vote = TRUE;
    sender->maintainer_reset_vote_channel_id = channel_id;
    sender->maintainer_reset_vote_generation = channel->maintainer_generation;

    // count channel members and valid votes in one pass. music bots are skipped on both sides:
    // they never hold channel keys (bot audio is not channel-key encrypted) and never vote,
    // so counting them would only inflate the quorum denominator
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client_in_loop = &g_clients_array[i];

        if (client_in_loop->is_existing == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_music_bot == TRUE)
        {
            continue;
        }

        if (client_in_loop->channel_id != channel_id)
        {
            continue;
        }

        channel_member_count++;

        status0 = client_in_loop->has_pending_maintainer_reset_vote == TRUE && client_in_loop->maintainer_reset_vote_channel_id == channel_id && client_in_loop->maintainer_reset_vote_generation == channel->maintainer_generation;
        if (status0 == TRUE)
        {
            vote_count++;
        }
    }

    DBG_CLIENT_MESSAGE log_info("%s %llu %s %llu %s", "client_msg__channel__process_reset_maintainer_request votes ", vote_count, " of ", channel_member_count, "\n");

    // quorum rule: MORE THAN HALF of all clients present in the channel
    if ((vote_count * 2) <= channel_member_count)
    {
        goto label_reset_channel_maintainer_end;
    }

    deposed_maintainer_id = channel->maintainer_id;

    // An admin can only be voted out when another admin can take over.
    is_maintainer_found = channel__find_voted_maintainer_replacement(&new_maintainer_index, channel);
    if (is_maintainer_found == FALSE)
    {
        // Keep the current maintainer and generation when no permitted replacement is present.
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_reset_maintainer_request no replacement maintainer found \n");
        goto label_reset_channel_maintainer_end;
    }

    // consume the votes that carried this reset so they cannot count a second time
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client_in_loop = &g_clients_array[i];

        if (client_in_loop->has_pending_maintainer_reset_vote == TRUE && client_in_loop->maintainer_reset_vote_channel_id == channel_id)
        {
            client_in_loop->has_pending_maintainer_reset_vote = FALSE;
        }
    }

    channel__set_maintainer(channel, new_maintainer_index);

    // always-on log: a majority of a channel just declared its maintainer broken - the operator should see that
    log_info("%s %llu %s %llu %s %llu %s", "channel ", channel_id, ": maintainer reset by vote, deposed client ", deposed_maintainer_id, ", new maintainer client ", new_maintainer_index, "\n");

    // same announcement as any other maintainer change; the new maintainer's client reacts to it
    // by generating and distributing fresh channel keys to everyone in the channel
    server_msg__channel__send_maintainer_id_to_clients_in_same_channel(channel, channel->maintainer_id);

label_reset_channel_maintainer_end:
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief validates a go-to-idle-mode request: there are no fields to check, so it always succeeds
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole -> always TRUE
 */
static boole _client_msg_channel_internal__is_go_to_idle_mode_request_valid(cJSON *json_root)
{
    return TRUE;
}

/**
 * @brief This function processes go to idle more request, its modified version of join channel
 *        request
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__channel__process_go_to_idle_mode_request(cJSON *json_root, uint64 sender_client_id)
{
    channel_t *old_channel = NULL_POINTER;
    client_t *client = NULL_POINTER;
    boole is_maintainer_found = FALSE;
    boole is_authenticated = FALSE;
    boole is_existing = FALSE;
    boole is_idle = FALSE;

    // this is modified version of join channel request
    // no password verification, no checking if idle channel has maintainer (it's not a channel really)
    if (base__is_request_allowed_based_on_spam_protection(sender_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_go_to_idle_mode_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    if (_client_msg_channel_internal__is_go_to_idle_mode_request_valid(json_root) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_go_to_idle_mode_request is not valid");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    clib__write_lock(&g_channels_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    is_authenticated = client->is_authenticated;
    is_existing = client->is_existing;
    is_idle = client->is_idle;

    if (is_existing == FALSE || is_authenticated == FALSE || is_idle == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_go_to_idle_mode_request is_existing == FALSE || is_authenticated == FALSE || is_idle == TRUE \n");
        goto label_go_to_idle_mode_request_end;
    }

    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_go_to_idle_mode_request got here \n");

    old_channel = &g_channel_array[client->channel_id];

    // idle leaves the channel for video purposes: a streamer's stream ends, a viewer drops off
    video_channel__process_client_leaving_channel(client, client->channel_id);

    // change channel id and idle state at this
    client->idle_return_channel_id = client->channel_id;
    clib__copy_memory(old_channel->password, client->idle_return_channel_password, sizeof(client->idle_return_channel_password), sizeof(client->idle_return_channel_password));
    client->is_idle = TRUE;
    client->channel_id = -2; // -2 marks the client as being in idle mode rather than in a real channel
    channel_groups__refresh(client);
    client->has_pending_maintainer_reset_vote = FALSE; // channel changed - a pending reset vote belongs to the old channel

    is_maintainer_found = channel__replace_departed_maintainer(old_channel, sender_client_id);
    server_msg__channel__send_client_going_to_idle_mode_info_to_all_clients(client);
    if (is_maintainer_found == TRUE)
    {
        server_msg__channel__send_maintainer_id_to_clients_in_same_channel(old_channel, old_channel->maintainer_id);
    }

    // the client closes its end of the datachannel right after sending this request; drop the server
    // side too - a slot left "connected" on a transport that no longer worked made the re-create request on resume be
    // refused until the ICE consent timeout noticed, tens of seconds without working audio
    audio_channel__process_client_disconnect(client);
    video_websocket__disconnect(client);

    // the transport is gone, say so; the fresh peer built on resume re-announces the real state
    client->audio_state = AUDIO_STATE__AUDIO_COMPLETELY_DISABLED;
    server_msg__audio__send_state_of_client_to_all_clients(client, AUDIO_STATE__AUDIO_COMPLETELY_DISABLED);

label_go_to_idle_mode_request_end:
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Validates a remembered channel ID; a removed destination is resolved to root under the
 *        locks.
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole -> TRUE when the request is valid, FALSE otherwise
 */
static boole _client_msg_channel_internal__is_process_come_back_from_idle_mode_request_valid(cJSON *json_root)
{
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_channel_id = NULL_POINTER;
    uint64 channel_id = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
    if (util__json_read_uint64(json_channel_id, MAX_CHANNEL_COUNT - 1, &channel_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_channel_internal__is_process_come_back_from_idle_mode_request_valid cJSON_IsNumber(json_channel_id)");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief processes a client's request to come back from idle mode into a channel
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__channel__process_come_back_from_idle_mode_request(cJSON *json_root, uint64 sender_client_id)
{
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_channel_id = NULL_POINTER;
    cJSON *json_channel_password = NULL_POINTER;
    boole password_already_proven = FALSE;
    client_t *client = NULL_POINTER;
    channel_t *channel_to_join = NULL_POINTER;

    if (_client_msg_channel_internal__is_process_come_back_from_idle_mode_request_valid(json_root) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_come_back_from_idle_mode_request is not valid");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");

    clib__write_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    clib__write_lock(&g_channels_global_rwlock_guard);

    // check if the client that sent the message is valid: connected and existing.
    // this was checked before but not within write lock like here
    client = &g_clients_array[sender_client_id];

    if (client->is_authenticated == FALSE || client->is_existing == FALSE || client->is_idle == FALSE)
    {
        goto label_client_msg__process_come_back_from_idle_mode_request_end;
    }

    // A channel may have been deleted and the allocation shrunk while the
    // client was idle or this request waited for the lock. Resume in the lobby.
    channel_to_join = _client_msg_channel_internal__is_channel_id_valid(json_channel_id) ? &g_channel_array[json_channel_id->valueint] : &g_channel_array[ROOT_CHANNEL_ID];

    json_channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");
    password_already_proven = (boole)(client->idle_return_channel_id == channel_to_join->channel_id && clib__is_string_equal(client->idle_return_channel_password, channel_to_join->password));

    if (channel_to_join->is_existing == FALSE || channel__check_channel_access(client, channel_to_join, cJSON_IsString(json_channel_password) ? json_channel_password->valuestring : NULL_POINTER, password_already_proven) == FALSE)
    {
        /* Older clients send no password on resume. Return to the lobby on
         * refusal so they can use the normal channel/password UI. */
        channel_to_join = &g_channel_array[ROOT_CHANNEL_ID];
    }

    client->idle_return_channel_id = ROOT_CHANNEL_ID;
    clib__null_memory(client->idle_return_channel_password, sizeof(client->idle_return_channel_password));
    client->is_idle = FALSE;
    client->channel_id = channel_to_join->channel_id;
    client->has_pending_maintainer_reset_vote = FALSE; // channel changed - a pending reset vote belongs to the old channel

    // keep the webrtc peer's channel in sync, otherwise the audio relay keeps skipping this client on the
    // channel-mismatch check after it returns from idle
    audio_channel__process_client_channel_join(client);
    video_channel__process_client_joined_channel(client);

    server_msg__channel__send_client_coming_back_from_idle_mode_info_to_all_clients(&g_clients_array[sender_client_id], channel_to_join->channel_id);

    channel__send_hidden_members(client, channel_to_join);
    channel__welcome_client(client);

    server_msg__audio__send_active_microphone_usage_for_current_channel_to_single_client(client, client->channel_id);

label_client_msg__process_come_back_from_idle_mode_request_end:
    clib__unlock(&g_channels_global_rwlock_guard);
    audio_channel__apply_permission_changes();
    video_channel__apply_permission_changes();
    clib__unlock(&g_clients_global_rwlock_guard);
}
