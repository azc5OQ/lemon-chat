#include "definitions.h"

#include "../third-party/dave-g-json/cJSON.h" // needed by base.h
#include "base.h"

#include "../third-party/libdatachannel-0.24.2/include/rtc/rtc.h"
#include "../third-party/rxi-log/log.h"

#include "clib/clib_memory.h"
#include "clib/clib_string.h"

#include "memory_manager.h"
#include "server_message.h"
#include "audio_channel.h"
#include "util.h"

#include "video_stream.h"

/**
 * @brief forgets a client's viewer consent and connection, the streamer is told when it was watching
 *
 * @param client_t* client -> the viewer
 * @param client_t* streamer -> the streamer to notify, or NULL when it must not be told (it is gone or is the one stopping)
 *
 * @return void
 */
static void _video_stream_internal__clear_viewer(client_t* client, client_t* streamer)
{
    boole was_watching = client->is_video_viewer_watching;

    client->is_video_viewer_allowed = FALSE;
    client->is_video_viewer_watching = FALSE;

    if (was_watching == TRUE && streamer != NULL_POINTER)
    {
        server_msg__send_video_stream_viewer_state_to_single_client(streamer, client->client_id, FALSE);
    }
}

/**
 * @brief starts a stream in the streamer's channel: records what it announced, lets every member present watch (they still have to accept), tells the channel and sends each member the offer
 *        the caller already checked the server-wide switch, the channel toggle and that the channel is
 *        free; this only refuses what it can see itself
 *
 * @param client_t* streamer -> the client that wants to stream
 * @param cstring source -> "screen" or "file"
 * @param cstring codec -> the webcodecs codec string the streamer will encode with
 * @param int64 width -> encoded frame width
 * @param int64 height -> encoded frame height
 * @param int64 fps -> target frame rate
 * @param cstring* out_refusal_reason -> set to a short wire reason when FALSE comes back
 *
 * @return boole TRUE when the stream started
 */
boole video_stream__start(client_t* streamer, cstring source, cstring codec, int64 width, int64 height, int64 fps, cstring* out_refusal_reason)
{
    channel_t* channel = NULL_POINTER;
    client_t* member = NULL_POINTER;
    uint64 i = 0;

    *out_refusal_reason = "";

    if (streamer == NULL_POINTER || streamer->is_existing == FALSE || streamer->is_authenticated == FALSE || streamer->is_music_bot == TRUE || streamer->is_idle == TRUE)
    {
        *out_refusal_reason = "not_allowed";
        return FALSE;
    }

    if (streamer->channel_id >= g_server_settings.max_channel_count)
    {
        *out_refusal_reason = "not_allowed";
        return FALSE;
    }

    channel = &g_channel_array[streamer->channel_id];

    if (channel->is_existing == FALSE)
    {
        *out_refusal_reason = "not_allowed";
        return FALSE;
    }

    if (g_server_settings.is_video_streaming_active == FALSE)
    {
        *out_refusal_reason = "disabled";
        return FALSE;
    }

    if (channel->is_video_stream_enabled == FALSE)
    {
        *out_refusal_reason = "channel_disabled";
        return FALSE;
    }

    // a restart by the same streamer (a new source, a reconnect) replaces its own stream; anybody
    // else has to wait, one stream per channel
    if (channel->is_video_stream_active == TRUE)
    {
        if (channel->video_streamer_client_id == streamer->client_id)
        {
            video_stream__stop(channel->channel_id, "restarted");
        }
        else
        {
            *out_refusal_reason = "busy";
            return FALSE;
        }
    }

    streamer->is_streaming_video = TRUE;
    clib__null_memory(streamer->video_stream_source, VIDEO_STREAM_SOURCE_MAX_LENGTH);
    clib__copy_memory((void*)source, streamer->video_stream_source, clib__utf8_string_length(source), VIDEO_STREAM_SOURCE_MAX_LENGTH - 1);
    clib__null_memory(streamer->video_stream_codec, VIDEO_STREAM_CODEC_MAX_LENGTH);
    clib__copy_memory((void*)codec, streamer->video_stream_codec, clib__utf8_string_length(codec), VIDEO_STREAM_CODEC_MAX_LENGTH - 1);
    streamer->video_stream_width = width;
    streamer->video_stream_height = height;
    streamer->video_stream_fps = fps;
    streamer->timestamp_last_video_keyframe_request_forwarded_ms = 0;
    streamer->is_video_viewer_allowed = FALSE;
    streamer->is_video_viewer_watching = FALSE;

    channel->is_video_stream_active = TRUE;
    channel->video_streamer_client_id = streamer->client_id;

    DBG_VIDEO_STREAM log_info("%s %llu %s %llu %s %s", "video_stream__start client", streamer->client_id, "channel", channel->channel_id, "source", source);

    // the members present now are allowed by the streamer's choice of "share with current channel";
    // each still has to accept on its side before a single frame goes its way
    server_msg__send_video_stream_state_to_clients_in_same_channel(channel->channel_id, TRUE, streamer->client_id, "started");

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        member = &g_clients_array[i];

        if (member->is_existing == FALSE || member->is_authenticated == FALSE || member->is_music_bot == TRUE || member->is_idle == TRUE)
        {
            continue;
        }

        if (member->channel_id != channel->channel_id || member->client_id == streamer->client_id)
        {
            continue;
        }

        member->is_video_viewer_allowed = TRUE;
        member->is_video_viewer_watching = FALSE;

        server_msg__send_video_stream_offer_to_single_client(member, streamer);
    }

    return TRUE;
}

