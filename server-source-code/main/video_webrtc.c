// This file lets Lemon Chat clients send and receive video through WebRTC.
// It creates the video data channel, handles arriving packets and connection changes,
// and sends packets to viewers whose connection can accept more data.
//
// audio_webrtc.c creates and closes the WebRTC connection shared by audio and video.
// This file handles only its video data channel. video_channel.c decides who may stream
// or watch and forwards the packets through each viewer's chosen connection.
#include "definitions.h"
#include "../third-party/dave-g-json/cJSON.h"
#include "base.h"
#include "../third-party/libdatachannel-0.24.2/include/rtc/rtc.h"
#include "../third-party/rxi-log/log.h"
#include "clib/clib_memory.h"
#include "audio_webrtc.h"
#include "video_webrtc.h"
#include "core/video_channel.h"
#include "util.h"

// declarations
static void RTC_API _video_webrtc_internal__datachannel_on_open_callback(int id, void* ptr);
static void RTC_API _video_webrtc_internal__datachannel_on_closed_callback(int id, void* ptr);
static void RTC_API _video_webrtc_internal__datachannel_on_message_callback(int id, const char* message, int size, void* ptr);
static void RTC_API _video_webrtc_internal__datachannel_on_error_callback(int id, const char* error, void* ptr);

/**
 * @brief the client's video data channel opened, so the server can send video to it
 *
 * @param int id -> data channel handle
 * @param void* ptr -> the webrtc_peer_t this data channel belongs to
 *
 * @return void
 */
static void RTC_API _video_webrtc_internal__datachannel_on_open_callback(int id, void* ptr)
{
    webrtc_peer_t* peer = (webrtc_peer_t*)ptr;

    DBG_VIDEO_WEBRTC log_info("%s", "_video_webrtc_internal__datachannel_on_open_callback");

    clib__write_lock(&g_webrtc_peers_rwlock_guard);

    // A callback from a channel replaced during reconnect must not change the new channel's state.
    if (peer != NULL_POINTER && peer->video_data_channel_handle == id)
    {
        peer->is_video_channel_connected = TRUE;
    }

    clib__unlock(&g_webrtc_peers_rwlock_guard);
}

/**
 * @brief the client's video data channel closed, so stop using it to send or receive video
 *        video_channel.c decides whether to stop streaming or watching; a working video WebSocket
 *        can keep either one going. When audio_webrtc.c closes the whole connection, it removes the
 *        pointer to the client before deleting the channels, so later callbacks do nothing.
 *
 * @param int id -> data channel handle
 * @param void* ptr -> the webrtc_peer_t this data channel belongs to
 *
 * @return void
 */
