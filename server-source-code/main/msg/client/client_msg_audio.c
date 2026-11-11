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
static boole _client_msg_audio_internal__is_json_sdp_answer_format_valid(cJSON *json_root, uint64 client_id, cJSON **checked_value);
static boole _client_msg_audio_internal__is_ice_candidate_format_valid(cJSON *json_root, uint64 client_id, cJSON **checked_value);
static boole _client_msg_audio_internal__is_json_process_microphone_usage_valid(cJSON *json_root);

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @return boole
 */
static boole _client_msg_audio_internal__is_json_sdp_answer_format_valid(cJSON *json_root, uint64 client_id, cJSON **checked_value)
{
    cJSON *json_sdp_message_value = NULL_POINTER;
    cJSON *json_value_object = NULL_POINTER;
    cJSON *json_sdp_message_type = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;

    // read the message field
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    // check the value object
    json_value_object = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    if (cJSON_IsObject(json_value_object) == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " cJSON_IsObject(json_value_object) \n");
        return FALSE;
    }

    // check the type field
    json_sdp_message_type = cJSON_GetObjectItemCaseSensitive(json_value_object, "type");
    if (cJSON_IsString(json_sdp_message_type) == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_sdp_message_type) \n");
        return FALSE;
    }

    // refuse a null type
    if (json_sdp_message_type->valuestring == NULL_POINTER)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " json_sdp_message_type->valuestring == NULL \n");
        return FALSE;
    }

    // refuse an empty type
    if (clib__utf8_string_length(json_sdp_message_type->valuestring) == 0)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " clib__utf8_string_length(json_sdp_message_type->valuestring) \n");
        return FALSE;
    }

    // check the SDP field
    json_sdp_message_value = cJSON_GetObjectItemCaseSensitive(json_value_object, "sdp");
    if (cJSON_IsString(json_sdp_message_value) == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_sdp_message_value) \n");
        return FALSE;
    }

    // refuse a null SDP
    if (json_sdp_message_value->valuestring == NULL_POINTER)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " json_sdp_message_value->valuestring == NULL \n");
        return FALSE;
    }

    // refuse an empty SDP
    if (clib__utf8_string_length(json_sdp_message_value->valuestring) == 0)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " clib__utf8_string_length(json_sdp_message_value->valuestring) \n");
        return FALSE;
    }

    // store the checked object
    *checked_value = json_value_object;
    return TRUE;
}