/**
 * @brief ends the stream of a channel: every member forgets its consent, the streamer its stream, the channel its streamer, and the channel is told with the reason
 *        safe to call on a channel with no stream, it then does nothing
 *
 * @param uint64 channel_id -> the channel
 * @param cstring reason -> a short wire reason for the viewers' popups ("stopped", "streamer_left", "disabled", ...)
 *
 * @return void
 */
void video_stream__stop(uint64 channel_id, cstring reason)
{
    channel_t* channel = NULL_POINTER;
    client_t* member = NULL_POINTER;
    uint64 streamer_client_id = 0;
    uint64 i = 0;

    if (channel_id >= g_server_settings.max_channel_count)
    {
        return;
    }

    channel = &g_channel_array[channel_id];

    if (channel->is_video_stream_active == FALSE)
    {
        return;
    }

    streamer_client_id = channel->video_streamer_client_id;

    DBG_VIDEO_STREAM log_info("%s %llu %s %s", "video_stream__stop channel", channel_id, "reason", reason);

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        member = &g_clients_array[i];

        if (member->is_existing == FALSE || member->channel_id != channel_id)
        {
            continue;
        }

        // no viewer-state message to the streamer here: it learns everything from the state broadcast
        member->is_video_viewer_allowed = FALSE;
        member->is_video_viewer_watching = FALSE;

        if (member->client_id == streamer_client_id)
        {
            member->is_streaming_video = FALSE;
            clib__null_memory(member->video_stream_source, VIDEO_STREAM_SOURCE_MAX_LENGTH);
            clib__null_memory(member->video_stream_codec, VIDEO_STREAM_CODEC_MAX_LENGTH);
            member->video_stream_width = 0;
            member->video_stream_height = 0;
            member->video_stream_fps = 0;
        }
    }

    channel->is_video_stream_active = FALSE;
    channel->video_streamer_client_id = 0;

    server_msg__send_video_stream_state_to_clients_in_same_channel(channel_id, FALSE, streamer_client_id, reason);
}

/**
 * @brief ends every running stream, used when the admin switches video streaming off server-wide
 *
 * @param cstring reason -> the wire reason handed to every channel
 *
 * @return void
 */
