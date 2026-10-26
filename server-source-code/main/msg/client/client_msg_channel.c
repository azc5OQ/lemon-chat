// This file checks requests to create, edit, join or leave channels and to choose a new channel maintainer.
#include "client_msg.h"
#include "../../core/permissions.h"
#include "../../core/channel.h"
#include "../server/server_msg.h"
#include "../../core/audio_channel.h"
#include "../../video_websocket.h"
#include "../../core/video_channel.h"
#include "../../base.h"
#include "../../util.h"
#include "../../memory_manager.h"
#include "../../clib/clib_string.h"
#include "../../clib/clib_memory.h"
#include "../../../third-party/rxi-log/log.h"
#include "../../../third-party/eteran-cvector/cvector.h"

// declarations
static boole _client_msg_channel_internal__is_json_create_channel_request_valid(cJSON* json_root, uint64 client_id);
static boole _client_msg_channel_internal__is_json_edit_channel_request_valid(cJSON* json_root, uint64 client_id);
static boole _client_msg_channel_internal__is_json_join_channel_request_format_valid(cJSON* json_root, uint64 client_id);
static boole _client_msg_channel_internal__is_json_delete_request_format_valid(cJSON* json_root, uint64 client_id);
static boole _client_msg_channel_internal__is_go_to_idle_mode_request_valid(cJSON* json_root);
static boole _client_msg_channel_internal__is_process_come_back_from_idle_mode_request_valid(cJSON* json_root);

