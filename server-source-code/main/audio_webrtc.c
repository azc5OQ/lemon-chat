// This file creates the WebRTC connection between a Lemon Chat client and the server.
// It exchanges connection details through the client's encrypted chat connection, handles
// reconnects and cleanup, and sends and receives audio through its audio data channel.
// Received audio goes to audio_channel.c for delivery to the other clients in the channel.
//
// Audio and video use the same WebRTC connection. This file owns that connection's lifetime,
// while video_webrtc.c creates and handles the separate data channel used for video.
#include "definitions.h"

#include "../third-party/dave-g-json/cJSON.h" // needed by base.h
#include "base.h"
#include "metadata_crypto.h"

#include "../third-party/libdatachannel-0.24.2/include/rtc/rtc.h"
#include "../third-party/rxi-log/log.h"

#include "clib/clib_memory.h"
#include "clib/clib_string.h"

#include "memory_manager.h"
#include "msg/server/server_msg.h"

#include "core/audio_channel.h"
#include "audio_webrtc.h"
#include "core/video_channel.h"
#include "video_webrtc.h"

#include "util.h"

webrtc_peer_t* g_webrtc_peers_array = 0;

// handles of one disconnected peer's libdatachannel objects, passed to the detached teardown thread
typedef struct webrtc_teardown_arg_t
{
    int peer_connection_handle;
    int data_channel_handle;       // the audio channel
    int video_data_channel_handle; // the video channel (see video_webrtc.c), 0 when none
} webrtc_teardown_arg_t;

// declarations
void RTC_API audio_webrtc__peerconnection_on_setlocaldescription_callback(int pc, const char* sdp, const char* type, void* ptr);
void RTC_API audio_webrtc__peerconnection_on_icecandidate_callback(int pc, const char* cand, const char* mid, void* ptr);
void RTC_API audio_webrtc__peerconnection_on_statechanged_callback(int pc, rtcState state, void* ptr);
void RTC_API audio_webrtc__peerconnection_on_gatheringstatechanged_callback(int pc, rtcGatheringState state, void* ptr);
void RTC_API audio_webrtc__peerconnection_on_datachannel_callback(int pc, int dc, void* ptr);
void RTC_API audio_webrtc__datachannel_on_open_callback(int id, void* ptr);
void RTC_API audio_webrtc__datachannel_on_closed_callback(int id, void* ptr);
void RTC_API audio_webrtc__datachannel_on_message_callback(int id, const char* message, int size, void* ptr);
// declarations
static void _audio_webrtc_internal__send_public_candidate(const char* candidate, const char* mid, client_t* client);
static char* _audio_webrtc_internal__state_print(rtcState state);
static char* _audio_webrtc_internal__gathering_state_print(rtcGatheringState state);
static void* _audio_webrtc_internal__teardown_thread(void* arg_void);
static void _audio_webrtc_internal__spawn_teardown(int peer_connection_handle, int data_channel_handle, int video_data_channel_handle);

/**
 * @brief error callback
 *
 * @param int id -> data channel handle
 * @param const char* error -> the error description string
 * @param void* ptr -> the webrtc_peer_t this data channel belongs to
 *
 * @return void
 */
void RTC_API audio_webrtc__datachannel_on_error_callback(int id, const char* error, void* ptr)
{
    DBG_AUDIO_WEBRTC log_info("%s %s %s", "DataChannel from %s error: %s\n", error, "\n");
}

/**
 * @brief Queues the server's WebRTC connection details when they are ready, so
 * they can be encrypted and delivered through the client's current chat socket.
 *
 * @param int pc -> peer connection handle
 * @param const char* sdp -> the local SDP description string
 * @param const char* type -> the description type ("offer" / "answer")
 * @param void* ptr -> the webrtc_peer_t this description belongs to
 *
 * @return void
 */