void video_stream__stop_all(cstring reason)
{
    uint64 i = 0;

    for (i = 0; i < g_server_settings.max_channel_count; i++)
    {
        if (g_channel_array[i].is_existing == TRUE && g_channel_array[i].is_video_stream_active == TRUE)
        {
            video_stream__stop(i, reason);
        }
    }
}

/**
 * @brief a channel's toggle was edited: a stream running in a channel that just got switched off ends
 *
 * @param uint64 channel_id -> the edited channel
 *
 * @return void
 */
void video_stream__process_channel_toggle_changed(uint64 channel_id)
{
    if (channel_id >= g_server_settings.max_channel_count)
    {
        return;
    }

    if (g_channel_array[channel_id].is_video_stream_enabled == FALSE && g_channel_array[channel_id].is_video_stream_active == TRUE)
    {
        video_stream__stop(channel_id, "channel_disabled");
    }
}

/**
 * @brief a client leaves a channel (switch, idle, disconnect, move to root): as the streamer it ends the stream, as a viewer it drops out and the streamer's count follows
 *        call it BEFORE the client's channel_id changes, with the channel it is leaving
 *
 * @param client_t* client -> the leaving client
 * @param uint64 old_channel_id -> the channel it leaves; anything out of range (idle's -2) is ignored
 *
 * @return void
 */
void video_stream__process_client_leaving_channel(client_t* client, uint64 old_channel_id)
{
    channel_t* channel = NULL_POINTER;

    if (client == NULL_POINTER || old_channel_id >= g_server_settings.max_channel_count)
    {
        return;
    }

    channel = &g_channel_array[old_channel_id];

    if (channel->is_video_stream_active == FALSE)
    {
        client->is_video_viewer_allowed = FALSE;
        client->is_video_viewer_watching = FALSE;
        return;
    }

    if (channel->video_streamer_client_id == client->client_id)
    {
        video_stream__stop(old_channel_id, "streamer_left");
        return;
    }

    _video_stream_internal__clear_viewer(client, util__is_client_valid((int)channel->video_streamer_client_id) ? &g_clients_array[channel->video_streamer_client_id] : NULL_POINTER);
}

/**
 * @brief a client arrived in a channel (switch, login, back from idle, move to root): when a stream runs there, the streamer is asked whether the newcomer may watch
 *        call it AFTER the client's channel_id changed
 *
 * @param client_t* client -> the arriving client
 *
 * @return void
 */
void video_stream__process_client_joined_channel(client_t* client)
{
    channel_t* channel = NULL_POINTER;

    if (client == NULL_POINTER || client->is_music_bot == TRUE || client->channel_id >= g_server_settings.max_channel_count)
    {
        return;
    }

    client->is_video_viewer_allowed = FALSE;
    client->is_video_viewer_watching = FALSE;

    channel = &g_channel_array[client->channel_id];

    if (channel->is_existing == FALSE || channel->is_video_stream_active == FALSE || channel->video_streamer_client_id == client->client_id)
    {
        return;
    }

    if (util__is_client_valid((int)channel->video_streamer_client_id) == FALSE)
    {
        return;
    }

    server_msg__send_video_stream_viewer_request_to_single_client(&g_clients_array[channel->video_streamer_client_id], client);
}

/**
 * @brief a viewer pressed connect or disconnect: with the streamer's consent the watching bit flips and the streamer hears about it (it sends a keyframe for a new viewer)
 *
 * @param client_t* viewer -> the client asking
 * @param boole is_watching -> TRUE to start receiving frames, FALSE to stop
 * @param cstring* out_refusal_reason -> set to a short wire reason when FALSE comes back
 *
 * @return boole TRUE when the request was applied
 */
