// This file sends microphone status, audio connection details and WebRTC connection offers.

#include "server_msg.h"
#include "../../base.h"
#include "../../../third-party/rxi-log/log.h"

/**
 * @brief Sends the microphone states of clients in the selected channel.
 *
 * @param client_t* receiver -> the client that receives it
 * @param uint64 channel_id -> channel whose microphone states are reported
 *
 * @attention it doesnt need readlock, already called within writelock in msg/client/
 *
 * @return void
 */
void server_msg__audio__send_active_microphone_usage_for_current_channel_to_single_client(client_t* receiver, uint64 channel_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_clients_array = 0;
    uint64 x = 0;
    client_t* client_in_loop = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("current_channel_active_microphone_usage", &message);
    json_clients_array = cJSON_CreateArray();

    // create array of clients
    for (x = 0; x < g_server_settings.max_client_count; x++)
    {
        cJSON* single_object = NULL_POINTER;

        client_in_loop = &g_clients_array[x];
        if (client_in_loop->is_existing == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        if (client_in_loop->channel_id != channel_id)
        {
            continue;
        }

        // have to send even to ourselves
        // if receiver.client_id == current_client_id {
        // continue;
        // }

        // only active mics are relevant since this is "active microphone usage"
        // though
        if (client_in_loop->audio_state != AUDIO_STATE__PUSH_TO_TALK_ACTIVE)
        {
            continue;
        }

        single_object = cJSON_CreateObject();
        cJSON_AddNumberToObject(single_object, "client_id", client_in_loop->client_id);
        cJSON_AddNumberToObject(single_object, "audio_state", client_in_loop->audio_state);
        cJSON_AddBoolToObject(single_object, "is_streaming_song", client_in_loop->is_streaming_song);
        cJSON_AddBoolToObject(single_object, "is_streaming_video", client_in_loop->is_streaming_video);
        cJSON_AddStringToObject(single_object, "song_name", client_in_loop->song_name);

        cJSON_AddItemToArray(json_clients_array, single_object);
    }

    cJSON_AddItemToObject(message, "clients", json_clients_array);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief tells a client the server will build no datachannel for it for a while: its attempts kept
 *        failing (blocked udp, usually). the client sleeps that long instead of asking every 10 s
 *
 * @param client_t* receiver -> the client whose request was refused
 * @param uint64 seconds_left -> how long the refusal lasts
 *
 * @return void
 */
void server_msg__audio__send_datachannel_cooldown_to_single_client(client_t* receiver, uint64 seconds_left)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("datachannel_cooldown", &message);

    cJSON_AddNumberToObject(message, "seconds_left", (double)seconds_left);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief sends a WebRTC SDP offer to a single client
 *
 * @param client_t* receiver -> the client to send the candidate to
 * @param const char* candidate -> the ICE candidate string
 * @param const char* mid -> the sdpMid the candidate belongs to
 *
 * @return void
 */
void server_msg__audio__send_webrtc_sdp_offer_to_single_client(client_t* receiver, const char* candidate, const char* mid)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_message_value = NULL_POINTER;
    cJSON* jmid = NULL_POINTER;
    cJSON* jsoncandidate = NULL_POINTER;
    cJSON* jsonlineindex = NULL_POINTER;

    root = server_msg__delivery__new_message("ice_candidate", &message);
    json_message_value = cJSON_CreateObject();

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    jsoncandidate = cJSON_CreateString(candidate);
    cJSON_AddItemToObject(json_message_value, "candidate", jsoncandidate);

    jmid = cJSON_CreateString(mid);
    cJSON_AddItemToObject(json_message_value, "sdpMid", jmid);

    jsonlineindex = cJSON_CreateNumber(0.0);
    cJSON_AddItemToObject(json_message_value, "sdpMLineIndex", jsonlineindex);

    cJSON_AddItemToObject(message, "value", json_message_value);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief broadcasts a client's audio state to all clients
 *
 * @param client_t* subject -> the client whose audio state is broadcast
 * @param uint64 state -> the client's new audio state
 *
 * @attention clients read lock held by the caller; the channels array is read for the hidden check
 *
 * @return void
 */
void server_msg__audio__send_state_of_client_to_all_clients(client_t* subject, uint64 state)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* root_other_channel = NULL_POINTER;
    cJSON* message_other_channel = NULL_POINTER;
    char* plaintext = NULL_POINTER;
    char* plaintext_other_channel = NULL_POINTER;
    uint64 i = 0;
    client_t* client = NULL_POINTER;
    boole is_subject_hidden = FALSE;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    // two variants, each printed once: the subject's channel hears the real state, the rest of the server
    // only learns that the microphone is on or off, never that it is transmitting right now
    root = server_msg__delivery__new_message("audio_state_of_single_client", &message);
    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);
    cJSON_AddNumberToObject(message, "value", (double)state);
    plaintext = cJSON_PrintUnformatted(root);

    root_other_channel = server_msg__delivery__new_message("audio_state_of_single_client", &message_other_channel);
    cJSON_AddNumberToObject(message_other_channel, "client_id", (double)subject->client_id);
    cJSON_AddNumberToObject(message_other_channel, "value", (double)(state == AUDIO_STATE__PUSH_TO_TALK_ACTIVE ? AUDIO_STATE__PUSH_TO_TALK_ENABLED : state));
    plaintext_other_channel = cJSON_PrintUnformatted(root_other_channel);

    // while clients in password protected channels are hidden, a subject standing in one is not reported outside it
    is_subject_hidden = g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE && subject->channel_id < g_server_settings.max_channel_count && g_channel_array[subject->channel_id].is_existing == TRUE && g_channel_array[subject->channel_id].is_using_password == TRUE;

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        if (base__is_client_valid_and_not_music_bot((int)i) == FALSE)
        {
            continue;
        }
        if (client->channel_id == subject->channel_id)
        {
            server_msg__delivery__send_text_to_client(client, plaintext);
        }
        else if (is_subject_hidden == FALSE)
        {
            server_msg__delivery__send_text_to_client(client, plaintext_other_channel);
        }
    }

    cJSON_free(plaintext);
    cJSON_free(plaintext_other_channel);
    cJSON_Delete(root);
    cJSON_Delete(root_other_channel);
}
