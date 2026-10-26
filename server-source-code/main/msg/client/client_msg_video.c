// This file checks requests to start, stop or watch video streams and to allow viewers.
#include "client_msg.h"
#include "../server/server_msg.h"
#include "../../core/video_channel.h"
#include "../../base.h"
#include "../../clib/clib_string.h"
#include "../../clib/clib_memory.h"
#include "../../../third-party/rxi-log/log.h"

// declarations
static boole _client_msg_video_internal__is_json_video_stream_start_valid(cJSON* json_root);

/**
 * @brief checks a video_stream_start request: source is "screen" or "file", codec is a short webcodecs string of letters, digits and dots, width / height / fps are numbers inside the server's caps, is_sound_included is a boolean when present
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole TRUE when every field is present and sane
 */
static boole _client_msg_video_internal__is_json_video_stream_start_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* json_source = 0;
    cJSON* json_codec = 0;
    cJSON* json_width = 0;
    cJSON* json_height = 0;
    cJSON* json_fps = 0;
    cJSON* json_is_sound_included = 0;
    uint64 codec_length = 0;
    uint64 i = 0;
    char c = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == NULL_POINTER)
    {
        return FALSE;
    }

    json_source = cJSON_GetObjectItemCaseSensitive(json_message_object, "source");
    if (cJSON_IsString(json_source) == FALSE || json_source->valuestring == NULL_POINTER)
    {
        return FALSE;
    }
    if (clib__is_string_equal(json_source->valuestring, "screen") == FALSE && clib__is_string_equal(json_source->valuestring, "file") == FALSE)
    {
        return FALSE;
    }

    json_codec = cJSON_GetObjectItemCaseSensitive(json_message_object, "codec");
    if (cJSON_IsString(json_codec) == FALSE || json_codec->valuestring == NULL_POINTER)
    {
        return FALSE;
    }

    codec_length = clib__utf8_string_length(json_codec->valuestring);
    if (codec_length == 0 || codec_length >= VIDEO_STREAM_CODEC_MAX_LENGTH)
    {
        return FALSE;
    }

    // a webcodecs codec string: "avc1.42E01E", "vp8", "vp09.00.10.08" - nothing else gets stored or echoed
    for (i = 0; i < codec_length; i++)
    {
        c = json_codec->valuestring[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.')
        {
            continue;
        }
        return FALSE;
    }

    json_width = cJSON_GetObjectItemCaseSensitive(json_message_object, "width");
    json_height = cJSON_GetObjectItemCaseSensitive(json_message_object, "height");
    json_fps = cJSON_GetObjectItemCaseSensitive(json_message_object, "fps");

    if (cJSON_IsNumber(json_width) == FALSE || json_width->valuedouble < 16 || json_width->valuedouble > VIDEO_STREAM_MAX_DIMENSION)
    {
        return FALSE;
    }
    if (cJSON_IsNumber(json_height) == FALSE || json_height->valuedouble < 16 || json_height->valuedouble > VIDEO_STREAM_MAX_DIMENSION)
    {
        return FALSE;
    }
    if (cJSON_IsNumber(json_fps) == FALSE || json_fps->valuedouble < 1 || json_fps->valuedouble > VIDEO_STREAM_MAX_FPS)
    {
        return FALSE;
    }

    // optional: a client from before the stream sound never sends it, which means "no sound"
    json_is_sound_included = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_sound_included");
    if (json_is_sound_included != NULL_POINTER && cJSON_IsBool(json_is_sound_included) == FALSE)
    {
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief a client wants to stream its screen or a video file to its channel
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__video__process_start(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    client_t* client = 0;
    cJSON* json_message_object = 0;
    cJSON* json_is_sound_included = 0;
    cstring refusal_reason = "";

    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
    {
        return;
    }

    status = _client_msg_video_internal__is_json_video_stream_start_valid(json_root);
    if (status == FALSE)
    {
        DBG_VIDEO_CHANNEL log_info("%s", "client_msg__video__process_start request is not valid \n");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_channels_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    if (client->is_existing == TRUE && client->is_authenticated == TRUE)
    {
        json_is_sound_included = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_sound_included");

        status = video_channel__start(client, cJSON_GetObjectItemCaseSensitive(json_message_object, "source")->valuestring, cJSON_GetObjectItemCaseSensitive(json_message_object, "codec")->valuestring, (int64)cJSON_GetObjectItemCaseSensitive(json_message_object, "width")->valuedouble, (int64)cJSON_GetObjectItemCaseSensitive(json_message_object, "height")->valuedouble, (int64)cJSON_GetObjectItemCaseSensitive(json_message_object, "fps")->valuedouble, (boole)(cJSON_IsBool(json_is_sound_included) && cJSON_IsTrue(json_is_sound_included)), &refusal_reason);

        if (status == FALSE)
        {
            if (clib__is_string_equal(refusal_reason, "insufficient_permissions") == TRUE)
            {
                server_msg__administration__send_access_denied_to_single_client(client, NULL_POINTER);
            }
            server_msg__video__send_refused_to_single_client(client, refusal_reason);
        }
    }

    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief the streamer ends its own stream
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__video__process_stop(cJSON* json_root, uint64 sender_client_id)
{
    client_t* client = 0;

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_channels_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    if (client->is_existing == TRUE && client->is_authenticated == TRUE && client->is_streaming_video == TRUE && client->channel_id < g_server_settings.max_channel_count && g_channel_array[client->channel_id].video_streamer_client_id == sender_client_id)
    {
        video_channel__stop(client->channel_id, "stopped");
    }

    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief a viewer presses connect or disconnect on the stream of its channel
 *
 * @param cJSON* json_root -> the parsed client request, "is_watching" says which
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__video__process_watch(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    client_t* client = 0;
    cJSON* json_message_object = 0;
    cJSON* json_is_watching = 0;
    cstring refusal_reason = "";

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == NULL_POINTER)
    {
        return;
    }

    json_is_watching = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_watching");
    if (cJSON_IsBool(json_is_watching) == FALSE)
    {
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_channels_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    if (client->is_existing == TRUE && client->is_authenticated == TRUE)
    {
        status = video_channel__set_viewer_watching(client, (boole)cJSON_IsTrue(json_is_watching), &refusal_reason);

        if (status == FALSE)
        {
            server_msg__video__send_refused_to_single_client(client, refusal_reason);
        }
    }

    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief the streamer answers whether a member that joined after the start may watch (or revokes one)
 *
 * @param cJSON* json_root -> the parsed client request, "client_id" and "is_allowed"
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__video__process_allow_viewer(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    client_t* client = 0;
    cJSON* json_message_object = 0;
    cJSON* json_client_id = 0;
    cJSON* json_is_allowed = 0;

    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
    {
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == NULL_POINTER)
    {
        return;
    }

    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
    json_is_allowed = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_allowed");

    if (cJSON_IsNumber(json_client_id) == FALSE || json_client_id->valuedouble < 0 || json_client_id->valuedouble >= (double)g_server_settings.max_client_count)
    {
        return;
    }
    if (cJSON_IsBool(json_is_allowed) == FALSE)
    {
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_channels_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    if (client->is_existing == TRUE && client->is_authenticated == TRUE)
    {
        video_channel__allow_viewer(client, (uint64)json_client_id->valuedouble, (boole)cJSON_IsTrue(json_is_allowed));
    }

    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief a watching viewer lost frames and asks the streamer for a keyframe (rate limited per streamer)
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__video__process_keyframe_request(cJSON* json_root, uint64 sender_client_id)
{
    client_t* client = 0;

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_channels_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    if (client->is_existing == TRUE && client->is_authenticated == TRUE)
    {
        video_channel__forward_keyframe_request(client);
    }

    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}
