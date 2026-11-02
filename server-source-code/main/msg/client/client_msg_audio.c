// This file checks requests to set up WebRTC and change the client's microphone state.
#include "client_msg.h"
#include "../server/server_msg.h"
#include "../../audio_webrtc.h"
#include "../../core/permissions.h"
#include "../../base.h"
#include "../../util.h"
#include "../../clib/clib.h"
#include "../../../third-party/rxi-log/log.h"

// declarations
static boole _client_msg_audio_internal__is_json_sdp_answer_format_valid(cJSON* json_root, uint64 client_id, cJSON** checked_value);
static boole _client_msg_audio_internal__is_ice_candidate_format_valid(cJSON* json_root, uint64 client_id, cJSON** checked_value);
static boole _client_msg_audio_internal__is_json_process_microphone_usage_valid(cJSON* json_root);

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @return boole
 */
static boole _client_msg_audio_internal__is_json_sdp_answer_format_valid(cJSON* json_root, uint64 client_id, cJSON** checked_value)
{
    cJSON* json_sdp_message_value = 0;
    cJSON* json_value_object = 0;
    cJSON* json_sdp_message_type = 0;
    cJSON* json_message_object = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_value_object = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    if (cJSON_IsObject(json_value_object) == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " cJSON_IsObject(json_value_object) \n");
        return FALSE;
    }

    json_sdp_message_type = cJSON_GetObjectItemCaseSensitive(json_value_object, "type");
    if (cJSON_IsString(json_sdp_message_type) == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_sdp_message_type) \n");
        return FALSE;
    }

    if (json_sdp_message_type->valuestring == NULL_POINTER)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " json_sdp_message_type->valuestring == NULL \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_sdp_message_type->valuestring) == 0)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " clib__utf8_string_length(json_sdp_message_type->valuestring) \n");
        return FALSE;
    }

    json_sdp_message_value = cJSON_GetObjectItemCaseSensitive(json_value_object, "sdp");
    if (cJSON_IsString(json_sdp_message_value) == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_sdp_message_value) \n");
        return FALSE;
    }

    if (json_sdp_message_value->valuestring == NULL_POINTER)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " json_sdp_message_value->valuestring == NULL \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_sdp_message_value->valuestring) == 0)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " clib__utf8_string_length(json_sdp_message_value->valuestring) \n");
        return FALSE;
    }

    *checked_value = json_value_object;
    return TRUE;
}

/**
 * @brief This function processes sdp answer for offer that server sent to client as part of process of establishing webrtc datachannel connection
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__audio__process_sdp_answer(cJSON* json_root, uint64 sender_client_id)
{
    boole status0 = FALSE;
    boole status = FALSE;
    cJSON* json_message_value = 0;

    DBG_AUDIO_WEBRTC log_info("%s", "client_msg__audio__process_sdp_answer");

    status = _client_msg_audio_internal__is_json_sdp_answer_format_valid(json_root, sender_client_id, &json_message_value);
    if (status == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s", "client_msg__audio__process_sdp_answer client_msg__is_json_sdp_answer_format_valid == FALSE \n");
        return;
    }

    // Keep the exact object validated above; differently cased keys cannot replace it.
    clib__read_lock(&g_clients_global_rwlock_guard);
    status0 = base__is_current_client_request(sender_client_id);
    if (status0 == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    if (g_clients_array[sender_client_id].is_authenticated == TRUE)
    {
        DBG_AUDIO_WEBRTC log_info("%s", "client_msg__audio__process_sdp_answer got here");

        audio_webrtc__set_remote_description(&g_clients_array[sender_client_id],
            cJSON_GetObjectItemCaseSensitive(json_message_value, "sdp")->valuestring,
            cJSON_GetObjectItemCaseSensitive(json_message_value, "type")->valuestring);
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
static boole _client_msg_audio_internal__is_ice_candidate_format_valid(cJSON* json_root, uint64 client_id, cJSON** checked_value)
{
    boole status0 = FALSE;
    boole status1 = FALSE;
    boole status2 = FALSE;
    cJSON* json_candidate = 0;
    cJSON* json_value_object = 0;
    cJSON* json_sdpMid = 0;
    cJSON* json_sdpMLineIndex = 0;
    cJSON* json_message_object = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_value_object = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    if (cJSON_IsObject(json_value_object) == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " cJSON_IsObject(json_value_object) \n");
        return FALSE;
    }

    json_candidate = cJSON_GetObjectItemCaseSensitive(json_value_object, "candidate");
    if (cJSON_IsString(json_candidate) == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_candidate) \n");
        return FALSE;
    }

    if (json_candidate->valuestring == NULL_POINTER)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " json_candidate->valuestring == NULL \n");
        return FALSE;
    }

    // if (clib__utf8_string_length(json_sdp_message_type->valuestring) == 0)
    // {
    // DBG_AUDIO_WEBRTC log_info("%s %llu %s","client : ", client_id, " clib__utf8_string_length(json_sdp_message_type->valuestring) \n");
    // return FALSE;
    // }
    json_sdpMid = cJSON_GetObjectItemCaseSensitive(json_value_object, "sdpMid");
    if (cJSON_IsString(json_sdpMid) == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_sdpMid) \n");
        return FALSE;
    }

    if (json_sdpMid->valuestring == NULL_POINTER)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " json_sdpMid->valuestring == NULL \n");
        return FALSE;
    }

    // if (clib__utf8_string_length(json_sdp_message_value->valuestring) == 0)
    // {
    // DBG_AUDIO_WEBRTC log_info("%s %llu %s","client : ", client_id, " clib__utf8_string_length(json_sdp_message_value->valuestring) \n");
    // return FALSE;
    // }
    json_sdpMLineIndex = cJSON_GetObjectItemCaseSensitive(json_value_object, "sdpMLineIndex");
    status0 = cJSON_IsNumber(json_sdpMLineIndex) == FALSE;
    if (status0 == FALSE)
    {
        status1 = json_sdpMLineIndex->valueint < 0;
        status2 = json_sdpMLineIndex->valuedouble != (double)json_sdpMLineIndex->valueint;
        status0 = status1 == TRUE || status2 == TRUE;
    }
    if (status0 == TRUE)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(json_sdpMLineIndex) \n");
        return FALSE;
    }

    *checked_value = json_value_object;
    return TRUE;
}

/**
 * @brief This function processes ice candidate. When I tried this on local network, this was not needed. Still, good to send and process
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__audio__process_ice_candidate(cJSON* json_root, uint64 sender_client_id)
{
    boole status0 = FALSE;
    boole status = FALSE;
    cJSON* json_message_value = 0;

    DBG_AUDIO_WEBRTC log_info("%s", "client_msg__audio__process_ice_candidate");

    status = _client_msg_audio_internal__is_ice_candidate_format_valid(json_root, sender_client_id, &json_message_value);
    if (status == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s", "client_msg__audio__process_ice_candidate client_msg__is_ice_candidate_format_valid == FALSE \n");
        return;
    }

    // Keep the exact object validated above; differently cased keys cannot replace it.
    clib__read_lock(&g_clients_global_rwlock_guard);
    status0 = base__is_current_client_request(sender_client_id);
    if (status0 == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    if (g_clients_array[sender_client_id].is_authenticated == TRUE)
    {
        DBG_AUDIO_WEBRTC log_info("%s", "client_msg__audio__process_ice_candidate got here");

        audio_webrtc__add_remote_candidate(&g_clients_array[sender_client_id],
            cJSON_GetObjectItemCaseSensitive(json_message_value, "candidate")->valuestring);
    }

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief validates a microphone-usage message: the usage value is present and within the 1..3 range
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole
 */
