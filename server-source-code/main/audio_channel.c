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

// opus_data_buffer_entry_t* opus_data_buffer_entries_array = 0;
webrtc_peer_t* g_webrtc_muggles_array = 0;

void RTC_API peerconnection_on_setlocaldescription_callback(int pc, const char* sdp, const char* type, void* ptr);
void RTC_API peerconnection_on_icecandidate_callback(int pc, const char* cand, const char* mid, void* ptr);
void RTC_API peerconnection_on_statechanged_callback(int pc, rtcState state, void* ptr);
void RTC_API peerconnection_on_gatheringstatechanged_callback(int pc, rtcGatheringState state, void* ptr);
void RTC_API peerconnection_on_datachannel_callback(int pc, int dc, void* ptr);
void RTC_API datachannel_on_open_callback(int id, void* ptr);
void RTC_API datachannel_on_closed_callback(int id, void* ptr);
void RTC_API datachannel_on_message_callback(int id, const char* message, int size, void* ptr);
char* state_print(rtcState state);
char* rtcGatheringState_print(rtcGatheringState state);

static void* _audio_channel_internal__webrtc_teardown_thread(void* arg_void);
static void _audio_channel_internal__spawn_webrtc_teardown(int peer_connection_handle, int data_channel_handle);

/**
 * @brief error callback
 *
 * @param int id -> data channel handle
 * @param const char* error -> the error description string
 * @param void* ptr -> the webrtc_peer_t this data channel belongs to
 *
 * @return void
 */
void RTC_API datachannel_onerror_callback(int id, const char* error, void* ptr)
{
    DBG_AUDIOCHANNEL_WEBRTC log_info("%s %s %s", "DataChannel from %s error: %s\n", error, "\n");
}

/**
 * @brief when our local SDP description is ready, encrypts it and sends it to the client over the websocket
 *
 * @param int pc -> peer connection handle
 * @param const char* sdp -> the local SDP description string
 * @param const char* type -> the description type ("offer" / "answer")
 * @param void* ptr -> the webrtc_peer_t this description belongs to
 *
 * @return void
 */
void RTC_API peerconnection_on_setlocaldescription_callback(int pc, const char* sdp, const char* type, void* ptr)
{
    char* text_to_send = 0;
    cJSON* json_root_object = NULL_POINTER;
    cJSON* json_message_object = NULL_POINTER;
    cJSON* json_message_value = NULL_POINTER;
    char* json_root_object_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    cJSON* json_type = NULL_POINTER;
    cJSON* json_sdp = NULL_POINTER;
    webrtc_peer_t* peer = 0;
    boole status = FALSE;

    DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "peerconnection_on_setlocaldescription_callback \n");

    clib__read_lock(&g_clients_global_rwlock_guard);
    clib__read_lock(&g_webrtc_muggles_rwlock_guard);

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

    status = util__is_client_valid(peer->client_id);

    if (status == FALSE)
    {
        goto label_descriptionCallback_end;
    }

    if (clib__is_string_equal(type, "offer") == TRUE)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "we are creating offer \n");
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s %p %s", "peerconnection_on_setlocaldescription_callback", peer, "\n");

        json_root_object = cJSON_CreateObject();
        json_message_object = cJSON_CreateObject();

        json_message_value = cJSON_CreateObject(); // this object will be parsed to string down below, and converted to json again when received on client's side

        DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "test 2");

        json_type = cJSON_CreateString(type);
        cJSON_AddItemToObject(json_message_value, "type", json_type);
        json_sdp = cJSON_CreateString(sdp);
        cJSON_AddItemToObject(json_message_value, "sdp", json_sdp);
        json_root_object_string = cJSON_PrintUnformatted(json_message_value);

        cJSON_AddStringToObject(json_message_object, "value", json_root_object_string);

        cJSON_AddStringToObject(json_message_object, "type", "sdp_offer");
        cJSON_AddItemToObject(json_root_object, "message", json_message_object);

        json_root_object_string = cJSON_PrintUnformatted(json_root_object);

        text_to_send = base__encrypt_cstring_and_convert_to_base64(json_root_object_string, &size_of_allocated_message_buffer, peer->dh_shared_secret);
        ws_sendframe_txt(peer->p_ws_connection, text_to_send);

        base__free_json_message(json_root_object, json_root_object_string);

        memorymanager__free((nuint)text_to_send);
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "test 3");
    }

