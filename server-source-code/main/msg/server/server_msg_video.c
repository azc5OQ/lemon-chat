// This file tells clients about video streams, viewing requests and requests for a complete video frame.

#include "server_msg.h"
#include "../../base.h"
#include "../../../third-party/rxi-log/log.h"

/**
 * @brief tells every client in a channel that a video stream started or ended there, with what the streamer announced while it runs
 *        the streamer gets it too, that is how its popup confirms the start; late joiners to the
 *        server learn about a running stream from is_streaming_video in the client list instead
 *
 * @param uint64 channel_id -> the channel
 * @param boole is_active -> TRUE for a start, FALSE for an end
 * @param uint64 streamer_client_id -> the streamer (still valid on an end, its fields are already cleared then)
 * @param cstring reason -> "started", "stopped", "streamer_left", "channel_disabled", "disabled", ...
 *
 * @return void
 */
void server_msg__video__send_state_to_clients_in_same_channel(uint64 channel_id, boole is_active, uint64 streamer_client_id, cstring reason)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    client_t* streamer = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("video_stream_state", &message);

    cJSON_AddNumberToObject(message, "channel_id", (double)channel_id);
    cJSON_AddNumberToObject(message, "streamer_client_id", (double)streamer_client_id);
    cJSON_AddBoolToObject(message, "is_active", is_active);
    cJSON_AddStringToObject(message, "reason", reason);

    if (is_active == TRUE && streamer_client_id < g_server_settings.max_client_count)
    {
        streamer = &g_clients_array[streamer_client_id];
        cJSON_AddStringToObject(message, "source", streamer->video_stream_source);
        cJSON_AddStringToObject(message, "codec", streamer->video_stream_codec);
        cJSON_AddNumberToObject(message, "width", (double)streamer->video_stream_width);
        cJSON_AddNumberToObject(message, "height", (double)streamer->video_stream_height);
        cJSON_AddNumberToObject(message, "fps", (double)streamer->video_stream_fps);
        cJSON_AddBoolToObject(message, "is_sound_included", streamer->is_video_stream_sound_included);
    }

    server_msg__delivery__send_json_to_channel(root, channel_id, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief hands one member the stream's offer: it may now press connect. sent to everybody present at the start and to a later joiner the streamer allowed
 *
 * @param client_t* receiver -> the member
 * @param client_t* streamer -> the streamer, whose announced source / codec / size are sent with it so the receiver can probe its decoder first, with whether sound is included and the ids of the members watching right now
 *
 * @return void
 */
void server_msg__video__send_offer_to_single_client(client_t* receiver, client_t* streamer)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* watching_client_ids = NULL_POINTER;
    client_t* member = NULL_POINTER;
    uint64 i = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("video_stream_offer", &message);

    cJSON_AddNumberToObject(message, "streamer_client_id", (double)streamer->client_id);
    cJSON_AddStringToObject(message, "source", streamer->video_stream_source);
    cJSON_AddStringToObject(message, "codec", streamer->video_stream_codec);
    cJSON_AddNumberToObject(message, "width", (double)streamer->video_stream_width);
    cJSON_AddNumberToObject(message, "height", (double)streamer->video_stream_height);
    cJSON_AddNumberToObject(message, "fps", (double)streamer->video_stream_fps);
    cJSON_AddBoolToObject(message, "is_sound_included", streamer->is_video_stream_sound_included);

    // who watches right now, so the receiver can mark those rows at once; every later change reaches
    // it as a video_stream_viewer_state message, which an allowed member receives from here on
    watching_client_ids = cJSON_AddArrayToObject(message, "watching_client_ids");
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        member = &g_clients_array[i];
        if (member->is_existing == TRUE && member->channel_id == streamer->channel_id && member->is_video_viewer_watching == TRUE)
        {
            cJSON_AddItemToArray(watching_client_ids, cJSON_CreateNumber((double)member->client_id));
        }
    }

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief asks the streamer whether a member that joined the channel after the start may watch
 *
 * @param client_t* receiver -> the streamer
 * @param client_t* newcomer -> the member that just arrived
 *
 * @return void
 */
void server_msg__video__send_viewer_request_to_single_client(client_t* receiver, client_t* newcomer)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("video_stream_viewer_request", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)newcomer->client_id);
    cJSON_AddStringToObject(message, "username", newcomer->username);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief tells one client that a viewer connected or disconnected. the streamer counts its viewers from it and sends a keyframe for a
 *        new one; a member the streamer allowed marks the watching row in its channel tree. video_channel.c sends it to those clients
 *        only, so a member that was not allowed never learns who watches
 *
 * @param client_t* receiver -> the streamer, or a member the streamer allowed
 * @param uint64 viewer_client_id -> the viewer
 * @param boole is_watching -> TRUE when it connected, FALSE when it dropped out
 *
 * @return void
 */
void server_msg__video__send_viewer_state_to_single_client(client_t* receiver, uint64 viewer_client_id, boole is_watching)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("video_stream_viewer_state", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)viewer_client_id);
    cJSON_AddBoolToObject(message, "is_watching", is_watching);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief asks the streamer for a keyframe on behalf of a viewer that lost frames
 *
 * @param client_t* receiver -> the streamer
 *
 * @return void
 */
void server_msg__video__send_keyframe_request_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("video_stream_keyframe_request", &message);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief tells a client why its stream request was refused ("disabled", "channel_disabled", "busy", "not_allowed", "no_stream", "revoked")
 *
 * @param client_t* receiver -> the client that asked
 * @param cstring reason -> the short reason code sent to the clients
 *
 * @return void
 */
void server_msg__video__send_refused_to_single_client(client_t* receiver, cstring reason)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("video_stream_refused", &message);

    cJSON_AddStringToObject(message, "reason", reason);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}