static boole _client_msg_audio_internal__is_json_process_microphone_usage_valid(cJSON* json_root)
{
    cJSON* json_value = 0;
    cJSON* json_message_object = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    if (cJSON_IsNumber(json_value) == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s", "_client_msg_audio_internal__is_json_process_microphone_usage_valid cJSON_IsNumber(json_value_object) \n");
        return FALSE;
    }

    if (json_value->valueint < 1 || json_value->valueint > 3
        || json_value->valuedouble != (double)json_value->valueint)
    {
        DBG_AUDIO_WEBRTC log_info("%s", "_client_msg_audio_internal__is_json_process_microphone_usage_valid json_value->valueint < 1 || json_value->valueint > 3 \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief processes a client's microphone-usage change and broadcasts the resulting audio state to the channel
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__audio__process_microphone_usage(cJSON* json_root, uint64 sender_client_id)
{
    boole status1 = FALSE;
    boole status0 = FALSE;
    cJSON* json_value = 0;
    cJSON* json_message_object = 0;
    boole status = FALSE;
    int received_microphone_usage = 0;
    client_t* client = 0;
    boole is_served = FALSE;

    status = _client_msg_audio_internal__is_json_process_microphone_usage_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__audio__process_microphone_usage _client_msg_audio_internal__is_json_process_microphone_usage_valid == FALSE \n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);
    status1 = base__is_current_client_request(sender_client_id);
    if (status1 == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    client = &g_clients_array[sender_client_id];

    if (client->is_authenticated == FALSE)
    {
        goto label_client_msg__process_microphone_usage_end;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");

    received_microphone_usage = json_value->valueint;

    // every state needs an audio connection the server holds for this client while audio is on: a
    // dropped connection set "audio disabled", an attach resets it to listening, and the client then
    // reports its state again (talking, microphone ready, or listening only)
    clib__read_lock(&g_webrtc_peers_rwlock_guard);
    status0 = g_server_settings.is_voice_chat_active == TRUE;
    if (status0 == FALSE)
    {
        status0 = g_server_settings.is_music_bot_audio_active == TRUE;
    }
    if (status0 == TRUE)
    {
        status0 = client->is_audio_websocket_authenticated == TRUE;
        if (status0 == FALSE)
        {
            status0 = g_server_settings.is_audio_over_webrtc_allowed == TRUE;
            if (status0 == TRUE)
            {
                status0 = audio_webrtc__is_connected(client->client_id) == TRUE;
            }
        }
    }
    is_served = status0;
    clib__unlock(&g_webrtc_peers_rwlock_guard);

    if (is_served == TRUE)
    {
        if (received_microphone_usage == MICROPHONE_USAGE__KEEP_PUSH_TO_TALK_READY_BUT_DONT_SEND_AUDIO)
        {
            if (client->audio_state != AUDIO_STATE__PUSH_TO_TALK_ENABLED)
            {
                client->audio_state = AUDIO_STATE__PUSH_TO_TALK_ENABLED;
                audio_webrtc__set_is_client_sending_audio(client->client_id, FALSE);
                server_msg__audio__send_state_of_client_to_all_clients(client, client->audio_state);
            }
        }
        else if (received_microphone_usage == MICROPHONE_USAGE__DISABLE_PUSH_TO_TALK)
        {
            if (client->audio_state != AUDIO_STATE__PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS)
            {
                client->audio_state = AUDIO_STATE__PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS;

                if (client->is_streaming_song == TRUE)
                {
                    client->is_streaming_song = FALSE;
                    server_msg__musicbot__send_stop_song_stream_message_to_clients_in_same_channel(client);
                }

                audio_webrtc__set_is_client_sending_audio(client->client_id, FALSE);
                server_msg__audio__send_state_of_client_to_all_clients(client, client->audio_state);
            }
        }
        else if (received_microphone_usage == MICROPHONE_USAGE__ACTIVATE_PUSH_TO_TALK_AND_SEND_AUDIO)
        {
            clib__read_lock(&g_channels_global_rwlock_guard);
            status = client->channel_id < g_server_settings.max_channel_count
                && permissions__can_use_channel(client, &g_channel_array[client->channel_id], PERMISSION_TALK_POWER);
            clib__unlock(&g_channels_global_rwlock_guard);
            if (status == FALSE)
            {
                server_msg__administration__send_access_denied_to_single_client(client, NULL_POINTER);
                goto label_client_msg__process_microphone_usage_end;
            }
            if (client->audio_state != MICROPHONE_USAGE__ACTIVATE_PUSH_TO_TALK_AND_SEND_AUDIO)
            {
                client->audio_state = MICROPHONE_USAGE__ACTIVATE_PUSH_TO_TALK_AND_SEND_AUDIO;
                audio_webrtc__set_is_client_sending_audio(client->client_id, TRUE);
                server_msg__audio__send_state_of_client_to_all_clients(client, client->audio_state);
            }
        }
    }

label_client_msg__process_microphone_usage_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes a client's request to open a new WebRTC datachannel connection for voice
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__audio__process_create_new_webrtc_datachannel_connection(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole status0 = FALSE;
    boole status1 = FALSE;
    boole status2 = FALSE;
    boole status3 = FALSE;
    boole status4 = FALSE;
    client_t* client = NULL_POINTER;
    webrtc_peer_t* peer = NULL_POINTER;
    uint64 timestamp_now = 0;
    uint64 cooldown_seconds = 0;
    boole initialize = FALSE;

    (void)json_root;
    clib__write_lock(&g_clients_global_rwlock_guard);
    status = base__is_current_client_request(sender_client_id);
    if (status == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    client = &g_clients_array[sender_client_id];
    status0 = client->is_authenticated == FALSE;
    status1 = client->is_existing == FALSE;
    status2 = g_server_settings.is_voice_chat_active == FALSE;
    status3 = g_server_settings.is_music_bot_audio_active == FALSE;
    status4 = g_server_settings.is_video_streaming_active == FALSE;
    if (status0 == TRUE || status1 == TRUE
        || (status2 == TRUE && status3 == TRUE && status4 == TRUE))
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    timestamp_now = util__get_timestamp_ms();
    clib__write_lock(&g_webrtc_peers_rwlock_guard);
    peer = &g_webrtc_peers_array[sender_client_id];
    if (peer->is_existing == TRUE && peer->connected == TRUE && client->audio_state != AUDIO_STATE__AUDIO_COMPLETELY_DISABLED)
    {
        initialize = FALSE;
    }
    else if (peer->cooldown_until_ms > timestamp_now)
    {
        cooldown_seconds = (peer->cooldown_until_ms - timestamp_now + 999) / 1000;
    }
    else if (peer->attempts_since_connected >= WEBRTC_DATACHANNEL_ATTEMPTS_BEFORE_COOLDOWN
        && g_server_settings.webrtc_datachannel_cooldown_seconds > 0)
    {
        cooldown_seconds = (uint64)g_server_settings.webrtc_datachannel_cooldown_seconds;
        peer->cooldown_until_ms = timestamp_now + cooldown_seconds * 1000;
        peer->attempts_since_connected = 0;
    }
    else
    {
        peer->attempts_since_connected++;
        initialize = TRUE;
    }
    clib__unlock(&g_webrtc_peers_rwlock_guard);

    if (cooldown_seconds != 0)
    {
        server_msg__audio__send_datachannel_cooldown_to_single_client(client, cooldown_seconds);
    }
    if (initialize == TRUE)
    {
        // Initialization takes the peers lock itself, after the snapshot above.
        audio_webrtc__initialize_datachannel_connection(client);
    }
    clib__unlock(&g_clients_global_rwlock_guard);
}