label_descriptionCallback_end:

    clib__unlock(&g_webrtc_muggles_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief fires when the client opens the data channel; stores its handle and wires up the message and close callbacks
 *
 * @param int pc -> peer connection handle
 * @param int dc -> data channel handle
 * @param void* ptr -> the webrtc_peer_t this data channel belongs to
 *
 * @return void
 */
void RTC_API peerconnection_on_datachannel_callback(int pc, int dc, void* ptr)
{
    webrtc_peer_t* peer = (webrtc_peer_t*)ptr;
    char buffer[256];

    clib__null_memory(buffer, sizeof(buffer));

    DBG_AUDIOCHANNEL_WEBRTC log_info("%s %s", "void RTC_API peerconnection_on_datachannel_callback(int peer_connection_handle, int data_channel_handle, void* ptr)", "\n");

    clib__write_lock(&g_webrtc_muggles_rwlock_guard);

    // only the slot's current peer connection may hand the slot a data channel; a stale one must not
    if (peer != NULL_POINTER && peer->peer_connection_handle == pc)
    {
        peer->data_channel_handle = dc;
        peer->connected = TRUE;
        rtcSetClosedCallback(dc, datachannel_on_closed_callback);
        rtcSetMessageCallback(dc, datachannel_on_message_callback);

        if (rtcGetDataChannelLabel(dc, buffer, 256) >= 0)
        {
            DBG_AUDIOCHANNEL_WEBRTC log_info("%s%s%s%s%s", "peerconnection_on_datachannel_callback DataChannel ", "answerer", ": Received with label \"", buffer, "\"\n");
        }
    }

    clib__unlock(&g_webrtc_muggles_rwlock_guard);
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
void RTC_API peerconnection_on_icecandidate_callback(int pc, const char* cand, const char* mid, void* ptr)
{
    webrtc_peer_t* peer = 0;
    boole is_client_valid = FALSE;

    DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "peerconnection_on_icecandidate_callback \n");

    peer = (webrtc_peer_t*)ptr;

    if (peer == NULL_POINTER)
    {
        return;
    }

    clib__read_lock(&g_clients_global_rwlock_guard);

    // stale peer connections (slot re-initialized since) must not push their candidates to the client
    if (peer->is_existing == TRUE && peer->peer_connection_handle == pc)
    {
        is_client_valid = util__is_client_valid(peer->client_id);

        if (is_client_valid == FALSE)
        {
            goto _label_peerconnection_on_icecandidate_callback_end;
        }

        DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "sending sdp offer to client \n");
        server_msg__send_webrtc_sdp_offer_to_single_client(cand, mid, &g_clients_array[peer->client_id]);
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
void audio_channel__set_is_client_sending_audio(uint64 client_id, boole is_active)
{
    g_webrtc_muggles_array[client_id].is_sending_audio_right_now = is_active;
    // to add lock?
}

/**
 * @brief fires when the peer connection state changes; updates the client's audio state and broadcasts it to the channel
 *
 * @param int pc -> peer connection handle
 * @param rtcState state -> the new rtcState
 * @param void* ptr -> the webrtc_peer_t whose connection changed
 *
 * @return void
 */
void RTC_API peerconnection_on_statechanged_callback(int pc, rtcState state, void* ptr)
{
    webrtc_peer_t* peer = 0;
    client_t* client = 0;
    boole status = FALSE;

    DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "entered this function");

    peer = (webrtc_peer_t*)ptr;

    if (peer != NULL_POINTER)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s %lld %s %s %s", "peerconnection_on_statechanged_callback ", peer->client_id, " to client ", state_print(state), "\n");

        clib__write_lock(&g_clients_global_rwlock_guard);
        clib__write_lock(&g_webrtc_muggles_rwlock_guard);

        // a state change from a peer connection that is no longer the slot's current one (the slot was
        // re-initialized since) must be ignored - a dying orphan used to flip the live client's audio_state
        // to disabled here
        if (peer->is_existing == TRUE && peer->peer_connection_handle == pc)
        {
            // connected was set on datachannel open and never cleared anywhere; the re-create gate
            // reads it, so a silently dead transport must drop it here
            peer->connected = (boole)(state == RTC_CONNECTED);

            // a transport that came up clears the attempt count and any cooldown behind it
            if (state == RTC_CONNECTED)
            {
                peer->attempts_since_connected = 0;
                peer->cooldown_until_ms = 0;
            }

            status = util__is_client_valid(peer->client_id);

            if (status == TRUE)
            {
                DBG_AUDIOCHANNEL_WEBRTC log_info("%s %s %s", "peerconnection_on_statechanged_callback ", state_print(state), "\n");
                client = &g_clients_array[peer->client_id];

                if (state == RTC_CONNECTED)
                {
                    client->audio_state = AUDIO_STATE__PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS;
                }
                else
                {
                    client->audio_state = AUDIO_STATE__AUDIO_COMPLETELY_DISABLED;
                }

                if (peer->last_sent_audio_state != client->audio_state)
                {
                    peer->last_sent_audio_state = client->audio_state;
                    server_msg__send_audio_state_of_client_to_all_clients(peer->client_id, client->audio_state);
                }
            }
        }
        clib__unlock(&g_webrtc_muggles_rwlock_guard);
        clib__unlock(&g_clients_global_rwlock_guard);
    }
}

