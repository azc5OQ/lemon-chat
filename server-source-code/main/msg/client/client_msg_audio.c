// This file checks requests to set up WebRTC and change the client's microphone state.
#include "client_msg.h"
#include "../server/server_msg.h"
#include "../../audio_webrtc.h"
#include "../../base.h"
#include "../../util.h"
#include "../../clib/clib_string.h"
#include "../../clib/clib_memory.h"
#include "../../../third-party/rxi-log/log.h"

// declarations
static boole _client_msg_audio_internal__is_json_sdp_answer_format_valid(cJSON* json_root, uint64 client_id);
static boole _client_msg_audio_internal__is_ice_candidate_format_valid(cJSON* json_root, uint64 client_id);
static boole _client_msg_audio_internal__is_json_process_microphone_usage_valid(cJSON* json_root);

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @return boole
 */
static boole _client_msg_audio_internal__is_json_sdp_answer_format_valid(cJSON* json_root, uint64 client_id)
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
    boole status = FALSE;
    cJSON* json_message_object = 0;
    cJSON* json_message_value = 0;

    DBG_AUDIO_WEBRTC log_info("%s", "client_msg__audio__process_sdp_answer");

    status = _client_msg_audio_internal__is_json_sdp_answer_format_valid(json_root, sender_client_id);
    if (status == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s", "client_msg__audio__process_sdp_answer client_msg__is_json_sdp_answer_format_valid == FALSE \n");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_message_value = cJSON_GetObjectItem(json_message_object, "value");

    clib__read_lock(&g_clients_global_rwlock_guard);

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
static boole _client_msg_audio_internal__is_ice_candidate_format_valid(cJSON* json_root, uint64 client_id)
{
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
    if (cJSON_IsNumber(json_sdpMLineIndex) == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(json_sdpMLineIndex) \n");
        return FALSE;
    }

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
    boole status = FALSE;
    cJSON* json_message_object = 0;
    cJSON* json_message_value = 0;

    DBG_AUDIO_WEBRTC log_info("%s", "client_msg__audio__process_ice_candidate");

    status = _client_msg_audio_internal__is_ice_candidate_format_valid(json_root, sender_client_id);
    if (status == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s", "client_msg__audio__process_ice_candidate client_msg__is_ice_candidate_format_valid == FALSE \n");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_message_value = cJSON_GetObjectItem(json_message_object, "value");

    clib__read_lock(&g_clients_global_rwlock_guard);

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

    if (json_value->valueint < 1 || json_value->valueint > 3)
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
    is_served = (g_server_settings.is_voice_chat_active == TRUE || g_server_settings.is_music_bot_audio_active == TRUE) && (client->is_audio_websocket_authenticated == TRUE || (g_server_settings.is_audio_over_webrtc_allowed == TRUE && audio_webrtc__is_connected(client->client_id) == TRUE));
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
    client_t* client = 0;
    webrtc_peer_t* peer = 0;
    uint64 timestamp_now = 0;

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_channels_global_rwlock_guard);

    // check if the client that sent the message is valid: connected and existing.
    // this was checked before but not within write lock like here

    // log_info("%s", "client_msg__audio__process_create_new_webrtc_datachannel_connection");
    client = &g_clients_array[sender_client_id];

    if (client->is_authenticated == FALSE || client->is_existing == FALSE)
    {
        goto label_process_create_new_webrtc_datachannel_connection_end;
    }

    // nothing would travel on the datachannel: no voice, no music bot audio and no video streaming
    if (g_server_settings.is_voice_chat_active == FALSE && g_server_settings.is_music_bot_audio_active == FALSE && g_server_settings.is_video_streaming_active == FALSE)
    {
        goto label_process_create_new_webrtc_datachannel_connection_end;
    }

    // server will ignore the request only if these 3 are off
    peer = &g_webrtc_peers_array[sender_client_id];

    if (peer->is_existing == TRUE && peer->connected == TRUE && client->audio_state != AUDIO_STATE__AUDIO_COMPLETELY_DISABLED)
    {
        goto label_process_create_new_webrtc_datachannel_connection_end;
    }

    // a client whose transport never comes up (blocked udp) asks every 10 s forever, and every ask
    // is a new peer plus a teardown thread. after N attempts without RTC_CONNECTED it waits the
    // configured cooldown; a fresh session starts from zero, a connected transport resets the count
    timestamp_now = util__get_timestamp_ms();

    if (peer->cooldown_until_ms > timestamp_now)
    {
        server_msg__audio__send_datachannel_cooldown_to_single_client(client, (peer->cooldown_until_ms - timestamp_now + 999) / 1000);
        goto label_process_create_new_webrtc_datachannel_connection_end;
    }

    if (peer->attempts_since_connected >= WEBRTC_DATACHANNEL_ATTEMPTS_BEFORE_COOLDOWN && g_server_settings.webrtc_datachannel_cooldown_seconds > 0)
    {
        peer->cooldown_until_ms = timestamp_now + (uint64)g_server_settings.webrtc_datachannel_cooldown_seconds * 1000;
        peer->attempts_since_connected = 0;
        log_info("%s %s %s %s %s %lld %s", "datachannel cooldown:", client->username, "(ip", client->ip_address, ") never connected, no new peers for", g_server_settings.webrtc_datachannel_cooldown_seconds, "s \n");
        server_msg__audio__send_datachannel_cooldown_to_single_client(client, (uint64)g_server_settings.webrtc_datachannel_cooldown_seconds);
        goto label_process_create_new_webrtc_datachannel_connection_end;
    }

    peer->attempts_since_connected++;

    // log_info("%s", "attempting reconnect");
    audio_webrtc__initialize_datachannel_connection(client);

label_process_create_new_webrtc_datachannel_connection_end:
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}