/**
 * @brief This function processes sdp answer for offer that server sent to client as part of process
 *        of establishing webrtc datachannel connection
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__audio__process_sdp_answer(cJSON *json_root, uint64 sender_client_id)
{
    cJSON *json_message_value = NULL_POINTER;

    // log the request
    DBG_AUDIO_WEBRTC log_info("%s", "client_msg__audio__process_sdp_answer");

    // check the answer format
    if (_client_msg_audio_internal__is_json_sdp_answer_format_valid(json_root, sender_client_id, &json_message_value) == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s", "client_msg__audio__process_sdp_answer client_msg__is_json_sdp_answer_format_valid == FALSE \n");
        return;
    }

    // keep the exact object validated above; differently cased keys cannot replace it
    clib__read_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // set the remote description
    if (g_clients_array[sender_client_id].is_authenticated == TRUE)
    {
        DBG_AUDIO_WEBRTC log_info("%s", "client_msg__audio__process_sdp_answer got here");

        audio_webrtc__set_remote_description(&g_clients_array[sender_client_id], cJSON_GetObjectItemCaseSensitive(json_message_value, "sdp")->valuestring, cJSON_GetObjectItemCaseSensitive(json_message_value, "type")->valuestring);
    }

    // unlock the clients
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
static boole _client_msg_audio_internal__is_ice_candidate_format_valid(cJSON *json_root, uint64 client_id, cJSON **checked_value)
{
    cJSON *json_candidate = NULL_POINTER;
    cJSON *json_value_object = NULL_POINTER;
    cJSON *json_sdpMid = NULL_POINTER;
    cJSON *json_sdpMLineIndex = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;

    // read the message field
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    // check the value object
    json_value_object = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    if (cJSON_IsObject(json_value_object) == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " cJSON_IsObject(json_value_object) \n");
        return FALSE;
    }

    // check the candidate field
    json_candidate = cJSON_GetObjectItemCaseSensitive(json_value_object, "candidate");
    if (cJSON_IsString(json_candidate) == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_candidate) \n");
        return FALSE;
    }

    // refuse a null candidate
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

    // refuse a null sdpMid
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
    if (cJSON_IsNumber(json_sdpMLineIndex) == FALSE ||
        json_sdpMLineIndex->valueint < 0 ||
        json_sdpMLineIndex->valuedouble != (double)json_sdpMLineIndex->valueint)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(json_sdpMLineIndex) \n");
        return FALSE;
    }

    // store the checked object
    *checked_value = json_value_object;
    return TRUE;
}

/**
 * @brief This function processes ice candidate. When I tried this on local network, this was not
 *        needed. Still, good to send and process
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__audio__process_ice_candidate(cJSON *json_root, uint64 sender_client_id)
{
    cJSON *json_message_value = NULL_POINTER;

    // log the request
    DBG_AUDIO_WEBRTC log_info("%s", "client_msg__audio__process_ice_candidate");

    // check the candidate format
    if (_client_msg_audio_internal__is_ice_candidate_format_valid(json_root, sender_client_id, &json_message_value) == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s", "client_msg__audio__process_ice_candidate client_msg__is_ice_candidate_format_valid == FALSE \n");
        return;
    }

    // keep the exact object validated above; differently cased keys cannot replace it
    clib__read_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // add the remote candidate
    if (g_clients_array[sender_client_id].is_authenticated == TRUE)
    {
        DBG_AUDIO_WEBRTC log_info("%s", "client_msg__audio__process_ice_candidate got here");

        audio_webrtc__add_remote_candidate(&g_clients_array[sender_client_id], cJSON_GetObjectItemCaseSensitive(json_message_value, "candidate")->valuestring);
    }

    // unlock the clients
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief validates a microphone-usage message: the usage value is present and within the 1..3 range
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole
 */