/**
 * @brief processes an admin request to set (or clear) a channel's icon. an icon_id in the request assigns
 *        that icon (which must exist); a request without icon_id clears the channel's icon. broadcasts the
 *        change so every client updates its channel row live. only an authenticated admin may do this.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__channel__process_set_icon(cJSON* json_root, uint64 sender_client_id)
{
    client_t* sender_client = 0;
    cJSON* json_message_object = 0;
    cJSON* json_channel_id = 0;
    cJSON* json_icon_id = 0;
    uint64 channel_id = 0;
    uint64 icon_id = 0;
    boole wants_icon = FALSE;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
    json_icon_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "icon_id");

    if (cJSON_IsNumber(json_channel_id) == FALSE)
    {
        return;
    }

    if (json_channel_id->valueint < 0 || json_channel_id->valueint >= g_server_settings.max_channel_count)
    {
        return;
    }

    channel_id = (uint64)json_channel_id->valueint;
    wants_icon = cJSON_IsNumber(json_icon_id);
    if (wants_icon == TRUE)
    {
        icon_id = (uint64)json_icon_id->valueint;
    }

    // lock order: clients -> channels -> icons
    clib__write_lock(&g_clients_global_rwlock_guard);
    if (g_server_settings.is_permission_system_enabled == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], "Server groups and icons are disabled.");
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    clib__write_lock(&g_channels_global_rwlock_guard);
    clib__read_lock(&g_icons_global_rwlock_guard);

    sender_client = &g_clients_array[sender_client_id];

    if (sender_client->is_authenticated == FALSE || sender_client->is_existing == FALSE || sender_client->is_admin == FALSE)
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
static boole _client_msg_channel_internal__is_json_create_channel_request_valid(cJSON* json_root, uint64 client_id)
{
    cJSON* json_parent_channel_id = 0;
    cJSON* json_channel_name = 0;
    cJSON* json_channel_description = 0;
    cJSON* json_channel_password = 0;
    cJSON* json_is_audio_enabled = 0;
    cJSON* json_message_object = 0;

    int64 status = 0;

    // existence of "message" already checked before
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (channel__read_required_powers(json_message_object, NULL_POINTER) == FALSE)
    {
        return FALSE;
    }

    // json_message_object exists, continue validating received json
    json_parent_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "parent_channel_id");
    if (cJSON_IsNumber(json_parent_channel_id) == FALSE)
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
void client_msg__channel__process_create_request(cJSON* json_root, uint64 sender_client_id)
{
    boole is_channel_create_allowed = FALSE;
    boole is_parent_channel_id_existing = FALSE;
    boole creating_temp_channel = FALSE;
    boole parent_is_temp_channel = FALSE;
    cJSON* json_parent_channel_id = 0;
    cJSON* json_channel_description = 0;
    cJSON* json_channel_password = 0;
    cJSON* json_channel_name = 0;
    cJSON* json_is_audio_enabled = 0;
    cJSON* json_is_video_stream_enabled = 0;
    cJSON* json_is_client_limit_active = 0;
    cJSON* json_max_client_count = 0;
    boole is_password_used = FALSE;
    boole is_channel_created_successfully = FALSE;
    cJSON* json_message_object = 0;
    uint64 i = 0;
    uint64 created_channel_index = 0;
    uint64 channel_creator_client_id = 0;
    channel_t* channel = 0;
    boole status = FALSE;

    // check timestamp first
    // check if json is valid
    // check access rights
    // check if parent channel id is valid, if parent channel is is_existing
    // finally, create channel
    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_create_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    status = _client_msg_channel_internal__is_json_create_channel_request_valid(json_root, sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_create_request client_msg__is_json_create_channel_request_valid == FALSE \n");
        return;
    }

    // Keep the clients lock until creation finishes so group changes cannot alter this decision.
    is_channel_create_allowed = FALSE;
    clib__write_lock(&g_clients_global_rwlock_guard);
    if (g_clients_array[sender_client_id].is_authenticated == TRUE && g_clients_array[sender_client_id].is_existing == TRUE)
    {
        if (permissions__is_allowed(&g_clients_array[sender_client_id], PERMISSION_CREATE_CHANNEL) == TRUE)
        {
            is_channel_create_allowed = TRUE;
        }
        else if ((g_server_settings.is_permission_system_enabled == TRUE || g_server_settings.is_temp_channel_creation_allowed == TRUE)
            && g_clients_array[sender_client_id].is_temp_admin_channel == FALSE
            && permissions__is_allowed(&g_clients_array[sender_client_id], PERMISSION_CREATE_TEMP_CHANNEL) == TRUE)
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
        json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
        json_parent_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "parent_channel_id");
        json_channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");
        json_channel_description = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_description");
        json_channel_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_name");
        json_is_audio_enabled = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_audio_enabled");
        json_is_client_limit_active = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_client_limit_active");
        json_max_client_count = cJSON_GetObjectItemCaseSensitive(json_message_object, "max_client_count");

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
                    clib__copy_memory(json_channel_name->valuestring, channel->name, clib__utf8_string_length(json_channel_name->valuestring), CHANNEL_NAME_MAX_LENGTH);
                    clib__null_memory(channel->password, CHANNEL_PASSWORD_MAX_LENGTH);
                    if (clib__utf8_string_length(json_channel_password->valuestring) > 0)
                    {
                        util__hash_password_to_base64(json_channel_password->valuestring, channel->password, CHANNEL_PASSWORD_MAX_LENGTH);
                    }
                    clib__copy_memory(json_channel_description->valuestring, channel->description, clib__utf8_string_length(json_channel_description->valuestring), CHANNEL_DESCRIPTION_MAX_LENGTH);
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
                    channel->is_client_limit_active = (boole)cJSON_IsTrue(json_is_client_limit_active);
                    channel->max_client_count = 0;
                    // clamp the client-supplied capacity before the double->uint64 cast, which is undefined
                    // for negative / NaN / huge values. NaN fails both comparisons, so the range check
                    // rejects it too. a channel can never hold more clients than the server allows
                    if (cJSON_IsNumber(json_max_client_count) && json_max_client_count->valuedouble >= 0 && json_max_client_count->valuedouble <= (double)g_server_settings.max_client_count)
                    {
                        channel->max_client_count = (uint64)json_max_client_count->valuedouble;
                    }

                    DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__channel__process_create_request is_parent_channel_id_existing ", i, "\n");
                    is_channel_created_successfully = TRUE;
                    created_channel_index = i;
                    channel_creator_client_id = sender_client_id;

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
                channel__move_client_into_channel(channel_creator_client_id, created_channel_index);

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
static boole _client_msg_channel_internal__is_json_edit_channel_request_valid(cJSON* json_root, uint64 client_id)
{
    cJSON* json_channel_id = 0;
    cJSON* json_channel_name = 0;
    cJSON* json_channel_description = 0;
    cJSON* json_channel_password = 0;
    cJSON* json_message_object = 0;
    cJSON* json_is_audio_enabled = 0;

    int64 status = 0;

    // existence of "message" already checked before
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (channel__read_required_powers(json_message_object, NULL_POINTER) == FALSE)
    {
        return FALSE;
    }

    // json_message_object exists, continue validating received json
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
    if (cJSON_IsNumber(json_channel_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(channel_id) \n");
        return FALSE;
    }

    // the root channel IS editable (renaming it, description, audio): only deleting it is
    // forbidden, and that goes through a different request
    if (json_channel_id->valueint < 0 || (json_channel_id->valueint >= (g_server_settings.max_channel_count - 1)))
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
void client_msg__channel__process_edit_request(cJSON* json_root, uint64 sender_client_id)
{
    boole is_channel_edit_allowed = FALSE;
    cJSON* json_channel_id = 0;
    cJSON* json_channel_description = 0;
    cJSON* json_channel_password = 0;
    cJSON* json_channel_name = 0;
    boole is_password_used = FALSE;
    boole is_channel_edited_successfully = FALSE;
    cJSON* json_message_object = 0;
    cJSON* json_is_audio_enabled = 0;
    cJSON* json_is_video_stream_enabled = 0;
    boole should_stop_video_stream = FALSE;
    cJSON* json_is_client_limit_active = 0;
    cJSON* json_max_client_count = 0;
    uint64 channel_index_to_edit = 0;
    channel_t* channel = 0;
    boole status = FALSE;

    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_edit_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    status = _client_msg_channel_internal__is_json_edit_channel_request_valid(json_root, sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_edit_request client_msg__is_json_edit_channel_request_valid == FALSE \n");
        return;
    }

    // editing a channel is admin-only, except a temp admin may edit (rename / set password / toggle audio)
    // their own temp channel without admin rights
    is_channel_edit_allowed = FALSE;
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
    clib__write_lock(&g_clients_global_rwlock_guard);
    if (g_clients_array[sender_client_id].is_authenticated == TRUE && g_clients_array[sender_client_id].is_existing == TRUE)
    {
        if (g_clients_array[sender_client_id].is_admin == TRUE)
        {
            is_channel_edit_allowed = TRUE;
        }
        else if (g_clients_array[sender_client_id].is_temp_admin_channel == TRUE && g_clients_array[sender_client_id].temp_channel_id == (uint64)json_channel_id->valueint)
        {
            is_channel_edit_allowed = TRUE;
        }
    }

    if (is_channel_edit_allowed == TRUE)
    {
        clib__write_lock(&g_channels_global_rwlock_guard);
        json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
        json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
        json_channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");
        json_channel_description = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_description");
        json_channel_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_name");
        json_is_audio_enabled = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_audio_enabled");
        json_is_client_limit_active = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_client_limit_active");
        json_max_client_count = cJSON_GetObjectItemCaseSensitive(json_message_object, "max_client_count");

        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_edit_request got here \n");

        channel_index_to_edit = json_channel_id->valueint;

        channel = &g_channel_array[channel_index_to_edit];

        if (channel->is_existing == TRUE)
        {
            clib__null_memory(channel->name, CHANNEL_NAME_MAX_LENGTH);
            clib__null_memory(channel->description, CHANNEL_DESCRIPTION_MAX_LENGTH);

            clib__copy_memory(json_channel_name->valuestring, channel->name, clib__utf8_string_length(json_channel_name->valuestring), CHANNEL_NAME_MAX_LENGTH);

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
            clib__copy_memory(json_channel_description->valuestring, channel->description, clib__utf8_string_length(json_channel_description->valuestring), CHANNEL_DESCRIPTION_MAX_LENGTH);

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
                channel->is_client_limit_active = (boole)cJSON_IsTrue(json_is_client_limit_active);
                channel->max_client_count = 0;
                // clamp the client-supplied capacity before the double->uint64 cast, which is undefined
                // for negative / NaN / huge values. NaN fails both comparisons, so the range check
                // rejects it too. a channel can never hold more clients than the server allows
                if (cJSON_IsNumber(json_max_client_count) && json_max_client_count->valuedouble >= 0 && json_max_client_count->valuedouble <= (double)g_server_settings.max_client_count)
                {
                    channel->max_client_count = (uint64)json_max_client_count->valuedouble;
                }
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

            clib__unlock(&g_channels_global_rwlock_guard);
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
 * @return boole
 */