/**
 * @brief fires when the peer connection's ICE gathering state changes (currently only logged)
 *
 * @param int pc -> peer connection handle
 * @param rtcGatheringState state -> the new rtcGatheringState
 * @param void* ptr -> the webrtc_peer_t whose gathering state changed
 *
 * @return void
 */
void RTC_API peerconnection_on_gatheringstatechanged_callback(int pc, rtcGatheringState state, void* ptr)
{
    // log_info("%s %s %s", "peerconnection_on_gatheringstatechanged_callback", rtcGatheringState_print(state), "\n");

    DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "entered this function");

    if (ptr != NULL_POINTER)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s %lld %s %s %s", "peerconnection_on_gatheringstatechanged_callback ", ((webrtc_peer_t*)ptr)->client_id, " to client ", rtcGatheringState_print(state), "\n");
    }

    if (state == RTC_GATHERING_COMPLETE)
    {
    }
}

/**
 * @brief fires when the data channel opens; marks the peer connected
 *
 * @param int id -> data channel handle
 * @param void* ptr -> the webrtc_peer_t this data channel belongs to
 *
 * @return void
 */
void RTC_API datachannel_on_open_callback(int id, void* ptr)
{
    webrtc_peer_t* peer = (webrtc_peer_t*)ptr;
    char buffer[256];

    clib__null_memory(buffer, sizeof(buffer));

    DBG_AUDIOCHANNEL_WEBRTC log_info("%s %s", "datachannel_on_open_callback", "\n");

    clib__write_lock(&g_webrtc_muggles_rwlock_guard);

    // only the slot's current data channel may mark the peer connected; a stale one opening late must not
    if (peer != NULL_POINTER && peer->data_channel_handle == id)
    {
        peer->connected = TRUE;
        if (rtcGetDataChannelLabel(peer->data_channel_handle, buffer, 256) >= 0)
        {
            DBG_AUDIOCHANNEL_WEBRTC log_info("%s%s%s%s%s", "DataChannel ", "offerer", ": Received with label \"", buffer, "\"\n");
        }
    }

    clib__unlock(&g_webrtc_muggles_rwlock_guard);
}

/**
 * @brief fires when the data channel closes; clears the peer's slot
 *
 * @param int id -> data channel handle
 * @param void* ptr -> the webrtc_peer_t this data channel belongs to
 *
 * @attention the slot may have been re-initialized since this data channel was created (the client retries
 *            the datachannel setup every 10 seconds and each retry overwrites the slot's handles). a stale
 *            data channel dying later must not wipe the slot's CURRENT state - that silently removed the
 *            client from every audio relay loop while its live datachannel stayed open. only the handle
 *            that is still the slot's current one may clear the slot.
 *
 * @return void
 */
void RTC_API datachannel_on_closed_callback(int id, void* ptr)
{
    webrtc_peer_t* peer = 0;
    client_t* client = 0;
    int peer_connection_handle = 0;
    int data_channel_handle = 0;
    uint64 client_id = 0;
    boole status = FALSE;

    DBG_AUDIOCHANNEL_WEBRTC log_info("%s %s", "datachannel_on_closed_callback", "\n");

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_webrtc_muggles_rwlock_guard);

    peer = (webrtc_peer_t*)ptr;

    if (peer != NULL_POINTER && peer->data_channel_handle == id)
    {
        // this close can beat the go_to_idle / disconnect paths, which then find an empty slot
        // and skip their teardown - so the dying pair must be detached and deleted from here
        peer_connection_handle = peer->peer_connection_handle;
        data_channel_handle = peer->data_channel_handle;
        client_id = peer->client_id;

        if (data_channel_handle != 0)
        {
            rtcSetUserPointer(data_channel_handle, NULL_POINTER);
        }
        if (peer_connection_handle != 0)
        {
            rtcSetUserPointer(peer_connection_handle, NULL_POINTER);
        }

        clib__null_memory(peer, sizeof(webrtc_peer_t));

        // the transport is gone, say so - the statechange callback skips wiped slots, so a client
        // still believing it is connected would otherwise never learn and never re-create
        status = util__is_client_valid(client_id);

        if (status == TRUE && g_clients_array[client_id].audio_state != AUDIO_STATE__AUDIO_COMPLETELY_DISABLED)
        {
            client = &g_clients_array[client_id];
            client->audio_state = AUDIO_STATE__AUDIO_COMPLETELY_DISABLED;
            server_msg__send_audio_state_of_client_to_all_clients(client_id, AUDIO_STATE__AUDIO_COMPLETELY_DISABLED);
        }
    }

    clib__unlock(&g_webrtc_muggles_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);

    // deleting inline would deadlock: rtcDelete* blocks on libdatachannel threads whose
    // callbacks take the locks used above
    _audio_channel_internal__spawn_webrtc_teardown(peer_connection_handle, data_channel_handle);
}