void RTC_API audio_webrtc__peerconnection_on_setlocaldescription_callback(int pc, const char* sdp, const char* type, void* ptr)
{
    cJSON* json_root_object = NULL_POINTER;
    cJSON* json_message_object = NULL_POINTER;
    cJSON* json_message_value = NULL_POINTER;
    char* json_root_object_string = 0;
    cJSON* json_type = NULL_POINTER;
    cJSON* json_sdp = NULL_POINTER;
    webrtc_peer_t* peer = 0;
    boole status = FALSE;

    DBG_AUDIO_WEBRTC log_info("%s", "audio_webrtc__peerconnection_on_setlocaldescription_callback \n");

    clib__read_lock(&g_clients_global_rwlock_guard);
    clib__read_lock(&g_webrtc_peers_rwlock_guard);

    if (ptr == NULL_POINTER)
    {
        goto label_descriptionCallback_end;
    }

    peer = (webrtc_peer_t*)ptr;

    if (peer->is_existing == FALSE)
    {
        goto label_descriptionCallback_end;
    }

    // a stale peer connection (slot re-initialized since) must not send its offer to the client - it would
    // overwrite the live handshake's remote description on the client side
    if (peer->peer_connection_handle != pc)
    {
        goto label_descriptionCallback_end;
    }

    status = base__is_client_valid(peer->client_id);

    if (status == FALSE || g_clients_array[peer->client_id].p_ws_connection != peer->p_ws_connection)
    {
        goto label_descriptionCallback_end;
    }

    if (clib__is_string_equal(type, "offer") == TRUE)
    {
        DBG_AUDIO_WEBRTC log_info("%s", "we are creating offer \n");
        DBG_AUDIO_WEBRTC log_info("%s %p %s", "audio_webrtc__peerconnection_on_setlocaldescription_callback", peer, "\n");

        json_root_object = cJSON_CreateObject();
        json_message_object = cJSON_CreateObject();

        json_message_value = cJSON_CreateObject(); // this object will be parsed to string down below, and converted to json again when received on client's side

        DBG_AUDIO_WEBRTC log_info("%s", "test 2");

        json_type = cJSON_CreateString(type);
        cJSON_AddItemToObject(json_message_value, "type", json_type);
        json_sdp = cJSON_CreateString(sdp);
        cJSON_AddItemToObject(json_message_value, "sdp", json_sdp);
        json_root_object_string = cJSON_PrintUnformatted(json_message_value);

        cJSON_AddStringToObject(json_message_object, "value", json_root_object_string);
        cJSON_free(json_root_object_string);
        cJSON_Delete(json_message_value);

        cJSON_AddStringToObject(json_message_object, "type", "sdp_offer");
        cJSON_AddItemToObject(json_root_object, "message", json_message_object);

        json_root_object_string = cJSON_PrintUnformatted(json_root_object);

        server_msg__delivery__queue_text_to_client(&g_clients_array[peer->client_id], json_root_object_string);

        util__free_json_message(json_root_object, json_root_object_string);

        DBG_AUDIO_WEBRTC log_info("%s", "test 3");
    }

label_descriptionCallback_end:

    clib__unlock(&g_webrtc_peers_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief runs when the client opens the data channel; stores its handle and attaches the message and close callbacks
 *
 * @param int pc -> peer connection handle
 * @param int dc -> data channel handle
 * @param void* ptr -> the webrtc_peer_t this data channel belongs to
 *
 * @return void
 */
void RTC_API audio_webrtc__peerconnection_on_datachannel_callback(int pc, int dc, void* ptr)
{
    webrtc_peer_t* peer = (webrtc_peer_t*)ptr;
    char buffer[256];

    clib__null_memory(buffer, sizeof(buffer));

    DBG_AUDIO_WEBRTC log_info("%s %s", "void RTC_API audio_webrtc__peerconnection_on_datachannel_callback(int peer_connection_handle, int data_channel_handle, void* ptr)", "\n");

    clib__write_lock(&g_webrtc_peers_rwlock_guard);

    // only the slot's current peer connection may hand the slot a data channel; a stale one must not
    if (peer != NULL_POINTER && peer->peer_connection_handle == pc)
    {
        peer->data_channel_handle = dc;
        peer->connected = TRUE;
        rtcSetClosedCallback(dc, audio_webrtc__datachannel_on_closed_callback);
        rtcSetMessageCallback(dc, audio_webrtc__datachannel_on_message_callback);

        if (rtcGetDataChannelLabel(dc, buffer, 256) >= 0)
        {
            DBG_AUDIO_WEBRTC log_info("%s%s%s%s%s", "audio_webrtc__peerconnection_on_datachannel_callback DataChannel ", "answerer", ": Received with label \"", buffer, "\"\n");
        }
    }

    clib__unlock(&g_webrtc_peers_rwlock_guard);
}

/**
 * @brief wireguard mode: resends a host candidate with the router's public address, same port
 *
 * @param const char* candidate -> the candidate string from libdatachannel
 * @param const char* mid -> its sdpMid
 * @param client_t* client -> the client to send the copy to
 *
 * @return void
 */
static void _audio_webrtc_internal__send_public_candidate(const char* candidate, const char* mid, client_t* client)
{
    char foundation[64];
    char component[8];
    char transport[8];
    char ip[64];
    char port[16];
    char typ[8];
    char type[16];
    unsigned long long priority = 0;
    char rewritten[512];
    uint64 i = 0;

    if (g_server_settings.is_wireguard_mode_active == FALSE || g_server_settings.webrtc_public_address[0] == 0 || candidate == NULL_POINTER)
    {
        return;
    }

    if (sscanf(candidate, "%63s %7s %7s %llu %63s %15s %7s %15s", foundation, component, transport, &priority, ip, port, typ, type) != 8)
    {
        return;
    }

    if (clib__is_string_equal(type, "host") == FALSE || clib__is_string_equal(typ, "typ") == FALSE)
    {
        return;
    }

    if (clib__is_string_equal(transport, "UDP") == FALSE && clib__is_string_equal(transport, "udp") == FALSE)
    {
        return;
    }

    // ipv6 (a colon in the address) and a candidate that already names the public address are skipped
    for (i = 0; ip[i] != 0; i++)
    {
        if (ip[i] == ':')
        {
            return;
        }
    }

    if (clib__is_string_equal(ip, g_server_settings.webrtc_public_address) == TRUE)
    {
        return;
    }

    // libjuice's type preferences are host 126 and srflx 100, each shifted by 24 bits into the priority
    if (priority > ((unsigned long long)26 << 24))
    {
        priority = priority - ((unsigned long long)26 << 24);
    }

    clib__null_memory(rewritten, sizeof(rewritten));
    snprintf(rewritten, sizeof(rewritten), "%sp %s %s %llu %s %s typ srflx raddr %s rport %s", foundation, component, transport, priority, g_server_settings.webrtc_public_address, port, ip, port);

    DBG_AUDIO_WEBRTC log_info("%s %s", "public candidate:", rewritten);

    server_msg__audio__send_webrtc_sdp_offer_to_single_client(client, rewritten, mid);
}

/**
 * @brief forwards a locally-gathered ICE candidate to the client so it can add it on its side
 *
 * @param int pc -> peer connection handle
 * @param const char* cand -> the ICE candidate string
 * @param const char* mid -> the sdpMid: id of the SDP media section (m= line) this candidate belongs to
 * @param void* ptr -> the webrtc_peer_t the candidate was gathered for
 *
 * @attention libdatachannel's internal thread invokes this previously-set candidate callback
 *
 * @return void
 */
void RTC_API audio_webrtc__peerconnection_on_icecandidate_callback(int pc, const char* cand, const char* mid, void* ptr)
{
    webrtc_peer_t* peer = 0;
    boole is_client_valid = FALSE;

    DBG_AUDIO_WEBRTC log_info("%s", "audio_webrtc__peerconnection_on_icecandidate_callback \n");

    peer = (webrtc_peer_t*)ptr;

    if (peer == NULL_POINTER)
    {
        return;
    }

    clib__read_lock(&g_clients_global_rwlock_guard);

    // stale peer connections (slot re-initialized since) must not push their candidates to the client
    if (peer->is_existing == TRUE && peer->peer_connection_handle == pc)
    {
        is_client_valid = base__is_client_valid(peer->client_id);

        if (is_client_valid == FALSE)
        {
            goto _label_peerconnection_on_icecandidate_callback_end;
        }

        DBG_AUDIO_WEBRTC log_info("%s", "sending sdp offer to client \n");
        server_msg__audio__send_webrtc_sdp_offer_to_single_client(&g_clients_array[peer->client_id], cand, mid);

        // wireguard mode: the same candidate once more with the router's public address (no-op otherwise)
        _audio_webrtc_internal__send_public_candidate(cand, mid, &g_clients_array[peer->client_id]);
    }

_label_peerconnection_on_icecandidate_callback_end:

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief marks whether a client is currently sending audio on its webrtc peer
 *
 * @param uint64 client_id -> id of the client
 * @param boole is_active -> TRUE while the client is transmitting audio, FALSE otherwise
 *
 * @return void
 */
void audio_webrtc__set_is_client_sending_audio(uint64 client_id, boole is_active)
{
    g_webrtc_peers_array[client_id].is_sending_audio_right_now = is_active;
    // to add lock?
}

/**
 * @brief runs when the peer connection state changes; updates the client's audio state and broadcasts it to the channel
 *
 * @param int pc -> peer connection handle
 * @param rtcState state -> the new rtcState
 * @param void* ptr -> the webrtc_peer_t whose connection changed
 *
 * @return void
 */
void RTC_API audio_webrtc__peerconnection_on_statechanged_callback(int pc, rtcState state, void* ptr)
{
    webrtc_peer_t* peer = 0;
    client_t* client = 0;
    boole status = FALSE;

    DBG_AUDIO_WEBRTC log_info("%s", "entered this function");

    peer = (webrtc_peer_t*)ptr;

    if (peer != NULL_POINTER)
    {
        DBG_AUDIO_WEBRTC log_info("%s %lld %s %s %s", "audio_webrtc__peerconnection_on_statechanged_callback ", peer->client_id, " to client ", _audio_webrtc_internal__state_print(state), "\n");

        clib__write_lock(&g_clients_global_rwlock_guard);
        clib__write_lock(&g_webrtc_peers_rwlock_guard);

        // a state change from a peer connection that is no longer the slot's current one (the slot was
        // re-initialized since) must be ignored - a callback of a replaced connection used to flip the live client's audio_state
        // to disabled here
        if (peer->is_existing == TRUE && peer->peer_connection_handle == pc)
        {
            // connected was set on datachannel open and never cleared anywhere; the re-create gate
            // reads it, so a transport that stopped working silently must drop it here
            peer->connected = (boole)(state == RTC_CONNECTED);

            // a transport that came up clears the attempt count and any cooldown behind it
            if (state == RTC_CONNECTED)
            {
                peer->attempts_since_connected = 0;
                peer->cooldown_until_ms = 0;
            }

            status = base__is_client_valid(peer->client_id);

            if (status == TRUE)
            {
                DBG_AUDIO_WEBRTC log_info("%s %s %s", "audio_webrtc__peerconnection_on_statechanged_callback ", _audio_webrtc_internal__state_print(state), "\n");
                client = &g_clients_array[peer->client_id];

                if (client->is_audio_websocket_authenticated == FALSE
                    && g_server_settings.is_audio_over_webrtc_allowed == TRUE)
                {
                    client->audio_state = state == RTC_CONNECTED
                        ? AUDIO_STATE__PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS
                        : AUDIO_STATE__AUDIO_COMPLETELY_DISABLED;
                }
                if (state != RTC_CONNECTED)
                {
                    // the video channel uses the same transport: a streamer's stream ends, a viewer
                    // drops to disconnected (the channels lock comes after the webrtc peers lock in the lock order)
                    peer->is_video_channel_connected = FALSE;
                    clib__write_lock(&g_channels_global_rwlock_guard);
                    video_channel__process_transport_lost(peer->client_id);
                    clib__unlock(&g_channels_global_rwlock_guard);
                }

                if (peer->last_sent_audio_state != client->audio_state)
                {
                    peer->last_sent_audio_state = client->audio_state;
                    server_msg__audio__send_state_of_client_to_all_clients(client, client->audio_state);
                }
            }
        }
        clib__unlock(&g_webrtc_peers_rwlock_guard);
        clib__unlock(&g_clients_global_rwlock_guard);
    }
}

/**
 * @brief runs when the peer connection's ICE gathering state changes (currently only logged)
 *
 * @param int pc -> peer connection handle
 * @param rtcGatheringState state -> the new rtcGatheringState
 * @param void* ptr -> the webrtc_peer_t whose gathering state changed
 *
 * @return void
 */
void RTC_API audio_webrtc__peerconnection_on_gatheringstatechanged_callback(int pc, rtcGatheringState state, void* ptr)
{
    // log_info("%s %s %s", "audio_webrtc__peerconnection_on_gatheringstatechanged_callback", _audio_webrtc_internal__gathering_state_print(state), "\n");

    DBG_AUDIO_WEBRTC log_info("%s", "entered this function");

    if (ptr != NULL_POINTER)
    {
        DBG_AUDIO_WEBRTC log_info("%s %lld %s %s %s", "audio_webrtc__peerconnection_on_gatheringstatechanged_callback ", ((webrtc_peer_t*)ptr)->client_id, " to client ", _audio_webrtc_internal__gathering_state_print(state), "\n");
    }

    if (state == RTC_GATHERING_COMPLETE)
    {
    }
}

/**
 * @brief runs when the data channel opens; marks the peer connected
 *
 * @param int id -> data channel handle
 * @param void* ptr -> the webrtc_peer_t this data channel belongs to
 *
 * @return void
 */
void RTC_API audio_webrtc__datachannel_on_open_callback(int id, void* ptr)
{
    webrtc_peer_t* peer = (webrtc_peer_t*)ptr;
    char buffer[256];

    clib__null_memory(buffer, sizeof(buffer));

    DBG_AUDIO_WEBRTC log_info("%s %s", "audio_webrtc__datachannel_on_open_callback", "\n");

    clib__write_lock(&g_webrtc_peers_rwlock_guard);

    // only the slot's current data channel may mark the peer connected; a stale one opening late must not
    if (peer != NULL_POINTER && peer->data_channel_handle == id)
    {
        peer->connected = TRUE;
        if (rtcGetDataChannelLabel(peer->data_channel_handle, buffer, 256) >= 0)
        {
            DBG_AUDIO_WEBRTC log_info("%s%s%s%s%s", "DataChannel ", "offerer", ": Received with label \"", buffer, "\"\n");
        }
    }

    clib__unlock(&g_webrtc_peers_rwlock_guard);
}

/**
 * @brief runs when the data channel closes; clears the peer's slot
 *
 * @param int id -> data channel handle
 * @param void* ptr -> the webrtc_peer_t this data channel belongs to
 *
 * @attention the slot may have been re-initialized since this data channel was created (the client retries
 *            the datachannel setup every 10 seconds and each retry overwrites the slot's handles). a stale
 *            data channel closing later must not clear the slot's CURRENT state - that silently removed the
 *            client from every audio relay loop while its live datachannel stayed open. only the handle
 *            that is still the slot's current one may clear the slot.
 *
 * @return void
 */
void RTC_API audio_webrtc__datachannel_on_closed_callback(int id, void* ptr)
{
    webrtc_peer_t* peer = 0;
    client_t* client = 0;
    int peer_connection_handle = 0;
    int data_channel_handle = 0;
    int video_data_channel_handle = 0;
    uint64 client_id = 0;
    boole status = FALSE;

    DBG_AUDIO_WEBRTC log_info("%s %s", "audio_webrtc__datachannel_on_closed_callback", "\n");

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_webrtc_peers_rwlock_guard);

    peer = (webrtc_peer_t*)ptr;

    if (peer != NULL_POINTER && peer->data_channel_handle == id)
    {
        // this close can beat the go_to_idle / disconnect paths, which then find an empty slot
        // and skip their teardown - so the dying pair must be detached and deleted from here
        peer_connection_handle = peer->peer_connection_handle;
        data_channel_handle = peer->data_channel_handle;
        video_data_channel_handle = peer->video_data_channel_handle;
        client_id = peer->client_id;

        if (data_channel_handle != 0)
        {
            rtcSetUserPointer(data_channel_handle, NULL_POINTER);
        }
        if (video_data_channel_handle != 0)
        {
            rtcSetUserPointer(video_data_channel_handle, NULL_POINTER);
        }
        if (peer_connection_handle != 0)
        {
            rtcSetUserPointer(peer_connection_handle, NULL_POINTER);
        }

        clib__null_memory(peer, sizeof(webrtc_peer_t));

        // the transport is gone, say so - the statechange callback skips cleared slots, so a client
        // still believing it is connected would otherwise never learn and never re-create
        status = base__is_client_valid(client_id);

        if (status == TRUE && g_clients_array[client_id].is_audio_websocket_authenticated == FALSE
            && g_clients_array[client_id].audio_state != AUDIO_STATE__AUDIO_COMPLETELY_DISABLED)
        {
            client = &g_clients_array[client_id];
            client->audio_state = AUDIO_STATE__AUDIO_COMPLETELY_DISABLED;
            server_msg__audio__send_state_of_client_to_all_clients(client, AUDIO_STATE__AUDIO_COMPLETELY_DISABLED);
        }

        // the whole transport goes with the slot: a streamer's stream ends, a viewer drops to
        // disconnected (the channels lock comes after the webrtc peers lock in the lock order)
        if (status == TRUE)
        {
            clib__write_lock(&g_channels_global_rwlock_guard);
            video_channel__process_transport_lost(client_id);
            clib__unlock(&g_channels_global_rwlock_guard);
        }
    }

    clib__unlock(&g_webrtc_peers_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);

    // deleting inline would deadlock: rtcDelete* blocks on libdatachannel threads whose
    // callbacks take the locks used above
    _audio_webrtc_internal__spawn_teardown(peer_connection_handle, data_channel_handle, video_data_channel_handle);
}

/**
 * @brief a voice frame from a client's data channel, handed to the shared relay with the handle it arrived on,
 *        or the client's one-byte liveness probe, which goes straight back
 *
 * @param int id -> data channel handle the message arrived on
 * @param const char* message -> the received audio data
 * @param int size -> length in bytes of the received audio data
 * @param void* ptr -> the webrtc_peer_t of the sending client
 *
 * @return void
 */
void RTC_API audio_webrtc__datachannel_on_message_callback(int id, const char* message, int size, void* ptr)
{
    webrtc_peer_t* peer = ptr;
    uint64 sender_id = 0;

    if (peer == NULL_POINTER || size <= 0)
    {
        return;
    }
    // a one-byte frame is the client's liveness probe: it comes back on the same channel, so the client can
    // tell a transport that no longer works, while the browser still reports it open, from a quiet one
    if (size == 1)
    {
        base__count_webrtc_bytes(1, rtcSendMessage(id, message, 1) >= 0 ? 1 : 0);
        return;
    }
    base__count_webrtc_bytes((uint64)size, 0);
    // Peer slots never move. Derive the index without reading fields another callback can reset.
    sender_id = (uint64)(peer - g_webrtc_peers_array);
    audio_channel__relay_voice(sender_id, NULL_POINTER, id, (const unsigned char*)message, (uint64)size);
}

/**
 * @brief returns a human-readable name for an rtcState value
 *
 * @param rtcState state -> the rtcState to name
 *
 * @return char* static string naming the state, or 0 if unknown
 */
static char* _audio_webrtc_internal__state_print(rtcState state)
{
    char* str = NULL_POINTER;
    switch (state)
    {
    case RTC_NEW:
        str = "RTC_NEW";
        break;
    case RTC_CONNECTING:
        str = "RTC_CONNECTING";
        break;
    case RTC_CONNECTED:
        str = "RTC_CONNECTED";
        break;
    case RTC_DISCONNECTED:
        str = "RTC_DISCONNECTED";
        break;
    case RTC_FAILED:
        str = "RTC_FAILED";
        break;
    case RTC_CLOSED:
        str = "RTC_CLOSED";
        break;
    default:
        break;
    }

    return str;
}

/**
 * @brief returns a human-readable name for an rtcGatheringState value
 *
 * @param rtcGatheringState state -> the rtcGatheringState to name
 *
 * @return char* static string naming the state, or 0 if unknown
 */
static char* _audio_webrtc_internal__gathering_state_print(rtcGatheringState state)
{
    char* str = NULL_POINTER;
    switch (state)
    {
    case RTC_GATHERING_NEW:
        str = "RTC_GATHERING_NEW";
        break;
    case RTC_GATHERING_INPROGRESS:
        str = "RTC_GATHERING_INPROGRESS";
        break;
    case RTC_GATHERING_COMPLETE:
        str = "RTC_GATHERING_COMPLETE";
        break;
    default:
        break;
    }

    return str;
}

/**
 * @brief updates the client's webrtc peer with its new channel id when the client joins or switches a channel
 *
 * @attention takes the peers write lock
 *
 * @param client_t* client -> the client that changed channel
 *
 * @return void
 */
void audio_webrtc__process_client_channel_join(client_t* client)
{
    webrtc_peer_t* peer = NULL_POINTER;

    clib__write_lock(&g_webrtc_peers_rwlock_guard);

    peer = &g_webrtc_peers_array[client->client_id];

    if (peer->is_existing == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s", "audio_webrtc__process_client_channel_join peer not is_existing \n");
        goto label_audio_webrtc__process_client_channel_join_end;
    }

    peer->channel_id = client->channel_id;

label_audio_webrtc__process_client_channel_join_end:

    clib__unlock(&g_webrtc_peers_rwlock_guard);

    return;
}

/**
 * @brief Deletes a disconnected peer's audio channel, video channel, and WebRTC connection.
 *
 * @param void* arg_void -> allocated webrtc_teardown_arg_t containing the handles; freed by this thread
 *
 * @attention Runs detached without application locks. Deletion waits for library callbacks, which may need those locks.
 *
 * @return void* -> always NULL_POINTER.
 */
static void* _audio_webrtc_internal__teardown_thread(void* arg_void)
{
    webrtc_teardown_arg_t* arg = NULL_POINTER;

    arg = (webrtc_teardown_arg_t*)arg_void;

    if (arg->data_channel_handle != 0)
    {
        rtcDeleteDataChannel(arg->data_channel_handle);
    }

    if (arg->video_data_channel_handle != 0)
    {
        rtcDeleteDataChannel(arg->video_data_channel_handle);
    }

    if (arg->peer_connection_handle != 0)
    {
        rtcDeletePeerConnection(arg->peer_connection_handle);
    }

    memorymanager__free((nuint)arg);

    return NULL_POINTER;
}

/**
 * @brief Starts a detached thread to delete the old WebRTC connection and its channels.
 *
 * @param int peer_connection_handle -> connection to delete, or zero when absent
 * @param int data_channel_handle -> audio channel to delete, or zero when absent
 * @param int video_data_channel_handle -> video channel to delete, or zero when absent
 *
 * @attention Remove the handles from the peer slot and clear their library user pointers before calling. The deletion thread holds no application locks.
 *
 * @return void
 */
static void _audio_webrtc_internal__spawn_teardown(int peer_connection_handle, int data_channel_handle, int video_data_channel_handle)
{
    webrtc_teardown_arg_t* teardown_arg = NULL_POINTER;
    pthread_t teardown_thread = 0;

    if (peer_connection_handle == 0 && data_channel_handle == 0 && video_data_channel_handle == 0)
    {
        return;
    }

    teardown_arg = (webrtc_teardown_arg_t*)memorymanager__allocate(sizeof(webrtc_teardown_arg_t), MEMALLOC_WEBRTC_PEERS);

    if (teardown_arg == NULL_POINTER)
    {
        return;
    }

    teardown_arg->peer_connection_handle = peer_connection_handle;
    teardown_arg->data_channel_handle = data_channel_handle;
    teardown_arg->video_data_channel_handle = video_data_channel_handle;

    if (pthread_create(&teardown_thread, 0, _audio_webrtc_internal__teardown_thread, (void*)teardown_arg) == 0)
    {
        pthread_detach(teardown_thread);
    }
    else
    {
        memorymanager__free((nuint)teardown_arg);
    }
}

/**
 * @brief tears down a client's webrtc peer when the client disconnects
 *
 * @param client_t* client -> the client that disconnected
 *
 * @return void
 */
void audio_webrtc__process_client_disconnect(client_t* client)
{
    webrtc_peer_t* peer = NULL_POINTER;
    int peer_connection_handle = 0;
    int data_channel_handle = 0;
    int video_data_channel_handle = 0;

    DBG_AUDIO_WEBRTC log_info("%s", "audio_webrtc__process_client_disconnect \n");

    clib__write_lock(&g_webrtc_peers_rwlock_guard);

    peer = &g_webrtc_peers_array[client->client_id];

    if (peer->is_existing == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s", "audio_webrtc__process_client_disconnect peer not is_existing \n");
        clib__unlock(&g_webrtc_peers_rwlock_guard);
        return;
    }

    // snapshot the libdatachannel handles, then clear the slot so the lock-free relay callback stops referencing this peer the moment we unlock
    peer_connection_handle = peer->peer_connection_handle;
    data_channel_handle = peer->data_channel_handle;
    video_data_channel_handle = peer->video_data_channel_handle;

    // detach the dying objects from the slot (every callback null-checks the user pointer): the slot index
    // is reused for the next connecting client, and a late callback from these objects must not touch the
    // new occupant's state
    if (data_channel_handle != 0)
    {
        rtcSetUserPointer(data_channel_handle, NULL_POINTER);
    }
    if (video_data_channel_handle != 0)
    {
        rtcSetUserPointer(video_data_channel_handle, NULL_POINTER);
    }
    if (peer_connection_handle != 0)
    {
        rtcSetUserPointer(peer_connection_handle, NULL_POINTER);
    }

    clib__null_memory(peer, sizeof(webrtc_peer_t));

    DBG_AUDIO_WEBRTC log_info("%s %lld %s", "audio_webrtc__process_client_disconnect peer ", client->client_id, "disconnected \n");

    clib__unlock(&g_webrtc_peers_rwlock_guard);

    // delete the libdatachannel objects on a detached thread that holds no locks. rtcDeletePeerConnection
    // blocks until libdatachannel's threads finish, and those callbacks need locks this disconnect path's
    // callers hold, so deleting inline deadlocked (the original code commented these out and leaked)
    _audio_webrtc_internal__spawn_teardown(peer_connection_handle, data_channel_handle, video_data_channel_handle);
}

/**
 * @brief Adds a client's ICE candidate to its WebRTC connection.
 * @param client_t* client -> client that owns the connection
 * @param cstring candidate -> checked candidate from the client's message
 */
void audio_webrtc__add_remote_candidate(client_t* client, cstring candidate)
{
    webrtc_peer_t* peer = NULL_POINTER;

    clib__read_lock(&g_webrtc_peers_rwlock_guard);
    peer = &g_webrtc_peers_array[client->client_id];
    if (peer->is_existing == TRUE)
    {
        rtcAddRemoteCandidate(peer->peer_connection_handle, candidate, 0);
    }
    clib__unlock(&g_webrtc_peers_rwlock_guard);
}

/**
 * @brief Applies the client's answer to the server's WebRTC connection offer.
 * @param client_t* client -> client that owns the connection
 * @param cstring sdp -> checked session description from the client's message
 * @param cstring type -> checked description type from the client's message
 */
void audio_webrtc__set_remote_description(client_t* client, cstring sdp, cstring type)
{
    webrtc_peer_t* peer = NULL_POINTER;

    clib__write_lock(&g_webrtc_peers_rwlock_guard);
    peer = &g_webrtc_peers_array[client->client_id];
    if (peer->is_existing == TRUE)
    {
        rtcSetRemoteDescription(peer->peer_connection_handle, sdp, type);
    }
    clib__unlock(&g_webrtc_peers_rwlock_guard);
}

/**
 * @brief this function tries to initialize structure that represents webrtc datachannel connection that server creates with client.
 *
 *        on a re-init (the client retries the setup every 10 seconds until it connects) the previous peer
 *        connection and data channel are detached from the slot and deleted on a detached teardown thread,
 *        so their callbacks, which still point at this slot, cannot clear the slot's current state.
 *
 * @param client_t* client -> the client to set up the webrtc peer connection for
 *
 * @attention this function assumes that client pointer is used within readlock or write lock, and its safe to read it.
 *
 * @note like websocket, webrtc also works between client and server in this chat application, its not peer to peer. technically it is, but the peer is server.
 * @note takes the write lock of the webrtc peer array (g_webrtc_peers_rwlock_guard) while the slot is rebuilt, and releases it before the teardown thread is spawned
 *
 * @return boole -> TRUE when the peer connection and the data channel were created, FALSE when the client is missing, is not existing / not authenticated, or the peer connection could not be created
 */
boole audio_webrtc__initialize_datachannel_connection(client_t* client)
{
    webrtc_peer_t* peer = 0;
    boole result = FALSE;
    rtcConfiguration config;
    rtcDataChannelInit init;
    int old_peer_connection_handle = 0;
    int old_data_channel_handle = 0;
    int old_video_data_channel_handle = 0;
    int failed_peer_connection_handle = 0;
    const char* iceServers[1] = { "127.0.0.1:3478" }; // using our own stun server! (violet)

    DBG_AUDIO_WEBRTC log_info("%s", "audio_webrtc__initialize_datachannel_connection \n");

    // these two run before the webrtc peers lock is taken, so they must return directly - the shared
    // exit label unlocks, and unlocking a lock that was never taken is undefined behavior
    if (client == NULL_POINTER)
    {
        DBG_AUDIO_WEBRTC log_info("%s", "client == NULL_POINTER");
        return FALSE;
    }

    if (client->is_existing == FALSE || client->is_authenticated == FALSE)
    {
        DBG_AUDIO_WEBRTC log_info("%s", "client->is_existing == FALSE || client->is_authenticated == FALSE");
        return FALSE;
    }

    // rtcInitLogger(RTC_LOG_VERBOSE, NULL); If something doesn't work, uncomment!

    clib__null_memory(&config, sizeof(rtcConfiguration));

    // const char* iceServers[1] = { "stun:stun.l.google.com:19302" };
    config.iceServers = iceServers;
    config.iceServersCount = 1;

    // wireguard mode: the datachannel sockets stay inside the udp range the router forwards to this
    // machine (the matching public candidate is built in _audio_webrtc_internal__send_public_candidate)
    if (g_server_settings.is_wireguard_mode_active == TRUE)
    {
        config.portRangeBegin = (uint16_t)g_server_settings.webrtc_port_range_begin;
        config.portRangeEnd = (uint16_t)g_server_settings.webrtc_port_range_end;
    }

    clib__write_lock(&g_webrtc_peers_rwlock_guard);

    peer = &g_webrtc_peers_array[client->client_id];

    // a re-init (the client retries the datachannel setup every 10 seconds until it connects) must not
    // leak the previous libdatachannel objects: their callbacks stayed registered with this slot's address
    // as user pointer, and when such a replaced object later failed or closed, its callbacks cleared the slot's
    // CURRENT state - silencing the client in every relay loop forever. detach the replaced objects from the slot
    // (NULL user pointer; every callback null-checks it) and delete them on the detached teardown thread
    // below, exactly like the disconnect path does
    old_peer_connection_handle = peer->peer_connection_handle;
    old_data_channel_handle = peer->data_channel_handle;
    old_video_data_channel_handle = peer->video_data_channel_handle;

    if (old_data_channel_handle != 0)
    {
        rtcSetUserPointer(old_data_channel_handle, NULL_POINTER);
    }
    if (old_video_data_channel_handle != 0)
    {
        rtcSetUserPointer(old_video_data_channel_handle, NULL_POINTER);
    }
    if (old_peer_connection_handle != 0)
    {
        rtcSetUserPointer(old_peer_connection_handle, NULL_POINTER);
    }

    peer->is_existing = TRUE;
    peer->last_sent_audio_state = AUDIO_STATE__AUDIO_COMPLETELY_DISABLED;
    peer->peer_connection_handle = rtcCreatePeerConnection(&config);
    peer->p_ws_connection = client->p_ws_connection;
    peer->client_id = client->client_id;
    // keep the client's actual channel: a re-init used to hard-reset this to 0, so a client that had
    // already joined a channel was left with a peer still in the root channel and every relayed frame
    // was skipped on the channel-mismatch check
    peer->channel_id = client->channel_id;
    peer->is_sending_audio_right_now = FALSE;
    peer->data_channel_handle = 0;
    peer->video_data_channel_handle = 0;
    peer->is_video_channel_connected = FALSE;
    peer->last_sent_audio_state = 0;

    clib__null_memory(peer->dh_shared_secret, SHARED_SECRET_LENGTH);
    clib__copy_memory(client->dh_shared_secret, peer->dh_shared_secret, clib__utf8_string_length(client->dh_shared_secret), SHARED_SECRET_LENGTH);

    // the c api returns a NEGATIVE error code on failure, never 0 - the old == 0 check let a
    // failed create through as a "live" slot that could never send an offer
    if (peer->peer_connection_handle <= 0)
    {
        log_info("%s %d %s", "rtcCreatePeerConnection failed with", peer->peer_connection_handle, "\n");
        peer->peer_connection_handle = 0;
        peer->is_existing = FALSE;
        goto label_audio_webrtc__initialize_datachannel_connection_end;
    }

    rtcSetUserPointer(peer->peer_connection_handle, peer); // binds created peer connection to my own custom struct. Nice that this library has this
    rtcSetLocalDescriptionCallback(peer->peer_connection_handle, audio_webrtc__peerconnection_on_setlocaldescription_callback);
    rtcSetLocalCandidateCallback(peer->peer_connection_handle, audio_webrtc__peerconnection_on_icecandidate_callback); // gets called when ICE candidate is returned from STUN server
    rtcSetStateChangeCallback(peer->peer_connection_handle, audio_webrtc__peerconnection_on_statechanged_callback); // gets called when peer connection state is changed
    rtcSetGatheringStateChangeCallback(peer->peer_connection_handle, audio_webrtc__peerconnection_on_gatheringstatechanged_callback);
    rtcSetDataChannelCallback(peer->peer_connection_handle, audio_webrtc__peerconnection_on_datachannel_callback);

    DBG_AUDIO_WEBRTC log_info("%s", "audio_webrtc__initialize_datachannel_connection got here \n");

    // clear out structure or initialization fails only unreliable needs to be set

    clib__null_memory(&init, sizeof(rtcDataChannelInit));
    init.reliability.unreliable = TRUE;
    init.reliability.unordered = TRUE;
    init.reliability.maxRetransmits = 0;

    // name of datachannel is testQQQ

    DBG_AUDIO_WEBRTC log_info("%s %d %s", "peer->data_channel_handle", peer->data_channel_handle, "\n");
    DBG_AUDIO_WEBRTC log_info("%s %d %s", "audio_webrtc__initialize_datachannel_connection peer->data_channel_handle", peer->data_channel_handle, "\n");

    peer->data_channel_handle = rtcCreateDataChannelEx(peer->peer_connection_handle, "testQQQ", &init);

    // negative means creation failed - roll the slot back to empty so the client's next
    // retry re-inits, and hand the half-built peer connection to the teardown thread
    if (peer->data_channel_handle <= 0)
    {
        log_info("%s %d %s", "rtcCreateDataChannelEx failed with", peer->data_channel_handle, "\n");
        failed_peer_connection_handle = peer->peer_connection_handle;
        rtcSetUserPointer(failed_peer_connection_handle, NULL_POINTER);
        peer->peer_connection_handle = 0;
        peer->data_channel_handle = 0;
        peer->is_existing = FALSE;
        goto label_audio_webrtc__initialize_datachannel_connection_end;
    }

    result = TRUE;

    rtcSetOpenCallback(peer->data_channel_handle, audio_webrtc__datachannel_on_open_callback);
    rtcSetClosedCallback(peer->data_channel_handle, audio_webrtc__datachannel_on_closed_callback);
    rtcSetMessageCallback(peer->data_channel_handle, audio_webrtc__datachannel_on_message_callback);
    rtcSetErrorCallback(peer->data_channel_handle, audio_webrtc__datachannel_on_error_callback);

    // Video has its own data channel, but uses the connection created above.
    // If video setup fails, the client's audio channel can still be used.
    video_webrtc__create_data_channel(peer);

label_audio_webrtc__initialize_datachannel_connection_end:
    clib__unlock(&g_webrtc_peers_rwlock_guard);

    // delete the detached objects on the teardown thread; deleting inline would deadlock (rtcDelete*
    // blocks on libdatachannel threads whose callbacks take the locks our callers hold)
    _audio_webrtc_internal__spawn_teardown(old_peer_connection_handle, old_data_channel_handle, old_video_data_channel_handle);
    _audio_webrtc_internal__spawn_teardown(failed_peer_connection_handle, 0, 0);

    DBG_AUDIO_WEBRTC log_info("%s", "audio_webrtc__initialize_datachannel_connection end \n");

    return result;
}

/**
 * @brief Checks whether the client's WebRTC connection has reached the connected state.
 *
 * @param uint64 client_id -> valid index of the client's WebRTC peer
 *
 * @attention The caller holds the WebRTC peers read lock.
 *
 * @return boole -> TRUE when the peer exists and is connected; otherwise FALSE.
 */
boole audio_webrtc__is_connected(uint64 client_id)
{
    webrtc_peer_t* peer = &g_webrtc_peers_array[client_id];

    return peer->is_existing == TRUE && peer->connected == TRUE;
}

/**
 * @brief Checks whether a packet came from the client's current connected voice channel.
 *
 * @param uint64 client_id -> valid index of the client's WebRTC peer
 * @param int data_channel -> channel handle the packet arrived on
 *
 * @attention The caller holds the WebRTC peers read lock. Packets from a replaced channel are ignored.
 *
 * @return boole -> TRUE when the peer is connected and the handle matches; otherwise FALSE.
 */
boole audio_webrtc__is_data_channel_current(uint64 client_id, int data_channel)
{
    webrtc_peer_t* peer = &g_webrtc_peers_array[client_id];

    return peer->is_existing == TRUE && peer->connected == TRUE && peer->data_channel_handle == data_channel;
}

/**
 * @brief sends one relayed audio packet over the client's data channel
 *
 * @param uint64 client_id -> slot of the receiving client
 * @param const unsigned char* packet -> the framed packet
 * @param uint64 size -> its length in bytes
 *
 * @attention peers read lock required
 *
 * @return boole -> TRUE when the packet was handed to libdatachannel
 */
boole audio_webrtc__send(uint64 client_id, const unsigned char* packet, uint64 size)
{
    webrtc_peer_t* peer = &g_webrtc_peers_array[client_id];

    if (peer->is_existing == FALSE || peer->connected == FALSE || peer->data_channel_handle == 0)
    {
        return FALSE;
    }
    if (rtcSendMessage(peer->data_channel_handle, (const char*)packet, (int)size) < 0)
    {
        return FALSE;
    }
    base__count_webrtc_bytes(0, size);
    return TRUE;
}