static boole _client_msg_audio_internal__is_json_process_microphone_usage_valid(cJSON *json_root)
{
    cJSON *json_value = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;

    // read the message field
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    // value must be number
    json_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    if (cJSON_IsNumber(json_value) == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s", "_client_msg_audio_internal__is_json_process_microphone_usage_valid cJSON_IsNumber(json_value_object) \n");
        return FALSE;
    }

    // check the usage range
    if (json_value->valueint < 1 || json_value->valueint > 3 || json_value->valuedouble != (double)json_value->valueint)
    {
        DBG_AUDIO_WEBRTC log_info("%s", "_client_msg_audio_internal__is_json_process_microphone_usage_valid json_value->valueint < 1 || json_value->valueint > 3 \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief processes a client's microphone-usage change and broadcasts the resulting audio state to
 *        the channel
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__audio__process_microphone_usage(cJSON *json_root, uint64 sender_client_id)
{
    boole status0 = FALSE;
    cJSON *json_value = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;
    boole status = FALSE;
    int64 received_microphone_usage = 0;
    client_t *client = NULL_POINTER;
    boole is_served = FALSE;

    // check the usage value
    status = _client_msg_audio_internal__is_json_process_microphone_usage_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__audio__process_microphone_usage _client_msg_audio_internal__is_json_process_microphone_usage_valid == FALSE \n");
        return;
    }

    // lock the clients
    clib__write_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // find the client
    client = &g_clients_array[sender_client_id];

    // client must be authenticated
    if (client->is_authenticated == FALSE)
    {
        goto label_client_msg__process_microphone_usage_end;
    }

    // read the microphone usage
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");

    received_microphone_usage = json_value->valueint;

    // every state needs an audio connection the server holds for this client while audio is on: a
    // dropped connection set "audio disabled", an attach resets it to listening, and the client then
    // reports its state again (talking, microphone ready, or listening only)
    clib__read_lock(&g_webrtc_peers_rwlock_guard);
    status0 = ((g_server_settings.is_voice_chat_active == TRUE) || (g_server_settings.is_music_bot_audio_active == TRUE)) && ((client->is_audio_websocket_authenticated == TRUE) || ((g_server_settings.is_audio_over_webrtc_allowed == TRUE) && (audio_webrtc__is_connected(client->client_id) == TRUE)));
    is_served = status0;
    clib__unlock(&g_webrtc_peers_rwlock_guard);

    // change the audio state
    if (is_served == TRUE)
    {
        if (received_microphone_usage == MICROPHONE_USAGE__KEEP_PUSH_TO_TALK_READY_BUT_DONT_SEND_AUDIO)
        {
            // stop sending, keep ready
            if (client->audio_state != AUDIO_STATE__PUSH_TO_TALK_ENABLED)
            {
                client->audio_state = AUDIO_STATE__PUSH_TO_TALK_ENABLED;
                audio_webrtc__set_is_client_sending_audio(client->client_id, FALSE);
                server_msg__audio__send_state_of_client_to_all_clients(client, client->audio_state);
            }
        }
        else if (received_microphone_usage == MICROPHONE_USAGE__DISABLE_PUSH_TO_TALK)
        {
            // change to listening only
            if (client->audio_state != AUDIO_STATE__PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS)
            {
                client->audio_state = AUDIO_STATE__PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS;

                // stop the song stream
                if (client->is_streaming_song == TRUE)
                {
                    client->is_streaming_song = FALSE;
                    server_msg__musicbot__send_stop_song_stream_message_to_clients_in_same_channel(client);
                }

                // stop audio, send state
                audio_webrtc__set_is_client_sending_audio(client->client_id, FALSE);
                server_msg__audio__send_state_of_client_to_all_clients(client, client->audio_state);
            }
        }
        else if (received_microphone_usage == MICROPHONE_USAGE__ACTIVATE_PUSH_TO_TALK_AND_SEND_AUDIO)
        {
            // check the talk power
            clib__read_lock(&g_channels_global_rwlock_guard);
            status = client->channel_id < g_server_settings.max_channel_count && permissions__can_use_channel(client, &g_channel_array[client->channel_id], PERMISSION_TALK_POWER);
            clib__unlock(&g_channels_global_rwlock_guard);
            if (status == FALSE)
            {
                server_msg__administration__send_permission_denied_to_single_client(client, permissions__power_name(PERMISSION_TALK_POWER));
                goto label_client_msg__process_microphone_usage_end;
            }

            // start sending audio
            if (client->audio_state != MICROPHONE_USAGE__ACTIVATE_PUSH_TO_TALK_AND_SEND_AUDIO)
            {
                client->audio_state = MICROPHONE_USAGE__ACTIVATE_PUSH_TO_TALK_AND_SEND_AUDIO;
                audio_webrtc__set_is_client_sending_audio(client->client_id, TRUE);
                server_msg__audio__send_state_of_client_to_all_clients(client, client->audio_state);
            }
        }
    }

label_client_msg__process_microphone_usage_end:
    // unlock the clients
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
void client_msg__audio__process_create_new_webrtc_datachannel_connection(cJSON *json_root, uint64 sender_client_id)
{
    client_t *client = NULL_POINTER;
    webrtc_peer_t *peer = NULL_POINTER;
    uint64 timestamp_now = 0;
    uint64 cooldown_seconds = 0;
    boole initialize = FALSE;

    // JSON is not used
    (void)json_root;

    // lock the clients
    clib__write_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // check client and settings
    client = &g_clients_array[sender_client_id];
    if (client->is_authenticated == FALSE || client->is_existing == FALSE || (g_server_settings.is_voice_chat_active == FALSE && g_server_settings.is_music_bot_audio_active == FALSE && g_server_settings.is_video_streaming_active == FALSE))
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // check attempts and cooldown
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
    else if (peer->attempts_since_connected >= WEBRTC_DATACHANNEL_ATTEMPTS_BEFORE_COOLDOWN && g_server_settings.webrtc_datachannel_cooldown_seconds > 0)
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

    // send the cooldown time
    if (cooldown_seconds != 0)
    {
        server_msg__audio__send_datachannel_cooldown_to_single_client(client, cooldown_seconds);
    }

    // start the datachannel connection
    if (initialize == TRUE)
    {
        // initialization takes the peers lock itself, after the snapshot above
        audio_webrtc__initialize_datachannel_connection(client);
    }

    // unlock the clients
    clib__unlock(&g_clients_global_rwlock_guard);
}