/**
 * @brief sends UDP voice data to other clients in same channel.
 *
 * @param int id -> data channel handle the message arrived on
 * @param const char* message -> the received audio data buffer
 * @param int size -> length in bytes of the received audio data
 * @param void* ptr -> the webrtc_peer_t of the sending client
 *
 * @attention this cant use rwlocks. The audio transfer would be too slow
 * The reads im doing here are safe so Im not sure if that is a problem
 *
 * @return void
 */
void RTC_API datachannel_on_message_callback(int id, const char* message, int size, void* ptr)
{
    // printf("%s %s", "datachannel_on_message_callback" , "\n");
    webrtc_peer_t* peer_receiver = 0;
    webrtc_peer_t* peer_sender = 0;
    int dc = 0;
    boole status = FALSE;
    boole is_sending_audio = FALSE;

    if (size < 0)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "datachannel_on_message_callback size < 0\n");
    }
    else
    {
        peer_sender = (webrtc_peer_t*)ptr;

        // a detached orphan (slot re-initialized or client disconnected) has a NULL user pointer;
        // nothing from it may be relayed
        if (peer_sender == NULL_POINTER)
        {
            return;
        }

        is_sending_audio = peer_sender->is_sending_audio_right_now;

        if (is_sending_audio == TRUE)
        {
            // sent audio data to other clients located in same channel as found client, if they too have authenticated audio websocket

            for (uint64 i = 0; i < g_server_settings.max_client_count; i++)
            {
                void* message1 = NULL_POINTER;

                peer_sender = (webrtc_peer_t*)ptr;

                peer_receiver = &g_webrtc_muggles_array[i];

                if (peer_receiver->is_existing == FALSE)
                {
                    continue;
                }
                else if (peer_receiver->channel_id != peer_sender->channel_id)
                {
                    continue;
                }

                // is_voice_chat_active is the server-wide audio switch: when off the datachannel stays up
                // but the server stops re-transmitting audio between clients (checked live here)
                status = g_server_settings.is_voice_chat_active && g_channel_array[peer_receiver->channel_id].is_audio_enabled && g_channel_array[peer_receiver->channel_id].is_existing;

                if (status == FALSE)
                {
                    continue;
                }

                if (i == peer_sender->client_id) // don't want to send a message to ourselves
                {
                    continue;
                }

                // DBG_AUDIOCHANNEL_WEBRTC log_info("%s %s %s", "sending data to ", clients_array[i].username, "\n");

                message1 = (void*)memorymanager__allocate(size + 5, MEMALLOC_AUDIOCHANNEL_ONMESSAGE);
                if (message1 == NULL_POINTER)
                {
                    continue;
                }
                ((int*)message1)[0] = peer_sender->client_id;
                clib__copy_memory((void*)message, ((unsigned char*)message1 + 4), size, size);

                rtcSendMessage(peer_receiver->data_channel_handle, message1, size + 4);

                memorymanager__free((nuint)message1);
            }
        }
    }
}

/**
 * @brief returns a human-readable name for an rtcState value
 *
 * @param rtcState state -> the rtcState to name
 *
 * @return char* static string naming the state, or 0 if unknown
 */
char* state_print(rtcState state)
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
char* rtcGatheringState_print(rtcGatheringState state)
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
 * @param client_t* client -> the client that changed channel
 *
 * @return void
 */
void audio_channel__process_client_channel_join(client_t* client)
{
    webrtc_peer_t* peer = NULL_POINTER;

    clib__write_lock(&g_webrtc_muggles_rwlock_guard);

    peer = &g_webrtc_muggles_array[client->client_id];

    if (peer->is_existing == FALSE)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__process_client_channel_join peer not is_existing \n");
        goto label_audio_channel__process_client_channel_switch_end;
    }

    peer->channel_id = client->channel_id;

label_audio_channel__process_client_channel_switch_end:

    clib__unlock(&g_webrtc_muggles_rwlock_guard);

    return;
}

