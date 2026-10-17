// the shared part of the audio relay. a client's voice arrives over one of two connections, its
// audio websocket (audio_websocket.c) or its webrtc data channel (audio_webrtc.c), and leaves over
// whichever connection each receiver has: the websocket when one is attached, else the data channel.
// the music bot uses the same route. every frame is encrypted a second time on its way between a client
// and the server, with that client's own keys (media_auth.c explains why and how): here the sender's
// encryption comes off, and every receiver gets the frame encrypted with the receiver's keys. the policy
// helpers read the two "audio connections" settings, which govern the video websocket as well.
#include "definitions.h"
#include "metadata_crypto.h"

#include "../third-party/dave-g-json/cJSON.h" // needed by base.h
#include "base.h"

#include "../third-party/rxi-log/log.h"

#include "clib/clib_memory.h"
#include "clib/clib_string.h"

#include "memory_manager.h"
#include "server_message.h"

#include "audio_channel.h"
#include "audio_webrtc.h"
#include "audio_websocket.h"
#include "video_websocket.h"
#include "media_auth.h"

// declarations
static void _audio_channel_internal__relay(uint64 sender_id, uint64 channel_id, const unsigned char* body, uint64 body_size, boole is_music);

/**
 * @brief Returns the connection choice shown in the server settings.
 *
 * @return const char* -> static text: "both", "websocket", "webrtc", or "none"; do not free it.
 */
const char* audio_channel__policy_value(void)
{
    if (g_server_settings.is_audio_over_websocket_allowed == TRUE && g_server_settings.is_audio_over_webrtc_allowed == TRUE)
    {
        return "both";
    }
    if (g_server_settings.is_audio_over_websocket_allowed == TRUE)
    {
        return "websocket";
    }
    if (g_server_settings.is_audio_over_webrtc_allowed == TRUE)
    {
        return "webrtc";
    }
    return "none";
}

/**
 * @brief applies a changed "audio connections" setting to the connected clients
 *
 *        a switched-off websocket ends every attached audio socket. a client that no longer has an
 *        allowed audio connection is reported as audio disabled, so the others stop expecting voice
 *        from it. the clients learn the new policy from the broadcast that follows and reconnect
 *        over what is still allowed.
 *
 * @attention takes the clients write lock and the peers read lock
 *
 * @return void
 */
void audio_channel__apply_policy_change(void)
{
    uint64 i = 0;
    client_t* client = NULL_POINTER;
    boole is_served = FALSE;
    boole is_audio_on = (g_server_settings.is_voice_chat_active == TRUE || g_server_settings.is_music_bot_audio_active == TRUE);

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__read_lock(&g_webrtc_peers_rwlock_guard);
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];
        if (client->is_existing == FALSE || client->is_authenticated == FALSE || client->is_music_bot == TRUE)
        {
            continue;
        }
        if ((g_server_settings.is_audio_over_websocket_allowed == FALSE || is_audio_on == FALSE) && client->audio_websocket != NULL_POINTER)
        {
            DBG_AUDIO_CHANNEL log_info("%s %llu %s", "policy change: audio websocket of client", i, "ended");
            audio_websocket__disconnect(client);
        }
        if (g_server_settings.is_audio_over_websocket_allowed == FALSE && client->video_websocket != NULL_POINTER)
        {
            DBG_AUDIO_CHANNEL log_info("%s %llu %s", "policy change: video websocket of client", i, "ended");
            video_websocket__disconnect(client);
        }
        is_served = is_audio_on == TRUE
            && (client->is_audio_websocket_authenticated == TRUE
                || (g_server_settings.is_audio_over_webrtc_allowed == TRUE && audio_webrtc__is_connected(i) == TRUE));
        if (is_served == FALSE && client->audio_state != AUDIO_STATE__AUDIO_COMPLETELY_DISABLED)
        {
            DBG_AUDIO_CHANNEL log_info("%s %llu %s", "policy change: client", i, "has no allowed audio connection left, reported as audio disabled");
            client->audio_state = AUDIO_STATE__AUDIO_COMPLETELY_DISABLED;
            server_msg__send_audio_state_of_client_to_all_clients(client, client->audio_state);
        }
    }
    clib__unlock(&g_webrtc_peers_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Encrypts an audio body separately for each listener and sends it through their audio connection.
 *
 * @param uint64 sender_id -> original sender, excluded from the receivers
 * @param uint64 channel_id -> channel whose clients may receive the audio
 * @param const unsigned char* body -> sender header and audio contents still protected by the channel keys
 * @param uint64 body_size -> number of bytes in body
 * @param boole is_music -> TRUE for music-bot audio, FALSE for voice
 *
 * @attention The caller holds the clients, WebRTC peers, and channels read locks.
 *
 * @return void
 */
static void _audio_channel_internal__relay(uint64 sender_id, uint64 channel_id, const unsigned char* body, uint64 body_size, boole is_music)
{
    unsigned char frame[WS_AUDIO_FRAME_MAX];
    uint64 frame_size = 0;
    uint64 i = 0;
    client_t* receiver = NULL_POINTER;

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        receiver = &g_clients_array[i];
        // a receiver reported as audio disabled gets nothing: it has no audio connection
        if (i == sender_id || receiver->is_existing == FALSE || receiver->is_authenticated == FALSE
            || receiver->is_idle == TRUE || receiver->is_music_bot == TRUE || receiver->channel_id != channel_id
            || receiver->audio_state == AUDIO_STATE__AUDIO_COMPLETELY_DISABLED)
        {
            continue;
        }
        frame_size = media_auth__encrypt_for_client(receiver, MEDIA_STREAM_AUDIO, body, body_size, frame, sizeof(frame));
        if (frame_size == 0)
        {
            continue;
        }
        if (receiver->is_audio_websocket_authenticated == TRUE)
        {
            audio_websocket__send(receiver, frame, frame_size, is_music);
        }
        else if (g_server_settings.is_audio_over_webrtc_allowed == TRUE)
        {
            audio_webrtc__send(i, frame, frame_size);
        }
    }
}