static boole _client_msg_channel_internal__is_json_join_channel_request_format_valid(cJSON* json_root, uint64 client_id)
{
    cJSON* json_channel_id = 0;
    cJSON* json_channel_password = 0;
    cJSON* json_message_object = 0;

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
    if (cJSON_IsNumber(json_channel_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(json_channel_id) \n");
        return FALSE;
    }

    if (json_channel_id->valueint < 0 || (json_channel_id->valueint >= (g_server_settings.max_channel_count - 1)))
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_channel_id->valueint < 0 || (json_channel_id->valueint >= (g_server_settings.max_channel_count - 1)) \n");
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
void client_msg__channel__process_join_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    cJSON* json_channel_password = 0;
    cJSON* json_channel_id = 0;
    cJSON* json_message_object = 0;
    channel_t* new_channel = 0;
    channel_t* old_channel = 0;
    client_t* client_that_is_joining_channel = 0;
    boole is_maintainer_found = FALSE;
    boole is_authenticated = FALSE;
    boole is_existing = FALSE;
    boole channel_join_announced = FALSE;
    boole ownership_changed = FALSE;

    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_join_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    status = _client_msg_channel_internal__is_json_join_channel_request_format_valid(json_root, sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_join_request client_msg__is_json_join_channel_request_format_valid == FALSE \n");
        return;
    }

    // Keep membership changes and their announcements in the same order for every client.
    clib__write_lock(&g_clients_global_rwlock_guard);
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

    new_channel = &g_channel_array[json_channel_id->valueint];

    if (new_channel->is_existing == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_join_request channel not is_existing \n");
        goto client_msg__process_join_channel_request_end;
    }

    client_that_is_joining_channel = &g_clients_array[sender_client_id];
    old_channel = &g_channel_array[client_that_is_joining_channel->channel_id];

    status = (boole)(client_that_is_joining_channel->channel_id == json_channel_id->valueint);

    if (status == TRUE)
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
        server_msg__channel__send_join_message_to_all_clients(client_that_is_joining_channel, new_channel);
        channel_join_announced = TRUE;
        channel__destroy_temp_channel(old_channel->channel_id);
    }

    is_maintainer_found = channel__replace_departed_maintainer(old_channel, sender_client_id);
    if (channel_join_announced == FALSE)
    {
        server_msg__channel__send_join_message_to_all_clients(client_that_is_joining_channel, new_channel);
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
static boole _client_msg_channel_internal__is_json_delete_request_format_valid(cJSON* json_root, uint64 client_id)
{
    cJSON* json_channel_id = 0;
    cJSON* json_message_object = 0;

    // existence of "message" has already been checked at this point
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    // json_message_object exists, continue validating received json
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
    if (cJSON_IsNumber(json_channel_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(json_channel_id) \n");
        return FALSE;
    }

    if (json_channel_id->valueint < 0 || (json_channel_id->valueint >= (g_server_settings.max_channel_count - 1)))
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_channel_id->valueint < 0 || (json_channel_id->valueint >= (g_server_settings.max_channel_count - 1)) \n");
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
void client_msg__channel__process_delete_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole is_authenticated = FALSE;
    boole is_existing = FALSE;
    cJSON* json_channel_id = 0;
    cJSON* json_message_object = 0;
    uint64* channel_ids_to_delete = 0;
    uint64 channels_to_delete_count = 0;
    client_t* client_to_move_maybe = 0;
    uint64 channel_id_to_delete = 0;
    uint64 client_count_in_channel = 0;
    uint64 index_of_new_maintainer = 0;
    boole is_channel_delete_allowed = FALSE;
    uint64 marked_channel_index = 0;
    uint64 client_id = 0;
    uint64* owners_to_update = NULL_POINTER;
    uint64 owner_index = 0;

    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_delete_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    status = _client_msg_channel_internal__is_json_delete_request_format_valid(json_root, sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_delete_request _client_msg_channel_internal__is_json_delete_request_format_valid == FALSE \n");
        return;
    }

    // add admin rights check later
    clib__write_lock(&g_clients_global_rwlock_guard);
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

    status = g_channel_array[json_channel_id->valueint].is_existing;

    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_delete_request channel does not exist \n");
        goto label_client_msg__process_delete_channel_request_end;
    }

    // deleting a channel is admin-only, except a temp admin may delete their own temp channel
    is_channel_delete_allowed = FALSE;
    if (g_clients_array[sender_client_id].is_admin == TRUE)
    {
        is_channel_delete_allowed = TRUE;
    }
    else if (g_clients_array[sender_client_id].is_temp_admin_channel == TRUE && g_clients_array[sender_client_id].temp_channel_id == (uint64)json_channel_id->valueint && g_channel_array[json_channel_id->valueint].is_temp_channel == TRUE)
    {
        is_channel_delete_allowed = TRUE;
    }

    if (is_channel_delete_allowed == TRUE)
    {
        channel_ids_to_delete = (uint64*)memorymanager__allocate(g_server_settings.max_channel_count * sizeof(uint64), MEMALLOC_MARKED_CHANNEL_INDICES);
        channel_ids_to_delete[channels_to_delete_count] = json_channel_id->valueint;
        channels_to_delete_count += 1;

        // json_channel_id->valueint channel id to start marking other child channels from
        // channels_to_delete_count count of marked channels to delete
        // array that stores ids of channels that should be deleted
        channel__mark_channels_for_deletion(json_channel_id->valueint, &channels_to_delete_count, channel_ids_to_delete);

        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__channel__process_delete_request channel ids to delete count ", channels_to_delete_count, "\n");

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

                server_msg__channel__send_join_message_to_all_clients(client_to_move_maybe, &g_channel_array[ROOT_CHANNEL_ID]);

                if (g_channel_array[ROOT_CHANNEL_ID].is_channel_maintainer_present == TRUE)
                {
                    DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__channel__process_delete_request also sending maintainer id of root channel to moving client ", g_channel_array[ROOT_CHANNEL_ID].maintainer_id, "\n");
                    server_msg__channel__send_maintainer_id_to_single_client(client_to_move_maybe, &g_channel_array[ROOT_CHANNEL_ID], g_channel_array[ROOT_CHANNEL_ID].maintainer_id);
                }

                server_msg__audio__send_active_microphone_usage_for_current_channel_to_single_client(client_to_move_maybe, ROOT_CHANNEL_ID);
            }
        }

        // clients are moved, now delete channels
        for (marked_channel_index = 0; marked_channel_index < channels_to_delete_count; marked_channel_index++)
        {
            channel_id_to_delete = channel_ids_to_delete[marked_channel_index];

            clib__null_memory(&g_channel_array[channel_id_to_delete], sizeof(channel_t));
            server_msg__channel__send_delete_message_to_all_clients(channel_id_to_delete, sender_client_id);
        }
        // if there is no maintainer in root channel, find new maintainer now
        if (g_channel_array[ROOT_CHANNEL_ID].is_channel_maintainer_present == FALSE)
        {
            status = channel__find_new_maintainer_for_channel(&index_of_new_maintainer, ROOT_CHANNEL_ID, 0, FALSE);
            if (status == TRUE)
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
void client_msg__channel__process_reset_maintainer_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    uint64 channel_id = 0;
    channel_t* channel = 0;
    client_t* sender = 0;
    client_t* client_in_loop = 0;
    uint64 channel_member_count = 0;
    uint64 vote_count = 0;
    uint64 deposed_maintainer_id = 0;
    uint64 new_maintainer_index = 0;
    boole is_maintainer_found = FALSE;
    uint64 i = 0;

    // same per-client cooldown gate as every other request. the client re-votes every few seconds
    // for as long as it stays keyless, so a vote swallowed by the cooldown is simply retried
    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_reset_maintainer_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);
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

        if (client_in_loop->has_pending_maintainer_reset_vote == TRUE && client_in_loop->maintainer_reset_vote_channel_id == channel_id && client_in_loop->maintainer_reset_vote_generation == channel->maintainer_generation)
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

    // pick a replacement, never handing the role right back to the client that was just voted out
    is_maintainer_found = channel__find_new_maintainer_for_channel(&new_maintainer_index, channel_id, deposed_maintainer_id, TRUE);
    if (is_maintainer_found == FALSE)
    {
        // cannot really happen while at least one voter is present in the channel; leave state untouched
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
static boole _client_msg_channel_internal__is_go_to_idle_mode_request_valid(cJSON* json_root)
{
    return TRUE;
}

/**
 * @brief This function processes go to idle more request, its modified version of join channel request
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__channel__process_go_to_idle_mode_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    channel_t* old_channel = 0;
    client_t* client = 0;
    boole is_maintainer_found = FALSE;
    boole is_authenticated = FALSE;
    boole is_existing = FALSE;
    boole is_idle = FALSE;

    // this is modified version of join channel request
    // no password verification, no checking if idle channel has maintainer (it's not a channel really)
    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_go_to_idle_mode_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    status = _client_msg_channel_internal__is_go_to_idle_mode_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_go_to_idle_mode_request is not valid");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);
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
 * @brief validates a come-back-from-idle-mode request: the target channel id is present and within range
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole -> TRUE when the request is valid, FALSE otherwise
 */
static boole _client_msg_channel_internal__is_process_come_back_from_idle_mode_request_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* json_channel_id = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
    if (cJSON_IsNumber(json_channel_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_channel_internal__is_process_come_back_from_idle_mode_request_valid cJSON_IsNumber(json_channel_id)");
        return FALSE;
    }

    if (json_channel_id->valueint < 0 || json_channel_id->valueint >= g_server_settings.max_channel_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_channel_id is invalid");
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
void client_msg__channel__process_come_back_from_idle_mode_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    cJSON* json_message_object = 0;
    cJSON* json_channel_id = 0;
    cJSON* json_channel_password = 0;
    boole password_already_proven = FALSE;
    client_t* client = 0;
    channel_t* channel_to_join = 0;

    status = _client_msg_channel_internal__is_process_come_back_from_idle_mode_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__channel__process_come_back_from_idle_mode_request is not valid");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_channels_global_rwlock_guard);

    // check if the client that sent the message is valid: connected and existing.
    // this was checked before but not within write lock like here
    client = &g_clients_array[sender_client_id];

    if (client->is_authenticated == FALSE || client->is_existing == FALSE || client->is_idle == FALSE)
    {
        goto label_client_msg__process_come_back_from_idle_mode_request_end;
    }

    channel_to_join = &g_channel_array[json_channel_id->valueint];

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