/**
 * @brief detached thread that deletes a disconnected peer's libdatachannel objects, holding no locks
 *
 * @param void* arg_void -> heap webrtc_teardown_arg_t carrying the two handles to delete (freed here)
 *
 * @attention never joined; rtcDeletePeerConnection blocks on libdatachannel threads whose callbacks need locks the disconnect path holds, so it must run off-thread to avoid the deadlock that froze the server
 *
 * @return void* always 0
 */
static void* _audio_channel_internal__webrtc_teardown_thread(void* arg_void)
{
    webrtc_teardown_arg_t* arg = NULL_POINTER;

    arg = (webrtc_teardown_arg_t*)arg_void;

    if (arg->data_channel_handle != 0)
    {
        rtcDeleteDataChannel(arg->data_channel_handle);
    }

    if (arg->peer_connection_handle != 0)
    {
        rtcDeletePeerConnection(arg->peer_connection_handle);
    }

    memorymanager__free((nuint)arg);

    return NULL_POINTER;
}

/**
 * @brief hands a detached pc/dc pair to the detached teardown thread for deletion
 *
 * @param int peer_connection_handle -> peer connection to delete, 0 for none
 * @param int data_channel_handle -> data channel to delete, 0 for none
 *
 * @attention the handles must already be detached from their slot (user pointer NULLed);
 *            call this while holding no locks the libdatachannel callbacks may want
 *
 * @return void
 */
static void _audio_channel_internal__spawn_webrtc_teardown(int peer_connection_handle, int data_channel_handle)
{
    webrtc_teardown_arg_t* teardown_arg = NULL_POINTER;
    pthread_t teardown_thread = 0;

    if (peer_connection_handle == 0 && data_channel_handle == 0)
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

    if (pthread_create(&teardown_thread, 0, _audio_channel_internal__webrtc_teardown_thread, (void*)teardown_arg) == 0)
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
void audio_channel__process_client_disconnect(client_t* client)
{
    webrtc_peer_t* peer = NULL_POINTER;
    int peer_connection_handle = 0;
    int data_channel_handle = 0;

    DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__process_client_disconnect \n");

    clib__write_lock(&g_webrtc_muggles_rwlock_guard);

    peer = &g_webrtc_muggles_array[client->client_id];

    if (peer->is_existing == FALSE)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__process_client_disconnect peer not is_existing \n");
        clib__unlock(&g_webrtc_muggles_rwlock_guard);
        return;
    }

    // snapshot the libdatachannel handles, then clear the slot so the lock-free relay callback stops referencing this peer the moment we unlock
    peer_connection_handle = peer->peer_connection_handle;
    data_channel_handle = peer->data_channel_handle;

    // detach the dying objects from the slot (every callback null-checks the user pointer): the slot index
    // is reused for the next connecting client, and a late callback from these objects must not touch the
    // new occupant's state
    if (data_channel_handle != 0)
    {
        rtcSetUserPointer(data_channel_handle, NULL_POINTER);
    }
    if (peer_connection_handle != 0)
    {
        rtcSetUserPointer(peer_connection_handle, NULL_POINTER);
    }

    clib__null_memory(peer, sizeof(webrtc_peer_t));

    DBG_AUDIOCHANNEL_WEBRTC log_info("%s %lld %s", "audio_channel__process_client_disconnect peer ", client->client_id, "disconnected \n");

    clib__unlock(&g_webrtc_muggles_rwlock_guard);

    // delete the libdatachannel objects on a detached thread that holds no locks. rtcDeletePeerConnection
    // blocks until libdatachannel's threads finish, and those callbacks need locks this disconnect path's
    // callers hold, so deleting inline deadlocked (the original code commented these out and leaked)
    _audio_channel_internal__spawn_webrtc_teardown(peer_connection_handle, data_channel_handle);
}

/**
 * @brief applies an ICE candidate received from the client to that client's peer connection
 *
 * @param client_t* client -> the client the candidate came from
 * @param cJSON* json_root -> parsed message; its "candidate" field holds the ICE candidate string
 *
 * @return void
 */
