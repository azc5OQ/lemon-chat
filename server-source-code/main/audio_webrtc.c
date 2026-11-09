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
#include "crypto/crypto_metadata.h"

#include "../third-party/libdatachannel-0.24.2/include/rtc/rtc.h"
#include "../third-party/rxi-log/log.h"

#include "clib/clib.h"

#include "memory_manager.h"
#include "msg/server/server_msg.h"

#include "core/audio_channel.h"
#include "audio_webrtc.h"
#include "core/video_channel.h"
#include "video_webrtc.h"

#include "util.h"

webrtc_peer_t *g_webrtc_peers_array = 0;

// Reserved before creating a peer, then transferred to the persistent cleanup worker.
typedef struct webrtc_teardown_arg_t
{
    struct webrtc_teardown_arg_t *next;
    int peer_connection_handle;
    int data_channel_handle; // the audio channel
    int video_data_channel_handle; // the video channel (see video_webrtc.c), 0 when none
} webrtc_teardown_arg_t;

static pthread_mutex_t g_webrtc_teardown_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_webrtc_teardown_changed = PTHREAD_COND_INITIALIZER;
static webrtc_teardown_arg_t *g_webrtc_teardown_head = NULL_POINTER;
static webrtc_teardown_arg_t *g_webrtc_teardown_tail = NULL_POINTER;
static boole g_webrtc_teardown_started = FALSE;

// declarations
void RTC_API audio_webrtc__peerconnection_on_setlocaldescription_callback(int pc, const char *sdp, const char *type, void *ptr);
void RTC_API audio_webrtc__peerconnection_on_icecandidate_callback(int pc, const char *cand, const char *mid, void *ptr);
void RTC_API audio_webrtc__peerconnection_on_statechanged_callback(int pc, rtcState state, void *ptr);
void RTC_API audio_webrtc__peerconnection_on_gatheringstatechanged_callback(int pc, rtcGatheringState state, void *ptr);
void RTC_API audio_webrtc__peerconnection_on_datachannel_callback(int pc, int dc, void *ptr);
void RTC_API audio_webrtc__datachannel_on_open_callback(int id, void *ptr);
void RTC_API audio_webrtc__datachannel_on_closed_callback(int id, void *ptr);
void RTC_API audio_webrtc__datachannel_on_message_callback(int id, const char *message, int size, void *ptr);

// declarations
static void _audio_webrtc_internal__send_public_candidate(const char *candidate, const char *mid, client_t *client);
static char *_audio_webrtc_internal__state_print(rtcState state);
static char *_audio_webrtc_internal__gathering_state_print(rtcGatheringState state);
static void *_audio_webrtc_internal__teardown_thread(void *arg_void);
static boole _audio_webrtc_internal__start_teardown_worker(void);
static webrtc_teardown_arg_t *_audio_webrtc_internal__detach_peer(webrtc_peer_t *peer);
static void _audio_webrtc_internal__queue_teardown(webrtc_teardown_arg_t *cleanup);

/**
 * @brief error callback
 *
 * @param int id -> data channel handle
 * @param const char* error -> the error description string
 * @param void* ptr -> the webrtc_peer_t this data channel belongs to
 *
 * @return void
 */
void RTC_API audio_webrtc__datachannel_on_error_callback(int id, const char *error, void *ptr)
{
    DBG_AUDIO_WEBRTC log_info("%s %s %s", "DataChannel from %s error: %s\n", error, "\n");
}

/**
 * @brief Queues the server's WebRTC connection details when they are ready, so they can be
 *        encrypted and delivered through the client's current chat socket.
 *
 * @param int pc -> peer connection handle
 * @param const char* sdp -> the local SDP description string
 * @param const char* type -> the description type ("offer" / "answer")
 * @param void* ptr -> the webrtc_peer_t this description belongs to
 *
 * @return void
 */