boole video_stream__set_viewer_watching(client_t* viewer, boole is_watching, cstring* out_refusal_reason)
{
    channel_t* channel = NULL_POINTER;

    *out_refusal_reason = "";

    if (viewer == NULL_POINTER || viewer->is_authenticated == FALSE || viewer->is_music_bot == TRUE || viewer->channel_id >= g_server_settings.max_channel_count)
    {
        *out_refusal_reason = "not_allowed";
        return FALSE;
    }

    channel = &g_channel_array[viewer->channel_id];

    if (channel->is_existing == FALSE || channel->is_video_stream_active == FALSE || channel->video_streamer_client_id == viewer->client_id)
    {
        *out_refusal_reason = "no_stream";
        return FALSE;
    }

    if (util__is_client_valid((int)channel->video_streamer_client_id) == FALSE)
    {
        *out_refusal_reason = "no_stream";
        return FALSE;
    }

    if (is_watching == TRUE && viewer->is_video_viewer_allowed == FALSE)
    {
        *out_refusal_reason = "not_allowed";
        return FALSE;
    }

    if (viewer->is_video_viewer_watching == is_watching)
    {
        return TRUE;
    }

    viewer->is_video_viewer_watching = is_watching;

    server_msg__send_video_stream_viewer_state_to_single_client(&g_clients_array[channel->video_streamer_client_id], viewer->client_id, is_watching);

    return TRUE;
}

/**
 * @brief the streamer answered "may this member watch?": yes hands the member the offer, no (also the later revoke) drops it
 *
 * @param client_t* streamer -> the client answering; it must be the streamer of its channel
 * @param uint64 viewer_client_id -> the member the answer is about
 * @param boole is_allowed -> the answer
 *
 * @return boole TRUE when the answer was applied
 */
boole video_stream__allow_viewer(client_t* streamer, uint64 viewer_client_id, boole is_allowed)
{
    channel_t* channel = NULL_POINTER;
    client_t* viewer = NULL_POINTER;

    if (streamer == NULL_POINTER || streamer->is_streaming_video == FALSE || streamer->channel_id >= g_server_settings.max_channel_count)
    {
        return FALSE;
    }

    channel = &g_channel_array[streamer->channel_id];

    if (channel->is_video_stream_active == FALSE || channel->video_streamer_client_id != streamer->client_id)
    {
        return FALSE;
    }

    if (viewer_client_id == streamer->client_id || util__is_client_valid_and_not_music_bot((int)viewer_client_id) == FALSE)
    {
        return FALSE;
    }

    viewer = &g_clients_array[viewer_client_id];

    if (viewer->channel_id != streamer->channel_id)
    {
        return FALSE;
    }

    if (is_allowed == TRUE)
    {
        viewer->is_video_viewer_allowed = TRUE;
        viewer->is_video_viewer_watching = FALSE;
        server_msg__send_video_stream_offer_to_single_client(viewer, streamer);
    }
    else
    {
        // the streamer said no (or took a yes back): the viewer's popup learns why its picture stops
        _video_stream_internal__clear_viewer(viewer, streamer);
        server_msg__send_video_stream_refused_to_single_client(viewer, "revoked");
    }

    return TRUE;
}

/**
 * @brief a watching viewer lost frames and wants a keyframe: passed on to the streamer, at most once per VIDEO_KEYFRAME_REQUEST_MIN_INTERVAL_MS however many viewers ask
 *
 * @param client_t* viewer -> the client asking
 *
 * @return void
 */
void video_stream__forward_keyframe_request(client_t* viewer)
{
    channel_t* channel = NULL_POINTER;
    client_t* streamer = NULL_POINTER;
    uint64 now_ms = 0;

    if (viewer == NULL_POINTER || viewer->is_video_viewer_watching == FALSE || viewer->channel_id >= g_server_settings.max_channel_count)
    {
        return;
    }

    channel = &g_channel_array[viewer->channel_id];

    if (channel->is_video_stream_active == FALSE || util__is_client_valid((int)channel->video_streamer_client_id) == FALSE)
    {
        return;
    }

    streamer = &g_clients_array[channel->video_streamer_client_id];
    now_ms = base__get_timestamp_ms();

    if (now_ms - streamer->timestamp_last_video_keyframe_request_forwarded_ms < VIDEO_KEYFRAME_REQUEST_MIN_INTERVAL_MS)
    {
        return;
    }

    streamer->timestamp_last_video_keyframe_request_forwarded_ms = now_ms;
    server_msg__send_video_stream_keyframe_request_to_single_client(streamer);
}