void audio_channel__process_ice_candidate_from_remote_peer(client_t* client, cJSON* json_root)
{
    cJSON* cjson_candidate = NULL_POINTER;
    webrtc_peer_t* peer = NULL_POINTER;

    DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__process_ice_candidate_from_remote_peer \n");

    if (json_root == NULL_POINTER)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__process_ice_candidate_from_remote_peer json root is null \n");
        return;
    }

    clib__read_lock(&g_webrtc_muggles_rwlock_guard);

    peer = &g_webrtc_muggles_array[client->client_id];

    if (peer->is_existing == FALSE)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__process_ice_candidate_from_remote_peerk peer not is_existing \n");
        goto label_audio_channel__process_ice_candidate_from_remote_peer_end;
    }

    cjson_candidate = cJSON_GetObjectItemCaseSensitive(json_root, "candidate");

    if (cjson_candidate == NULL_POINTER)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s %lld %s", "process_ice_candidate_from_remote_peer ", client->client_id, "candicate is null \n");
        goto label_audio_channel__process_ice_candidate_from_remote_peer_end;
    }

    if (cJSON_IsString(cjson_candidate) == FALSE || cjson_candidate->valuestring == NULL_POINTER)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s %lld %s", "process_ice_candidate_from_remote_peer ", client->client_id, "cJSON_IsString(cjson_candidate) == FALSE || cjson_candidate->valuestring == NULL \n");
        goto label_audio_channel__process_ice_candidate_from_remote_peer_end;
    }

    DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "process_ice_candidate_from_remote_peer rtcAddRemoteCandidate \n");

    // 3rd arg is the sdpMid (id of the SDP media section the candidate belongs to); 0 means the single default media line; WebRTC's other router is sdpMLineIndex, the m= line's zero-based position
    rtcAddRemoteCandidate(peer->peer_connection_handle, cjson_candidate->valuestring, 0); // it is possible that value cjson_candidate->valuestring will have to be stored on heap, test for crashes

label_audio_channel__process_ice_candidate_from_remote_peer_end:

    clib__unlock(&g_webrtc_muggles_rwlock_guard);
}

/**
 * @brief applies the client's SDP answer to its peer connection, completing the offer/answer handshake
 *
 * @param client_t* client -> the client that sent the answer
 * @param cJSON* RTCSessionDescription -> parsed message holding the answer's "type" and "sdp" fields
 *
 * @return void
 */
void audio_channel__process_sdp_answer_from_remote_peer(client_t* client, cJSON* RTCSessionDescription)
{
    webrtc_peer_t* peer = NULL_POINTER;
    cJSON* cjson_type = NULL_POINTER;
    cJSON* cjson_sdp = NULL_POINTER;

    DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__process_sdp_answer_from_remote_peer \n");

    if (RTCSessionDescription == NULL_POINTER || client == NULL_POINTER)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__process_sdp_answer_from_remote_peer RTCSessionDescription == NULL_POINTER || client == NULL_POINTER  \n");
        return;
    }

    clib__write_lock(&g_webrtc_muggles_rwlock_guard);

    peer = &g_webrtc_muggles_array[client->client_id];

    if (peer->is_existing == FALSE)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__process_sdp_answer_from_remote_peer peer not is_existing \n");
        goto label_audio_channel__process_sdp_answer_from_remote_peer_end;
    }

    cjson_type = cJSON_GetObjectItemCaseSensitive(RTCSessionDescription, "type");
    cjson_sdp = cJSON_GetObjectItemCaseSensitive(RTCSessionDescription, "sdp");

    if (cjson_type == 0 || cjson_sdp == 0)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s %lld %s", "[!] audio_channel__process_sdp_answer_from_remote_peer : ", client->client_id, "\n");
        goto label_audio_channel__process_sdp_answer_from_remote_peer_end;
    }

    if (cJSON_IsString(cjson_type) == FALSE || cjson_type->valuestring == NULL_POINTER || (cjson_type->valuestring) == 0)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s%lld%s", "[!] audio_channel__process_sdp_answer_from_remote_peer : ", client->client_id, "\n");
        goto label_audio_channel__process_sdp_answer_from_remote_peer_end;
    }

    if (cJSON_IsString(cjson_sdp) == FALSE || cjson_sdp->valuestring == NULL_POINTER || clib__utf8_string_length(cjson_sdp->valuestring) == 0)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s%lld%s", "[!] audio_channel__process_sdp_answer_from_remote_peer : ", client->client_id, "\n");
        goto label_audio_channel__process_sdp_answer_from_remote_peer_end;
    }

    DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__process_sdp_answer_from_remote_peer rtcSetRemoteDescription \n");
    rtcSetRemoteDescription(peer->peer_connection_handle, cjson_sdp->valuestring, cjson_type->valuestring);

label_audio_channel__process_sdp_answer_from_remote_peer_end:

    clib__unlock(&g_webrtc_muggles_rwlock_guard);
}