void RTC_API audio_webrtc__peerconnection_on_setlocaldescription_callback(int pc, const char *sdp, const char *type, void *ptr)
{
    cJSON *json_root_object = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_message_value = NULL_POINTER;
    char *json_root_object_string = 0;
    webrtc_peer_t *peer = 0;
    boole status = FALSE;

    DBG_AUDIO_WEBRTC log_info("%s", "audio_webrtc__peerconnection_on_setlocaldescription_callback \n");

    clib__read_lock(&g_clients_global_rwlock_guard);
    clib__read_lock(&g_webrtc_peers_rwlock_guard);

    if (ptr == NULL_POINTER)
    {
        goto label_descriptionCallback_end;
    }

    peer = (webrtc_peer_t *)ptr;

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

    if (status == FALSE)
    {
        goto label_descriptionCallback_end;
    }
    status = g_clients_array[peer->client_id].p_ws_connection != peer->p_ws_connection;
    if (status == TRUE)
    {
        goto label_descriptionCallback_end;
    }

    if (clib__is_string_equal(type, "offer") == TRUE)
    {
        DBG_AUDIO_WEBRTC log_info("%s", "we are creating offer \n");
        DBG_AUDIO_WEBRTC log_info("%s %p %s", "audio_webrtc__peerconnection_on_setlocaldescription_callback", peer, "\n");

        json_root_object = server_msg__delivery__new_message("sdp_offer", &json_message_object);
        json_message_value = cJSON_CreateObject();
        if (json_root_object == NULL_POINTER || json_message_value == NULL_POINTER || cJSON_AddStringToObject(json_message_value, "type", type) == NULL_POINTER || cJSON_AddStringToObject(json_message_value, "sdp", sdp) == NULL_POINTER)
        {
            goto label_descriptionCallback_end;
        }
        json_root_object_string = cJSON_PrintUnformatted(json_message_value);
        if (json_root_object_string == NULL_POINTER || cJSON_AddStringToObject(json_message_object, "value", json_root_object_string) == NULL_POINTER)
        {
            goto label_descriptionCallback_end;
        }
        cJSON_free(json_root_object_string);
        json_root_object_string = cJSON_PrintUnformatted(json_root_object);
        if (json_root_object_string != NULL_POINTER)
        {
            server_msg__delivery__queue_text_to_client(&g_clients_array[peer->client_id], json_root_object_string);
        }

        DBG_AUDIO_WEBRTC log_info("%s", "test 3");
    }

label_descriptionCallback_end:
    cJSON_free(json_root_object_string);
    cJSON_Delete(json_message_value);
    cJSON_Delete(json_root_object);

    clib__unlock(&g_webrtc_peers_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Rejects an unsolicited remote-created data channel without replacing an owned channel.
 *
 * @param int pc -> peer connection handle
 * @param int dc -> data channel handle
 * @param void* ptr -> the webrtc_peer_t this data channel belongs to
 *
 * @return void
 */
void RTC_API audio_webrtc__peerconnection_on_datachannel_callback(int pc, int dc, void *ptr)
{
    (void)pc;
    (void)ptr;

    // The protocol uses only server-created audio/video channels. This handle
    // has never had application callbacks installed, so deleting it cannot wait
    // for a callback that needs an application lock. The public RTC contract
    // waits for callbacks of dc, not for this incoming callback on pc.
    // Reject stale and null-peer deliveries in exactly the same way.
    if (dc > 0)
    {
        rtcSetUserPointer(dc, NULL_POINTER);
        rtcDeleteDataChannel(dc);
    }
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
static void _audio_webrtc_internal__send_public_candidate(const char *candidate, const char *mid, client_t *client)
{
    char foundation[64] = { 0 };
    char component[8] = { 0 };
    char transport[8] = { 0 };
    char ip[64] = { 0 };
    char port[16] = { 0 };
    char typ[8] = { 0 };
    char type[16] = { 0 };
    uint64 priority = 0;
    char rewritten[512] = { 0 };
    uint64 i = 0;
    boole status0 = FALSE;
    boole status1 = FALSE;
    boole status2 = FALSE;

    status0 = g_server_settings.is_wireguard_mode_active == FALSE;
    status1 = g_server_settings.webrtc_public_address[0] == 0;
    status2 = candidate == NULL_POINTER;
    if (status0 == TRUE || status1 == TRUE || status2 == TRUE)
    {
        return;
    }

    if (sscanf(candidate, "%63s %7s %7s %llu %63s %15s %7s %15s", foundation, component, transport, &priority, ip, port, typ, type) != 8)
    {
        return;
    }

    status0 = clib__is_string_equal(type, "host") == FALSE;
    if (status0 == FALSE)
    {
        status0 = clib__is_string_equal(typ, "typ") == FALSE;
    }
    if (status0 == TRUE)
    {
        return;
    }

    status0 = clib__is_string_equal(transport, "UDP") == FALSE;
    if (status0 == TRUE)
    {
        status0 = clib__is_string_equal(transport, "udp") == FALSE;
    }
    if (status0 == TRUE)
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
    if (priority > ((uint64)26 << 24))
    {
        priority = priority - ((uint64)26 << 24);
    }

    clib__null_memory(rewritten, sizeof(rewritten));
    clib__string_format(rewritten, sizeof(rewritten), "%sp %s %s %llu %s %s typ srflx raddr %s rport %s", foundation, component, transport, priority, g_server_settings.webrtc_public_address, port, ip, port);

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
void RTC_API audio_webrtc__peerconnection_on_icecandidate_callback(int pc, const char *cand, const char *mid, void *ptr)
{
    webrtc_peer_t *peer = 0;
    boole is_client_valid = FALSE;
    boole status0 = FALSE;
    boole status1 = FALSE;

    DBG_AUDIO_WEBRTC log_info("%s", "audio_webrtc__peerconnection_on_icecandidate_callback \n");

    peer = (webrtc_peer_t *)ptr;

    if (peer == NULL_POINTER)
    {
        return;
    }

    clib__read_lock(&g_clients_global_rwlock_guard);

    // stale peer connections (slot re-initialized since) must not push their candidates to the client
    status0 = peer->is_existing == TRUE;
    status1 = peer->peer_connection_handle == pc;
    if (status0 == TRUE && status1 == TRUE)
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
 * @brief runs when the peer connection state changes; updates the client's audio state and
 *        broadcasts it to the channel
 *
 * @param int pc -> peer connection handle
 * @param rtcState state -> the new rtcState
 * @param void* ptr -> the webrtc_peer_t whose connection changed
 *
 * @return void
 */
void RTC_API audio_webrtc__peerconnection_on_statechanged_callback(int pc, rtcState state, void *ptr)
{
    webrtc_peer_t *peer = 0;
    client_t *client = 0;
    boole status = FALSE;
    boole status0 = FALSE;
    boole status1 = FALSE;

    DBG_AUDIO_WEBRTC log_info("%s", "entered this function");

    peer = (webrtc_peer_t *)ptr;

    if (peer != NULL_POINTER)
    {
        DBG_AUDIO_WEBRTC log_info("%s %lld %s %s %s", "audio_webrtc__peerconnection_on_statechanged_callback ", peer->client_id, " to client ", _audio_webrtc_internal__state_print(state), "\n");

        clib__write_lock(&g_clients_global_rwlock_guard);
        clib__write_lock(&g_webrtc_peers_rwlock_guard);

        // a state change from a peer connection that is no longer the slot's current one (the slot was
        // re-initialized since) must be ignored - a callback of a replaced connection used to flip the live client's audio_state
        // to disabled here
        status0 = peer->is_existing == TRUE;
        status1 = peer->peer_connection_handle == pc;
        if (status0 == TRUE && status1 == TRUE)
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

                status0 = client->is_audio_websocket_authenticated == FALSE;
                status1 = g_server_settings.is_audio_over_webrtc_allowed == TRUE;
                if (status0 == TRUE && status1 == TRUE)
                {
                    client->audio_state = state == RTC_CONNECTED ? AUDIO_STATE__PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS : AUDIO_STATE__AUDIO_COMPLETELY_DISABLED;
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
void RTC_API audio_webrtc__peerconnection_on_gatheringstatechanged_callback(int pc, rtcGatheringState state, void *ptr)
{
    // log_info("%s %s %s", "audio_webrtc__peerconnection_on_gatheringstatechanged_callback", _audio_webrtc_internal__gathering_state_print(state), "\n");

    DBG_AUDIO_WEBRTC log_info("%s", "entered this function");

    if (ptr != NULL_POINTER)
    {
        DBG_AUDIO_WEBRTC log_info("%s %lld %s %s %s", "audio_webrtc__peerconnection_on_gatheringstatechanged_callback ", ((webrtc_peer_t *)ptr)->client_id, " to client ", _audio_webrtc_internal__gathering_state_print(state), "\n");
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
void RTC_API audio_webrtc__datachannel_on_open_callback(int id, void *ptr)
{
    webrtc_peer_t *peer = (webrtc_peer_t *)ptr;
    char buffer[256] = { 0 };
    boole status = FALSE;

    clib__null_memory(buffer, sizeof(buffer));

    DBG_AUDIO_WEBRTC log_info("%s %s", "audio_webrtc__datachannel_on_open_callback", "\n");

    clib__write_lock(&g_webrtc_peers_rwlock_guard);

    // only the slot's current data channel may mark the peer connected; a stale one opening late must not
    if (peer != NULL_POINTER)
    {
        status = peer->data_channel_handle == id;
    }
    if (status == TRUE)
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
 * @attention the slot may have been re-initialized since this data channel was created (the client retries the datachannel setup every 10 seconds and each retry overwrites the slot's handles). a stale data channel closing later must not clear the slot's CURRENT state - that silently removed the client from every audio relay loop while its live datachannel stayed open. only the handle that is still the slot's current one may clear the slot.
 *
 * @return void
 */
void RTC_API audio_webrtc__datachannel_on_closed_callback(int id, void *ptr)
{
    webrtc_peer_t *peer = 0;
    client_t *client = 0;
    webrtc_teardown_arg_t *cleanup = NULL_POINTER;
    uint64 client_id = 0;
    boole status = FALSE;
    boole status0 = FALSE;
    boole status1 = FALSE;

    DBG_AUDIO_WEBRTC log_info("%s %s", "audio_webrtc__datachannel_on_closed_callback", "\n");

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_webrtc_peers_rwlock_guard);

    peer = (webrtc_peer_t *)ptr;

    if (peer != NULL_POINTER)
    {
        status0 = peer->data_channel_handle == id;
    }
    if (status0 == TRUE)
    {
        // this close can beat the go_to_idle / disconnect paths, which then find an empty slot
        // and skip their teardown - so the dying pair must be detached and deleted from here
        client_id = peer->client_id;
        cleanup = _audio_webrtc_internal__detach_peer(peer);

        // the transport is gone, say so - the statechange callback skips cleared slots, so a client
        // still believing it is connected would otherwise never learn and never re-create
        status = base__is_client_valid(client_id);

        if (status == TRUE)
        {
            status0 = g_clients_array[client_id].is_audio_websocket_authenticated == FALSE;
            status1 = g_clients_array[client_id].audio_state != AUDIO_STATE__AUDIO_COMPLETELY_DISABLED;
            if (status0 == TRUE && status1 == TRUE)
            {
                client = &g_clients_array[client_id];
                client->audio_state = AUDIO_STATE__AUDIO_COMPLETELY_DISABLED;
                server_msg__audio__send_state_of_client_to_all_clients(client, AUDIO_STATE__AUDIO_COMPLETELY_DISABLED);
            }
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
    _audio_webrtc_internal__queue_teardown(cleanup);
}

/**
 * @brief a voice frame from a client's data channel, handed to the shared relay with the handle it
 *        arrived on, or the client's one-byte liveness probe, which goes straight back
 *
 * @param int id -> data channel handle the message arrived on
 * @param const char* message -> the received audio data
 * @param int size -> length in bytes of the received audio data
 * @param void* ptr -> the webrtc_peer_t of the sending client
 *
 * @return void
 */
void RTC_API audio_webrtc__datachannel_on_message_callback(int id, const char *message, int size, void *ptr)
{
    webrtc_peer_t *peer = ptr;
    uint64 sender_id = 0;
    boole status0 = FALSE;
    boole status1 = FALSE;

    status0 = peer == NULL_POINTER;
    status1 = size <= 0;
    if (status0 == TRUE || status1 == TRUE)
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
    audio_channel__relay_voice(sender_id, NULL_POINTER, id, (const ubyte *)message, (uint64)size);
}

/**
 * @brief returns a human-readable name for an rtcState value
 *
 * @param rtcState state -> the rtcState to name
 *
 * @return char* static string naming the state, or 0 if unknown
 */
static char *_audio_webrtc_internal__state_print(rtcState state)
{
    char *str = NULL_POINTER;

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
static char *_audio_webrtc_internal__gathering_state_print(rtcGatheringState state)
{
    char *str = NULL_POINTER;

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
 * @brief updates the client's webrtc peer with its new channel id when the client joins or switches
 *        a channel
 *
 * @attention takes the peers write lock
 *
 * @param client_t* client -> the client that changed channel
 *
 * @return void
 */
void audio_webrtc__process_client_channel_join(client_t *client)
{
    webrtc_peer_t *peer = NULL_POINTER;

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
 * @brief Deletes queued RTC objects without holding application or queue locks.
 * @param arg_void Unused; this process-lifetime worker owns each dequeued job.
 */
static void *_audio_webrtc_internal__teardown_thread(void *arg_void)
{
    webrtc_teardown_arg_t *cleanup = NULL_POINTER;

    (void)arg_void;
    for (;;)
    {
        pthread_mutex_lock(&g_webrtc_teardown_mutex);
        while (g_webrtc_teardown_head == NULL_POINTER)
        {
            pthread_cond_wait(&g_webrtc_teardown_changed, &g_webrtc_teardown_mutex);
        }
        cleanup = g_webrtc_teardown_head;
        g_webrtc_teardown_head = cleanup->next;
        if (g_webrtc_teardown_head == NULL_POINTER)
        {
            g_webrtc_teardown_tail = NULL_POINTER;
        }
        pthread_mutex_unlock(&g_webrtc_teardown_mutex);

        if (cleanup->data_channel_handle > 0)
        {
            rtcDeleteDataChannel(cleanup->data_channel_handle);
        }
        if (cleanup->video_data_channel_handle > 0)
        {
            rtcDeleteDataChannel(cleanup->video_data_channel_handle);
        }
        if (cleanup->peer_connection_handle > 0)
        {
            rtcDeletePeerConnection(cleanup->peer_connection_handle);
        }
        memorymanager__free((nuint)cleanup);
    }
    return NULL_POINTER;
}

/**
 * @brief Starts cleanup before accepting ownership of the first RTC object. A failed start leaves
 *        no RTC handles behind and a later setup may retry.
 */
static boole _audio_webrtc_internal__start_teardown_worker(void)
{
    boole started = FALSE;

    pthread_mutex_lock(&g_webrtc_teardown_mutex);
    if (g_webrtc_teardown_started == FALSE)
    {
        g_webrtc_teardown_started = util__start_detached_thread(_audio_webrtc_internal__teardown_thread, NULL_POINTER);
    }
    started = g_webrtc_teardown_started;
    pthread_mutex_unlock(&g_webrtc_teardown_mutex);
    return started;
}

/**
 * @brief Transfers a peer's handles into its reserved cleanup job.
 * @attention The caller holds the peers write lock. No allocation is needed.
 */
static webrtc_teardown_arg_t *_audio_webrtc_internal__detach_peer(webrtc_peer_t *peer)
{
    webrtc_teardown_arg_t *cleanup = peer->teardown;

    if (cleanup != NULL_POINTER)
    {
        cleanup->peer_connection_handle = peer->peer_connection_handle;
        cleanup->data_channel_handle = peer->data_channel_handle;
        cleanup->video_data_channel_handle = peer->video_data_channel_handle;
    }
    if (peer->data_channel_handle > 0)
    {
        rtcSetUserPointer(peer->data_channel_handle, NULL_POINTER);
    }
    if (peer->video_data_channel_handle > 0)
    {
        rtcSetUserPointer(peer->video_data_channel_handle, NULL_POINTER);
    }
    if (peer->peer_connection_handle > 0)
    {
        rtcSetUserPointer(peer->peer_connection_handle, NULL_POINTER);
    }
    clib__null_memory(peer, sizeof(*peer));
    return cleanup;
}

/**
 * @brief Gives a reserved job to the worker without allocating or starting a thread. The worker
 *        releases the queue lock before waiting for RTC callbacks to finish.
 */
static void _audio_webrtc_internal__queue_teardown(webrtc_teardown_arg_t *cleanup)
{
    if (cleanup == NULL_POINTER)
    {
        return;
    }
    cleanup->next = NULL_POINTER;
    pthread_mutex_lock(&g_webrtc_teardown_mutex);
    if (g_webrtc_teardown_tail == NULL_POINTER)
    {
        g_webrtc_teardown_head = cleanup;
    }
    else
    {
        g_webrtc_teardown_tail->next = cleanup;
    }
    g_webrtc_teardown_tail = cleanup;
    pthread_cond_signal(&g_webrtc_teardown_changed);
    pthread_mutex_unlock(&g_webrtc_teardown_mutex);
}

/**
 * @brief tears down a client's webrtc peer when the client disconnects
 *
 * @param client_t* client -> the client that disconnected
 *
 * @return void
 */
void audio_webrtc__process_client_disconnect(client_t *client)
{
    webrtc_teardown_arg_t *cleanup = NULL_POINTER;

    clib__write_lock(&g_webrtc_peers_rwlock_guard);
    cleanup = _audio_webrtc_internal__detach_peer(&g_webrtc_peers_array[client->client_id]);
    clib__unlock(&g_webrtc_peers_rwlock_guard);
    _audio_webrtc_internal__queue_teardown(cleanup);
}

/**
 * @brief Adds a client's ICE candidate to its WebRTC connection.
 * @param client_t* client -> client that owns the connection
 * @param cstring candidate -> checked candidate from the client's message
 */
void audio_webrtc__add_remote_candidate(client_t *client, cstring candidate)
{
    webrtc_peer_t *peer = NULL_POINTER;

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
void audio_webrtc__set_remote_description(client_t *client, cstring sdp, cstring type)
{
    webrtc_peer_t *peer = NULL_POINTER;

    clib__write_lock(&g_webrtc_peers_rwlock_guard);
    peer = &g_webrtc_peers_array[client->client_id];
    if (peer->is_existing == TRUE)
    {
        rtcSetRemoteDescription(peer->peer_connection_handle, sdp, type);
    }
    clib__unlock(&g_webrtc_peers_rwlock_guard);
}

/**
 * @brief Creates the client's WebRTC connection and audio data channel.
 *
 * Reconnecting detaches the previous connection and channels before replacing
 * the slot, so their callbacks cannot change the new connection's state.
 * Their reserved cleanup jobs are then passed to the cleanup worker.
 *
 * @param client_t* client -> client requesting the connection
 * @attention The caller holds the clients read or write lock. This function takes the peers write lock while rebuilding the slot.
 * @return boole -> TRUE when both resources were created; FALSE when the client is missing, inactive or unauthenticated, or setup fails.
 */
boole audio_webrtc__initialize_datachannel_connection(client_t *client)
{
    webrtc_peer_t *peer = NULL_POINTER;
    webrtc_teardown_arg_t *reserved = NULL_POINTER;
    webrtc_teardown_arg_t *old_cleanup = NULL_POINTER;
    webrtc_teardown_arg_t *failed_cleanup = NULL_POINTER;
    boole result = FALSE;
    boole status0 = FALSE;
    boole status1 = FALSE;
    rtcConfiguration config;
    rtcDataChannelInit init;
    uint64 attempts = 0;
    uint64 cooldown = 0;
    const char *ice_servers[1] = { "127.0.0.1:3478" };

    if (client == NULL_POINTER)
    {
        return FALSE;
    }
    status0 = client->is_existing == FALSE;
    status1 = client->is_authenticated == FALSE;
    if (status0 == TRUE || status1 == TRUE)
    {
        return FALSE;
    }

    // Both failure-prone cleanup prerequisites come before creating any RTC
    // object or replacing the client's current transport.
    reserved = (webrtc_teardown_arg_t *)memorymanager__allocate(sizeof(*reserved), MEMALLOC_WEBRTC_PEERS);
    if (reserved == NULL_POINTER)
    {
        return FALSE;
    }
    status0 = _audio_webrtc_internal__start_teardown_worker();
    if (status0 == FALSE)
    {
        memorymanager__free((nuint)reserved);
        return FALSE;
    }
    clib__null_memory(&config, sizeof(config));
    config.iceServers = ice_servers;
    config.iceServersCount = 1;
    if (g_server_settings.is_wireguard_mode_active == TRUE)
    {
        config.portRangeBegin = (ushort)g_server_settings.webrtc_port_range_begin;
        config.portRangeEnd = (ushort)g_server_settings.webrtc_port_range_end;
    }

    clib__write_lock(&g_webrtc_peers_rwlock_guard);
    peer = &g_webrtc_peers_array[client->client_id];
    attempts = peer->attempts_since_connected;
    cooldown = peer->cooldown_until_ms;
    old_cleanup = _audio_webrtc_internal__detach_peer(peer);
    peer->teardown = reserved;
    peer->is_existing = TRUE;
    peer->p_ws_connection = client->p_ws_connection;
    peer->client_id = client->client_id;
    peer->channel_id = client->channel_id;
    peer->last_sent_audio_state = AUDIO_STATE__AUDIO_COMPLETELY_DISABLED;
    clib__string_copy(peer->dh_shared_secret, sizeof(peer->dh_shared_secret), client->dh_shared_secret);
    peer->peer_connection_handle = rtcCreatePeerConnection(&config);
    if (peer->peer_connection_handle <= 0)
    {
        peer->peer_connection_handle = 0;
        goto end;
    }

    rtcSetUserPointer(peer->peer_connection_handle, peer);
    rtcSetLocalDescriptionCallback(peer->peer_connection_handle, audio_webrtc__peerconnection_on_setlocaldescription_callback);
    rtcSetLocalCandidateCallback(peer->peer_connection_handle, audio_webrtc__peerconnection_on_icecandidate_callback);
    rtcSetStateChangeCallback(peer->peer_connection_handle, audio_webrtc__peerconnection_on_statechanged_callback);
    rtcSetGatheringStateChangeCallback(peer->peer_connection_handle, audio_webrtc__peerconnection_on_gatheringstatechanged_callback);
    rtcSetDataChannelCallback(peer->peer_connection_handle, audio_webrtc__peerconnection_on_datachannel_callback);
    clib__null_memory(&init, sizeof(init));
    init.reliability.unreliable = TRUE;
    init.reliability.unordered = TRUE;
    init.reliability.maxRetransmits = 0;
    peer->data_channel_handle = rtcCreateDataChannelEx(peer->peer_connection_handle, "testQQQ", &init);
    if (peer->data_channel_handle <= 0)
    {
        peer->data_channel_handle = 0;
        goto end;
    }
    rtcSetOpenCallback(peer->data_channel_handle, audio_webrtc__datachannel_on_open_callback);
    rtcSetClosedCallback(peer->data_channel_handle, audio_webrtc__datachannel_on_closed_callback);
    rtcSetMessageCallback(peer->data_channel_handle, audio_webrtc__datachannel_on_message_callback);
    rtcSetErrorCallback(peer->data_channel_handle, audio_webrtc__datachannel_on_error_callback);
    video_webrtc__create_data_channel(peer);
    result = TRUE;

end:
    if (result == FALSE)
    {
        failed_cleanup = _audio_webrtc_internal__detach_peer(peer);
    }
    peer->attempts_since_connected = attempts;
    peer->cooldown_until_ms = cooldown;
    clib__unlock(&g_webrtc_peers_rwlock_guard);
    _audio_webrtc_internal__queue_teardown(old_cleanup);
    _audio_webrtc_internal__queue_teardown(failed_cleanup);
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
    webrtc_peer_t *peer = &g_webrtc_peers_array[client_id];
    boole status0 = FALSE;
    boole status1 = FALSE;

    status0 = peer->is_existing == TRUE;
    status1 = peer->connected == TRUE;
    return status0 == TRUE && status1 == TRUE;
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
    webrtc_peer_t *peer = &g_webrtc_peers_array[client_id];
    boole status0 = FALSE;
    boole status1 = FALSE;
    boole status2 = FALSE;

    status0 = peer->is_existing == TRUE;
    status1 = peer->connected == TRUE;
    status2 = peer->data_channel_handle == data_channel;
    return status0 == TRUE && status1 == TRUE && status2 == TRUE;
}

/**
 * @brief sends one relayed audio packet over the client's data channel
 *
 * @param uint64 client_id -> slot of the receiving client
 * @param const ubyte* packet -> the framed packet
 * @param uint64 size -> its length in bytes
 *
 * @attention peers read lock required
 *
 * @return boole -> TRUE when the packet was handed to libdatachannel
 */
boole audio_webrtc__send(uint64 client_id, const ubyte *packet, uint64 size)
{
    webrtc_peer_t *peer = &g_webrtc_peers_array[client_id];
    boole status0 = FALSE;
    boole status1 = FALSE;
    boole status2 = FALSE;

    status0 = peer->is_existing == FALSE;
    status1 = peer->connected == FALSE;
    status2 = peer->data_channel_handle == 0;
    if (status0 == TRUE || status1 == TRUE || status2 == TRUE)
    {
        return FALSE;
    }
    if (rtcSendMessage(peer->data_channel_handle, (const char *)packet, (int)size) < 0)
    {
        return FALSE;
    }
    base__count_webrtc_bytes(0, size);
    return TRUE;
}