/**
 * @brief a client's webrtc transport died (peer state left connected, or its audio channel closed and took the peer with it): a streamer's stream ends, a viewer stops receiving and has to press connect again once the transport is back
 *        the consent (allowed) survives, only the connection does not
 *
 * @param uint64 client_id -> the client whose transport is gone
 *
 * @return void
 */
void video_stream__process_transport_lost(uint64 client_id)
{
    client_t* client = NULL_POINTER;
    channel_t* channel = NULL_POINTER;

    if (util__is_client_valid((int)client_id) == FALSE)
    {
        return;
    }

    client = &g_clients_array[client_id];

    if (client->channel_id >= g_server_settings.max_channel_count)
    {
        client->is_video_viewer_watching = FALSE;
        return;
    }

    channel = &g_channel_array[client->channel_id];

    if (channel->is_video_stream_active == TRUE && channel->video_streamer_client_id == client_id)
    {
        video_stream__stop(client->channel_id, "streamer_transport_lost");
        return;
    }

    if (client->is_video_viewer_watching == TRUE)
    {
        client->is_video_viewer_watching = FALSE;

        if (channel->is_video_stream_active == TRUE && util__is_client_valid((int)channel->video_streamer_client_id) == TRUE)
        {
            server_msg__send_video_stream_viewer_state_to_single_client(&g_clients_array[channel->video_streamer_client_id], client_id, FALSE);
        }
    }
}

/**
 * @brief the video data channel opened: the slot may now be relayed to
 *
 * @param int id -> data channel handle
 * @param void* ptr -> the webrtc_peer_t this data channel belongs to
 *
 * @return void
 */
void RTC_API video_stream__datachannel_on_open_callback(int id, void* ptr)
{
    webrtc_peer_t* peer = (webrtc_peer_t*)ptr;

    DBG_VIDEO_STREAM log_info("%s", "video_stream__datachannel_on_open_callback");

    clib__write_lock(&g_webrtc_muggles_rwlock_guard);

    // only the slot's current video channel may mark it connected; a stale one opening late must not
    if (peer != NULL_POINTER && peer->video_data_channel_handle == id)
    {
        peer->is_video_channel_connected = TRUE;
    }

    clib__unlock(&g_webrtc_muggles_rwlock_guard);
}

/**
 * @brief the video data channel closed while the peer itself lives on: no more relaying to (or from) this slot; a streamer's stream ends, a viewer drops to disconnected
 *        when the whole peer goes down the audio channel's close callback wipes the slot first and
 *        detaches this handle, so this then sees a NULL user pointer and does nothing
 *
 * @param int id -> data channel handle
 * @param void* ptr -> the webrtc_peer_t this data channel belongs to
 *
 * @return void
 */