/**
 * @brief this function tries to initialize structure that represents webrtc datachannel connection that server creates with client.
 *
 *        on a re-init (the client retries the setup every 10 seconds until it connects) the previous peer
 *        connection and data channel are detached from the slot and deleted on a detached teardown thread,
 *        so their orphaned callbacks cannot wipe the slot's current state.
 *
 * @param client_t* client -> the client to set up the webrtc peer connection for
 *
 * @attention this function assumes that client pointer is used within readlock or write lock, and its safe to read it.
 *
 * @note like websocket, webrtc also works between client and server in this chat application, its not peer to peer. technically it is, but the peer is server.
 * @note takes the g_webrtc_muggles_rwlock_guard write lock while the slot is rebuilt, and releases it before the teardown thread is spawned
 *
 * @return boole -> TRUE when the peer connection and the data channel were created, FALSE when the client is missing, is not existing / not authenticated, or the peer connection could not be created
 */
boole audio_channel__initialize_webrtc_datachannel_connection(client_t* client)
{
    webrtc_peer_t* peer = 0;
    boole result = FALSE;
    rtcConfiguration config;
    rtcDataChannelInit init;
    int old_peer_connection_handle = 0;
    int old_data_channel_handle = 0;
    int failed_peer_connection_handle = 0;
    const char* iceServers[1] = { "127.0.0.1:3478" }; // using our own stun server! (violet)

    DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__initialize_webrtc_datachannel_connection \n");

    // these two run before the muggles lock is taken, so they must return directly - the shared
    // exit label unlocks, and unlocking a lock that was never taken is undefined behavior
    if (client == NULL_POINTER)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "client == NULL_POINTER");
        return FALSE;
    }

    if (client->is_existing == FALSE || client->is_authenticated == FALSE)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "client->is_existing == FALSE || client->is_authenticated == FALSE");
        return FALSE;
    }

    // rtcInitLogger(RTC_LOG_VERBOSE, NULL); If something doesn't work, uncomment!

    clib__null_memory(&config, sizeof(rtcConfiguration));

    // const char* iceServers[1] = { "stun:stun.l.google.com:19302" };
    config.iceServers = iceServers;
    config.iceServersCount = 1;

    clib__write_lock(&g_webrtc_muggles_rwlock_guard);

    peer = &g_webrtc_muggles_array[client->client_id];

    // a re-init (the client retries the datachannel setup every 10 seconds until it connects) must not
    // leak the previous libdatachannel objects: their callbacks stayed registered with this slot's address
    // as user pointer, and when such an orphan later failed or closed, its callbacks wiped the slot's
    // CURRENT state - silencing the client in every relay loop forever. detach the orphans from the slot
    // (NULL user pointer; every callback null-checks it) and delete them on the detached teardown thread
    // below, exactly like the disconnect path does
    old_peer_connection_handle = peer->peer_connection_handle;
    old_data_channel_handle = peer->data_channel_handle;

    if (old_data_channel_handle != 0)
    {
        rtcSetUserPointer(old_data_channel_handle, NULL_POINTER);
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
    // already joined a channel was left with a peer parked in the root channel and every relayed frame
    // was skipped on the channel-mismatch check
    peer->channel_id = client->channel_id;
    peer->is_sending_audio_right_now = FALSE;
    peer->data_channel_handle = 0;
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
        goto label_audio_channel__initialize_webrtc_datachannel_connection_end;
    }

    rtcSetUserPointer(peer->peer_connection_handle, peer); // binds created peer connection to my own custom struct. Nice that this library has this
    rtcSetLocalDescriptionCallback(peer->peer_connection_handle, peerconnection_on_setlocaldescription_callback);
    rtcSetLocalCandidateCallback(peer->peer_connection_handle, peerconnection_on_icecandidate_callback); // gets called when ICE candidate is returned from STUN server
    rtcSetStateChangeCallback(peer->peer_connection_handle, peerconnection_on_statechanged_callback); // gets called when peer connection state is changed
    rtcSetGatheringStateChangeCallback(peer->peer_connection_handle, peerconnection_on_gatheringstatechanged_callback);
    rtcSetDataChannelCallback(peer->peer_connection_handle, peerconnection_on_datachannel_callback);

    DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__initialize_webrtc_datachannel_connection got here \n");

    // clear out structure or initialization fails only unreliable needs to be set

    clib__null_memory(&init, sizeof(rtcDataChannelInit));
    init.reliability.unreliable = TRUE;
    init.reliability.unordered = TRUE;
    init.reliability.maxRetransmits = 0;

    // name of datachannel is testQQQ

    DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s", "peer->data_channel_handle", peer->data_channel_handle, "\n");
    DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s", "audio_channel__initialize_webrtc_datachannel_connection peer->data_channel_handle", peer->data_channel_handle, "\n");

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
        goto label_audio_channel__initialize_webrtc_datachannel_connection_end;
    }

    result = TRUE;

    rtcSetOpenCallback(peer->data_channel_handle, datachannel_on_open_callback);
    rtcSetClosedCallback(peer->data_channel_handle, datachannel_on_closed_callback);
    rtcSetMessageCallback(peer->data_channel_handle, datachannel_on_message_callback);
    rtcSetErrorCallback(peer->data_channel_handle, datachannel_onerror_callback);