/**
 * @brief the shared voice route: a client's frame, from its websocket (socket set) or its data channel
 *        (data_channel set), has its authentication code checked and the sender's encryption removed, and is
 *        forwarded to the channel
 *
 *        the frame is relayed from either connection the server currently holds for the sender, so a
 *        handover between them loses nothing; a frame from a replaced socket or data channel of a reused
 *        slot is dropped
 *
 * @param uint64 sender_id -> slot of the sending client
 * @param ws_cli_conn_t* socket -> the audio websocket the frame arrived on, or NULL_POINTER
 * @param int data_channel -> the data channel handle the frame arrived on, 0 with a socket
 * @param const unsigned char* data -> the frame
 * @param uint64 size -> its length in bytes
 *
 * @return void
 */
void audio_channel__relay_voice(uint64 sender_id, ws_cli_conn_t* socket, int data_channel, const unsigned char* data, uint64 size)
{
    unsigned char body[AUDIO_PACKET_MAX];
    uint64 body_size = 0;
    client_t* sender = NULL_POINTER;
    uint64 channel_id = 0;

    if (sender_id >= g_server_settings.max_client_count || size <= MEDIA_FRAME_OVERHEAD || size > WS_AUDIO_FRAME_MAX)
    {
        return;
    }
    clib__read_lock(&g_clients_global_rwlock_guard);
    clib__read_lock(&g_webrtc_peers_rwlock_guard);
    clib__read_lock(&g_channels_global_rwlock_guard);
    sender = &g_clients_array[sender_id];
    channel_id = sender->channel_id;
    if (sender->is_existing == FALSE || sender->is_authenticated == FALSE || sender->is_idle == TRUE
        || sender->audio_state != AUDIO_STATE__PUSH_TO_TALK_ACTIVE
        || channel_id >= g_server_settings.max_channel_count
        || g_server_settings.is_voice_chat_active == FALSE
        || g_channel_array[channel_id].is_existing == FALSE || g_channel_array[channel_id].is_audio_enabled == FALSE)
    {
        DBG_AUDIO_CHANNEL log_info("%s %llu %s", "relay: frame of client", sender_id, "dropped: the client is not talking or is idle, voice chat is off, or its channel has no audio");
        goto done;
    }
    if (socket != NULL_POINTER)
    {
        if (g_server_settings.is_audio_over_websocket_allowed == FALSE || audio_websocket__owns_socket(sender, socket) == FALSE)
        {
            DBG_AUDIO_CHANNEL log_info("%s %llu %s", "relay: websocket frame of client", sender_id, "dropped: websocket audio is not allowed, or the socket is not the client's current one");
            goto done;
        }
    }
    else if (g_server_settings.is_audio_over_webrtc_allowed == FALSE
        || audio_webrtc__is_data_channel_current(sender_id, data_channel) == FALSE)
    {
        DBG_AUDIO_CHANNEL log_info("%s %llu %s", "relay: data channel frame of client", sender_id, "dropped: webrtc audio is not allowed, or the channel is not the client's current one");
        goto done;
    }
    body_size = media_auth__decrypt_from_client(sender, MEDIA_STREAM_AUDIO, data, size, body, sizeof(body));
    if (body_size == 0)
    {
        DBG_AUDIO_CHANNEL log_info("%s %llu %s", "relay: frame of client", sender_id, "dropped: the authentication code or the sender id inside did not match");
        goto done;
    }
    _audio_channel_internal__relay(sender_id, channel_id, body, body_size, FALSE);
done:
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_webrtc_peers_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief a client joined or switched a channel: queued audio of the old channel is dropped and the peer learns the channel
 *
 * @param client_t* client -> the client that changed channel
 *
 * @return void
 */
void audio_channel__process_client_channel_join(client_t* client)
{
    audio_websocket__clear_queue(client);
    audio_webrtc__process_client_channel_join(client);
}

/**
 * @brief a client disconnected: both of its audio connections are ended
 *
 * @param client_t* client -> the client that disconnected
 *
 * @attention clients write lock required (the websocket session is freed here)
 *
 * @return void
 */
void audio_channel__process_client_disconnect(client_t* client)
{
    audio_websocket__disconnect(client);
    audio_webrtc__process_client_disconnect(client);
}

/**
 * @brief relays one opus frame from a music bot to every client that is in the given channel.
 *
 *        the frame body is [4B bot client id][8B packet counter][2B sequence, little endian][opus]. the last two
 *        parts are encrypted with the bot's four keys and then with the server keys; the counter block of
 *        each of those layers is built from the bot id and the packet counter (media_auth.c). every
 *        receiver then gets its own copy, encrypted with the receiver's keys. nothing is relayed when the
 *        server wide music bot audio switch is off, or when the channel does not exist or has its audio
 *        disabled.
 *
 * @param uint64 sender_music_bot_client_id -> client id of the music bot the frame came from, written into the frame header so a receiver can feed each bot its own decoder
 * @param uint64 channel_id -> the channel whose peers receive the frame
 * @param uint64 sequence_number -> frame counter, only its low 16 bits are sent, letting receivers reorder frames that the unordered datachannel scrambled and detect losses
 * @param unsigned char* data -> the opus payload to relay
 * @param int data_length -> length of the opus payload in bytes
 *
 * @note takes the clients, peers and channels read locks for the whole relay loop
 *
 * @return void
 */
void audio_channel__send_music_bot_data(uint64 sender_music_bot_client_id, uint64 channel_id, uint64 sequence_number, unsigned char* data, int data_length)
{
    unsigned char body[AUDIO_PACKET_MAX] = {0};
    music_bot_audio_keys_t* audio_keys = NULL_POINTER;
    crypto_key_t bot_key = {0};
    uint64 counter = 0;
    uint64 i = 0;

    if (sender_music_bot_client_id >= g_server_settings.max_client_count
        || channel_id >= g_server_settings.max_channel_count || data_length <= 0
        || data_length > (int)sizeof(body) - 14)
    {
        return;
    }
    clib__read_lock(&g_clients_global_rwlock_guard);
    clib__read_lock(&g_webrtc_peers_rwlock_guard);
    clib__read_lock(&g_channels_global_rwlock_guard);
    if (g_server_settings.is_music_bot_audio_active == TRUE
        && g_channel_array[channel_id].is_existing == TRUE && g_channel_array[channel_id].is_audio_enabled == TRUE
        && g_clients_array[sender_music_bot_client_id].is_existing == TRUE
        && g_clients_array[sender_music_bot_client_id].is_music_bot == TRUE
        && g_clients_array[sender_music_bot_client_id].channel_id == channel_id)
    {
        audio_keys = g_clients_array[sender_music_bot_client_id].music_bot_client_extension.audio_keys;
        if (audio_keys == NULL_POINTER || audio_keys->packet_counter == UINT64_MAX)
        {
            goto done;
        }
        // the counter never resets between songs and starts at a random value: no counter block is reused
        counter = ++audio_keys->packet_counter;
        media_auth__write_body_header(body, sender_music_bot_client_id, counter);
        body[12] = (unsigned char)sequence_number;
        body[13] = (unsigned char)(sequence_number >> 8);
        memcpy(body + 14, data, data_length);
        for (i = 0; i < 4; i++)
        {
            // Music bots currently give clients four AES-256 keys. Mark each
            // key as that type before asking the encryption code to use it.
            if (!metadata_crypto__key_init(&bot_key, CRYPTO_KEY_AES_256_CTR, audio_keys->keys[i], sizeof(audio_keys->keys[i]))
                || !media_auth__apply_inner_layer(&bot_key, NULL_POINTER, sender_music_bot_client_id, counter, body + 12, data_length + 2))
            {
                goto done;
            }
        }
        if (!media_auth__apply_server_key_layers(sender_music_bot_client_id, counter, body + 12, data_length + 2))
        {
            goto done;
        }
        _audio_channel_internal__relay(sender_music_bot_client_id, channel_id, body, data_length + 14, TRUE);
    }
    else
    {
        DBG_AUDIO_CHANNEL log_info("%s %llu %s", "music bot frame of client", sender_music_bot_client_id, "dropped: bot audio is off, the channel is missing or has no audio, or the bot is not in it");
    }
done:
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_webrtc_peers_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}