static void RTC_API _video_webrtc_internal__datachannel_on_closed_callback(int id, void* ptr)
{
    webrtc_peer_t* peer = (webrtc_peer_t*)ptr;
    uint64 client_id = 0;
    boole is_current = FALSE;

    DBG_VIDEO_WEBRTC log_info("%s", "_video_webrtc_internal__datachannel_on_closed_callback");

    if (peer == NULL_POINTER)
    {
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_webrtc_peers_rwlock_guard);
    clib__write_lock(&g_channels_global_rwlock_guard);

    if (peer->is_existing == TRUE && peer->video_data_channel_handle == id)
    {
        is_current = TRUE;
        client_id = peer->client_id;
        peer->is_video_channel_connected = FALSE;
    }

    if (is_current == TRUE)
    {
        video_channel__process_transport_lost(client_id);
    }

    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_webrtc_peers_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief pass a video packet to video_channel.c, which checks the sender and forwards it to viewers
 *
 * @param int id -> data channel handle the packet arrived on
 * @param const char* message -> the packet
 * @param int size -> its length in bytes
 * @param void* ptr -> the webrtc_peer_t of the sending client
 *
 * @return void
 */
static void RTC_API _video_webrtc_internal__datachannel_on_message_callback(int id, const char* message, int size, void* ptr)
{
    webrtc_peer_t* peer_sender = (webrtc_peer_t*)ptr;

    if (size <= 0 || peer_sender == NULL_POINTER)
    {
        return;
    }
    // peer slots never move: the index comes from the pointer, not from fields another callback can reset
    base__count_webrtc_bytes((uint64)size, 0);
    video_channel__relay_packet((uint64)(peer_sender - g_webrtc_peers_array), NULL_POINTER, id, (const unsigned char*)message, (uint64)size);
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
static void RTC_API _video_webrtc_internal__datachannel_on_error_callback(int id, const char* error, void* ptr)
{
    DBG_VIDEO_WEBRTC log_info("%s %s", "_video_webrtc_internal__datachannel_on_error_callback", error);
}

/**
 * @brief Creates an ordered video channel that stops retrying packets after 250 milliseconds.
 *
 * @param webrtc_peer_t* peer -> peer that receives the new video handle and callbacks
 *
 * @attention The caller holds the WebRTC peers write lock. Failure leaves the video handle empty; the audio channel remains usable.
 *
 * @return void
 */
void video_webrtc__create_data_channel(webrtc_peer_t* peer)
{
    rtcDataChannelInit video_init;

    clib__null_memory(&video_init, sizeof(rtcDataChannelInit));
    video_init.reliability.unreliable = TRUE;
    video_init.reliability.unordered = FALSE;
    video_init.reliability.maxPacketLifeTime = 250;

    peer->video_data_channel_handle = rtcCreateDataChannelEx(peer->peer_connection_handle, VIDEO_DATACHANNEL_LABEL, &video_init);
    peer->is_video_channel_connected = FALSE;

    if (peer->video_data_channel_handle <= 0)
    {
        log_info("%s %d %s", "rtcCreateDataChannelEx for the video channel failed with", peer->video_data_channel_handle, "\n");
        peer->video_data_channel_handle = 0;
    }
    else
    {
        rtcSetOpenCallback(peer->video_data_channel_handle, _video_webrtc_internal__datachannel_on_open_callback);
        rtcSetClosedCallback(peer->video_data_channel_handle, _video_webrtc_internal__datachannel_on_closed_callback);
        rtcSetMessageCallback(peer->video_data_channel_handle, _video_webrtc_internal__datachannel_on_message_callback);
        rtcSetErrorCallback(peer->video_data_channel_handle, _video_webrtc_internal__datachannel_on_error_callback);
    }
}

/**
 * @brief Checks whether the client has an open video data channel.
 *
 * @param uint64 client_id -> valid index of the client's WebRTC peer
 *
 * @attention The caller holds the WebRTC peers read lock.
 *
 * @return boole -> TRUE when the peer exists and its video channel is open; otherwise FALSE.
 */
boole video_webrtc__is_connected(uint64 client_id)
{
    webrtc_peer_t* peer = &g_webrtc_peers_array[client_id];

    return peer->is_existing == TRUE && peer->is_video_channel_connected == TRUE;
}

/**
 * @brief Checks whether a packet came from the client's current open video channel.
 *
 * @param uint64 client_id -> valid index of the client's WebRTC peer
 * @param int data_channel -> channel handle the packet arrived on
 *
 * @attention The caller holds the WebRTC peers read lock. Channels replaced during reconnect must not send packets.
 *
 * @return boole -> TRUE when the handle matches the open channel; otherwise FALSE.
 */
boole video_webrtc__is_data_channel_current(uint64 client_id, int data_channel)
{
    webrtc_peer_t* peer = &g_webrtc_peers_array[client_id];

    return video_webrtc__is_connected(client_id) == TRUE && peer->video_data_channel_handle == data_channel;
}

/**
 * @brief Checks whether the viewer's open video channel has room for another packet.
 *
 * @param uint64 client_id -> valid index of the receiving client's WebRTC peer
 *
 * @attention The caller holds the WebRTC peers read lock.
 *
 * @return boole -> TRUE when the channel is open and its queued data is within the limit; otherwise FALSE.
 */
boole video_webrtc__can_send(uint64 client_id)
{
    webrtc_peer_t* peer = &g_webrtc_peers_array[client_id];
    int buffered = 0;

    if (video_webrtc__is_connected(client_id) == FALSE || peer->video_data_channel_handle == 0)
    {
        return FALSE;
    }
    buffered = rtcGetBufferedAmount(peer->video_data_channel_handle);
    return buffered >= 0 && buffered <= VIDEO_RELAY_MAX_BUFFERED_BYTES;
}

/**
 * @brief Sends one encrypted video packet through the viewer's data channel.
 *
 * @param uint64 client_id -> valid index of the receiving client's WebRTC peer
 * @param const unsigned char* packet -> encrypted packet; the caller keeps this buffer
 * @param uint64 size -> packet length in bytes
 *
 * @attention The caller holds the WebRTC peers read lock and checks video_webrtc__can_send before preparing the packet.
 *
 * @return boole -> TRUE when libdatachannel accepted the packet; FALSE if disconnected or sending fails.
 */
boole video_webrtc__send(uint64 client_id, const unsigned char* packet, uint64 size)
{
    webrtc_peer_t* peer = &g_webrtc_peers_array[client_id];

    if (video_webrtc__is_connected(client_id) == FALSE || peer->video_data_channel_handle == 0)
    {
        return FALSE;
    }
    if (rtcSendMessage(peer->video_data_channel_handle, (const char*)packet, (int)size) < 0)
    {
        return FALSE;
    }
    base__count_webrtc_bytes(0, size);
    return TRUE;
}