void RTC_API video_stream__datachannel_on_closed_callback(int id, void* ptr)
{
    webrtc_peer_t* peer = (webrtc_peer_t*)ptr;
    uint64 client_id = 0;
    boole is_current = FALSE;

    DBG_VIDEO_STREAM log_info("%s", "video_stream__datachannel_on_closed_callback");

    if (peer == NULL_POINTER)
    {
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_webrtc_muggles_rwlock_guard);
    clib__write_lock(&g_channels_global_rwlock_guard);

    if (peer->is_existing == TRUE && peer->video_data_channel_handle == id)
    {
        is_current = TRUE;
        client_id = peer->client_id;
        peer->is_video_channel_connected = FALSE;
    }

    if (is_current == TRUE)
    {
        video_stream__process_transport_lost(client_id);
    }

    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_webrtc_muggles_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief relays one encrypted video packet from the channel's streamer to every member that is allowed, watching and has its video channel up
 *        the packet is opaque: [encrypted header + chunk piece], the server only prepends the 4 byte
 *        sender client id like the audio relay does. one copy serves every receiver. a receiver with
 *        more than VIDEO_RELAY_MAX_BUFFERED_BYTES queued is skipped, so one slow viewer never stalls
 *        the others; it notices the gap and asks for a keyframe
 *
 * @param int id -> data channel handle the packet arrived on
 * @param const char* message -> the packet
 * @param int size -> its length in bytes
 * @param void* ptr -> the webrtc_peer_t of the sending client
 *
 * @attention lock free like the audio relay, for the same reason: taking the rwlocks per packet would
 *            be too slow. every read is of a plain field that the locked paths set atomically enough
 *
 * @return void
 */
void RTC_API video_stream__datachannel_on_message_callback(int id, const char* message, int size, void* ptr)
{
    webrtc_peer_t* peer_sender = (webrtc_peer_t*)ptr;
    webrtc_peer_t* peer_receiver = NULL_POINTER;
    client_t* receiver = NULL_POINTER;
    channel_t* channel = NULL_POINTER;
    void* relayed_message = NULL_POINTER;
    uint64 sender_client_id = 0;
    uint64 channel_id = 0;
    uint64 i = 0;

    if (size <= 0 || peer_sender == NULL_POINTER || peer_sender->is_existing == FALSE)
    {
        return;
    }

    sender_client_id = peer_sender->client_id;
    channel_id = peer_sender->channel_id;

    if (sender_client_id >= g_server_settings.max_client_count || channel_id >= g_server_settings.max_channel_count)
    {
        return;
    }

    channel = &g_channel_array[channel_id];

    // the server-wide switch and the channel toggle are checked live, exactly like the audio relay
    if (g_server_settings.is_video_streaming_active == FALSE || channel->is_existing == FALSE || channel->is_video_stream_enabled == FALSE)
    {
        return;
    }

    if (channel->is_video_stream_active == FALSE || channel->video_streamer_client_id != sender_client_id || g_clients_array[sender_client_id].is_streaming_video == FALSE)
    {
        return;
    }

    relayed_message = (void*)memorymanager__allocate(size + 4, MEMALLOC_VIDEOCHANNEL_ONMESSAGE);

    if (relayed_message == NULL_POINTER)
    {
        return;
    }

    ((int*)relayed_message)[0] = (int)sender_client_id;
    clib__copy_memory((void*)message, ((unsigned char*)relayed_message + 4), size, size);

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        peer_receiver = &g_webrtc_muggles_array[i];

        if (peer_receiver->is_existing == FALSE || i == sender_client_id)
        {
            continue;
        }

        if (peer_receiver->channel_id != channel_id || peer_receiver->is_video_channel_connected == FALSE || peer_receiver->video_data_channel_handle == 0)
        {
            continue;
        }

        receiver = &g_clients_array[i];

        if (receiver->is_existing == FALSE || receiver->is_video_viewer_allowed == FALSE || receiver->is_video_viewer_watching == FALSE)
        {
            continue;
        }

        // a negative value is an error from the library, treated like a full queue
        if (rtcGetBufferedAmount(peer_receiver->video_data_channel_handle) > VIDEO_RELAY_MAX_BUFFERED_BYTES
            || rtcGetBufferedAmount(peer_receiver->video_data_channel_handle) < 0)
        {
            continue;
        }

        rtcSendMessage(peer_receiver->video_data_channel_handle, relayed_message, size + 4);
    }

    memorymanager__free((nuint)relayed_message);
}

/**
 * @brief error callback of the video data channel, logged only
 *
 * @param int id -> data channel handle
 * @param const char* error -> the error description string
 * @param void* ptr -> the webrtc_peer_t this data channel belongs to
 *
 * @return void
 */
void RTC_API video_stream__datachannel_on_error_callback(int id, const char* error, void* ptr)
{
    DBG_VIDEO_STREAM log_info("%s %s", "video_stream__datachannel_on_error_callback", error);
}