label_audio_channel__initialize_webrtc_datachannel_connection_end:
    clib__unlock(&g_webrtc_muggles_rwlock_guard);

    // delete the detached orphans on the teardown thread; deleting inline would deadlock (rtcDelete*
    // blocks on libdatachannel threads whose callbacks take the locks our callers hold)
    _audio_channel_internal__spawn_webrtc_teardown(old_peer_connection_handle, old_data_channel_handle);
    _audio_channel_internal__spawn_webrtc_teardown(failed_peer_connection_handle, 0);

    DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__initialize_webrtc_datachannel_connection end \n");

    return result;
}

/**
 * @brief relays one opus frame from a music bot to every webrtc peer that sits in the given channel.
 *
 *        every receiver gets its own copy, prefixed with [4B bot client id][2B sequence, little endian]
 *        in front of the opus payload. nothing is relayed when the server wide music bot audio switch is
 *        off, or when the channel does not exist or has its audio disabled.
 *
 * @param uint64 sender_music_bot_client_id -> client id of the music bot the frame came from, written into the frame header so a receiver can feed each bot its own decoder
 * @param uint64 channel_id -> the channel whose peers receive the frame
 * @param uint64 sequence_number -> frame counter, only its low 16 bits are sent, letting receivers reorder frames that the unordered datachannel scrambled and detect losses
 * @param unsigned char* data -> the opus payload to relay
 * @param int data_length -> length of the opus payload in bytes
 *
 * @note takes the g_webrtc_muggles_rwlock_guard read lock for the whole relay loop
 *
 * @return void
 */
void audio_channel__send_music_bot_data(uint64 sender_music_bot_client_id, uint64 channel_id, uint64 sequence_number, unsigned char* data, int data_length)
{
    uint64 i = 0;
    void* message1 = NULL_POINTER;
 
    // log_info("%s", "audio_channel__send_music_bot_data called");

    // printf("%s %s", "datachannel_on_message_callback" , "\n");
    webrtc_peer_t* peer_receiver = 0;
    int dc = 0;

    // music bot audio has its own server-wide switch (is_music_bot_audio_active), independent of client
    // voice (is_voice_chat_active), so an admin can run "music bots only" with client voice off. it still
    // respects the per-channel audio toggle, like the client relay does
    if (g_server_settings.is_music_bot_audio_active == FALSE
        || g_channel_array[channel_id].is_existing == FALSE
        || g_channel_array[channel_id].is_audio_enabled == FALSE)
    {
        return;
    }

    clib__read_lock(&g_webrtc_muggles_rwlock_guard);

    // printf("Message %s: [binary of size %d]\n", "offerer", size);

    // sent audio data to other clients located in same channel as found client, if they too have authenticated audio websocket

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        message1 = NULL_POINTER;

        peer_receiver = &g_webrtc_muggles_array[i];

        if (peer_receiver->is_existing == FALSE)
        {
            continue;
        }
        else if (peer_receiver->channel_id != channel_id)
        {
            continue;
        }

        message1 = (void*)memorymanager__allocate(data_length + 7, MEMALLOC_MUSICBOT_AUDIOCHANNEL_ONMESSAGE);
        if (message1 == NULL_POINTER)
        {
            continue;
        }
        // frame layout: [4B bot client id][2B sequence, little endian][opus].
        // the real client id (not the old constant -2) lets a receiver with several bots in the channel
        // feed each bot its own decoder; the client recognizes bot senders by the is_music_bot flag from
        // the client list and skips voice decryption for them. the sequence lets receivers reorder frames
        // the unordered datachannel scrambled and detect losses
        ((int*)message1)[0] = (int)sender_music_bot_client_id;
        ((unsigned char*)message1)[4] = (unsigned char)(sequence_number & 0xff);
        ((unsigned char*)message1)[5] = (unsigned char)((sequence_number >> 8) & 0xff);
        clib__copy_memory((void*)data, ((unsigned char*)message1 + 6), data_length, data_length);

        // log_info("%s %d %s", "sending  ", data_length, "bytes of data \n");

        rtcSendMessage(peer_receiver->data_channel_handle, message1, data_length + 6);

        memorymanager__free((nuint)message1);
    }
    clib__unlock(&g_webrtc_muggles_rwlock_guard);
}