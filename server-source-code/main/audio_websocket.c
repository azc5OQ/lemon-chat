// This file lets Lemon Chat clients send and receive audio through the /audio WebSocket.
// It creates a ticket and a secret key, checks the client's first frame, and connects the socket
// to the correct logged-in client. It also limits incoming traffic, queues outgoing packets,
// and closes the connection when the client leaves or the server disables it.
//
// The first frame contains a byte set to 1, a four-byte client id, a 16-byte ticket, and a
// 32-byte HMAC-SHA256 code calculated from the first 21 bytes using the session key. The reply
// contains a byte set to 2, the ticket, and a code calculated from those first 17 bytes.
// Both sides check these codes to confirm that the socket belongs to the requested session.
// Tickets expire after 15 seconds and may be used by only one socket.
//
// Received packets go to audio_channel.c, which sends them to the other clients in the channel.
// audio_webrtc.c provides the WebRTC connection for the same kind of packets.
#include "definitions.h"
#include "../third-party/dave-g-json/cJSON.h"
#include "base.h"
#include "audio_channel.h"
#include "audio_webrtc.h"
#include "audio_websocket.h"
#include "metadata_crypto.h"
#include "memory_manager.h"
#include "../third-party/mbedtls-3.6.6/include/mbedtls/md.h"
#include "../third-party/mbedtls-3.6.6/include/mbedtls/platform_util.h"
#include <string.h>
#include "media_auth.h"
#include "server_message.h"
#include "../third-party/rxi-log/log.h"

// voice is 50 frames per second at most; this limit only stops a flood
#define AUDIO_WEBSOCKET_MAX_FRAMES_PER_SECOND 250

// Keep the ticket, authentication key, and frame count with this client's socket.
typedef struct audio_websocket_session
{
    ws_cli_conn_t* socket;            // the attached socket; NULL_POINTER until the proof frame arrived
    unsigned char auth_key[32];       // the key of this session; the proof frame and the acknowledgement are checked with it
    unsigned char ticket[16];         // the one-time ticket of this session
    mbedtls_md_context_t receive_mac; // the HMAC-SHA256 state set up with auth_key
    uint64 expires_at;                // the proof frame must arrive before this time
    uint64 requested_at;              // when the session was issued; a client gets one per 3 seconds at most
    uint64 rate_window;               // start of the current one-second window in which received frames are counted
    uint32_t rate_count;              // frames received in that window
} audio_websocket_session_t;

static uint32_t _audio_websocket_internal__read_u32(const unsigned char* bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | bytes[3];
}

static boole _audio_websocket_internal__authentication_code(mbedtls_md_context_t* context, const unsigned char* data, size_t size, unsigned char* out_code)
{
    return mbedtls_md_hmac_reset(context) == 0
        && mbedtls_md_hmac_update(context, data, size) == 0
        && mbedtls_md_hmac_finish(context, out_code) == 0;
}

static boole _audio_websocket_internal__is_equal(const unsigned char* left, const unsigned char* right, size_t size)
{
    unsigned char difference = 0;
    size_t i = 0;

    for (i = 0; i < size; i++)
    {
        difference |= left[i] ^ right[i];
    }
    return difference == 0;
}

// Send the ticket and its secret key through the client's encrypted chat connection.
// The caller holds the clients write lock while this replaces the previous session.
static boole _audio_websocket_internal__issue_session(client_t* client)
{
    audio_websocket_session_t** slot = &client->audio_websocket;
    audio_websocket_session_t* session = *slot;
    uint64 now = base__get_timestamp_ms();
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    char* plaintext = NULL_POINTER;
    char* encrypted = NULL_POINTER;
    int64 allocated = 0;
    char key_hex[65] = {0};
    char ticket_hex[33] = {0};
    size_t i = 0;
    boole is_issued = FALSE;

    // a client that asks again and again gets one session per 3 seconds at most
    if (session != NULL_POINTER && now - session->requested_at < 3000)
    {
        DBG_AUDIO_WEBSOCKET log_info("%s %s %llu %s", "audio", "session request of client", client->client_id, "ignored: the last one is under 3 s old");
        return FALSE;
    }
    // a new session replaces the old one: the old socket, when there is one, is closed
    audio_websocket__disconnect(client);
    session = calloc(1, sizeof(*session));
    if (session == NULL_POINTER)
    {
        return FALSE;
    }
    *slot = session;
    session->requested_at = now;
    session->expires_at = now + 15000;
    // the ticket and the key are random; the HMAC context is set up with the key once, for the proof
    // frame and the acknowledgement
    if (base__fill_secure_random_bytes(session->auth_key, sizeof(session->auth_key)) == FALSE
        || base__fill_secure_random_bytes(session->ticket, sizeof(session->ticket)) == FALSE
        || mbedtls_md_setup(&session->receive_mac, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1) != 0
        || mbedtls_md_hmac_starts(&session->receive_mac, session->auth_key, 32) != 0)
    {
        audio_websocket__disconnect(client);
        return FALSE;
    }
    // the client gets both values as hex text inside an encrypted chat message
    for (i = 0; i < sizeof(session->auth_key); i++)
    {
        snprintf(key_hex + i * 2, 3, "%02x", session->auth_key[i]);
    }
    for (i = 0; i < sizeof(session->ticket); i++)
    {
        snprintf(ticket_hex + i * 2, 3, "%02x", session->ticket[i]);
    }
    root = cJSON_CreateObject();
    message = cJSON_AddObjectToObject(root, "message");
    if (message != NULL_POINTER)
    {
        cJSON_AddStringToObject(message, "type", "audio_websocket_session");
        cJSON_AddNumberToObject(message, "client_id", (double)client->client_id);
        cJSON_AddStringToObject(message, "auth_key", key_hex);
        cJSON_AddStringToObject(message, "ticket", ticket_hex);
        plaintext = cJSON_PrintUnformatted(root);
    }
    if (plaintext != NULL_POINTER)
    {
        encrypted = metadata_crypto__encrypt_with_secret(plaintext, &allocated, client->dh_shared_secret);
        if (encrypted != NULL_POINTER)
        {
            ws_sendframe_txt(client->p_ws_connection, encrypted);
            memorymanager__free((nuint)encrypted);
            is_issued = TRUE;
            DBG_AUDIO_WEBSOCKET log_info("%s %s %llu", "audio", "websocket session issued to client", client->client_id);
        }
        // the key was in the plaintext: wiped before the memory is given back
        mbedtls_platform_zeroize(plaintext, strlen(plaintext));
        cJSON_free(plaintext);
    }
    cJSON_Delete(root);
    mbedtls_platform_zeroize(key_hex, sizeof(key_hex));
    return is_issued;
}

// Check the first frame before attaching this socket to a logged-in client.
// The caller holds the clients write lock, so a ticket cannot attach two sockets.
static uint64 _audio_websocket_internal__attach(ws_cli_conn_t* socket, const unsigned char* data, uint64 size, uint64 now)
{
    unsigned char code[32] = {0};
    unsigned char acknowledgement[49] = {0};
    audio_websocket_session_t* session = NULL_POINTER;
    client_t* client = NULL_POINTER;
    uint64 id = 0;

    if (size != 53 || data[0] != 1)
    {
        return UINT64_MAX;
    }
    id = _audio_websocket_internal__read_u32(data + 1);
    if (id >= g_server_settings.max_client_count)
    {
        return UINT64_MAX;
    }
    client = &g_clients_array[id];
    session = client->audio_websocket;
    // the client must be logged in and not idle, must have asked for a session that has not expired and
    // has no socket yet, and the ticket and the authentication code must be the ones of that session
    if (client->is_existing == FALSE || client->is_authenticated == FALSE || client->is_idle == TRUE
        || session == NULL_POINTER || session->socket != NULL_POINTER || now > session->expires_at
        || _audio_websocket_internal__is_equal(session->ticket, data + 5, 16) == FALSE
        || _audio_websocket_internal__authentication_code(&session->receive_mac, data, 21, code) == FALSE
        || _audio_websocket_internal__is_equal(code, data + 21, 32) == FALSE)
    {
        DBG_AUDIO_WEBSOCKET log_info("%s %s %llu %s", "audio", "proof frame for client", id, "refused: no session, wrong ticket or code, expired, idle, or already attached");
        return UINT64_MAX;
    }
    // the acknowledgement: the ticket and an authentication code over it, so the client knows the server
    // that answers is the one that issued the session
    acknowledgement[0] = 2;
    memcpy(acknowledgement + 1, session->ticket, 16);
    if (_audio_websocket_internal__authentication_code(&session->receive_mac, acknowledgement, 17, acknowledgement + 17) == FALSE)
    {
        return UINT64_MAX;
    }
    session->socket = socket;
    ws_media_bind(socket, id);
    client->is_audio_websocket_authenticated = TRUE;
    if (ws_media_queue_frame(socket, acknowledgement, sizeof(acknowledgement), WS_MEDIA_KIND_CONTROL) != 0)
    {
        return UINT64_MAX;
    }
    DBG_AUDIO_WEBSOCKET log_info("%s %s %llu", "audio", "websocket attached for client", id);
    return id;
}

static boole _audio_websocket_internal__is_frame_allowed(audio_websocket_session_t* session, uint64 now, uint32_t limit_per_second)
{
    // a one second window: when the current window is older than a second a new one starts at zero
    if (now - session->rate_window >= 1000)
    {
        session->rate_window = now;
        session->rate_count = 0;
    }
    return ++session->rate_count <= limit_per_second;
}

/** @brief whether the server offers the audio socket at all: the websocket connections must be allowed and some audio must be on */
static boole _audio_websocket_internal__is_served(void)
{
    return g_server_settings.is_audio_over_websocket_allowed == TRUE
        && (g_server_settings.is_voice_chat_active == TRUE || g_server_settings.is_music_bot_audio_active == TRUE);
}

/** @brief puts one audio frame that is meant for this client into the queue of its audio socket; a music bot frame may wait longer in the queue than voice. Clients lock required. */
boole audio_websocket__send(client_t* client, const unsigned char* data, uint64 size, boole is_music)
{
    if (size < MEDIA_FRAME_OVERHEAD || size > WS_AUDIO_FRAME_MAX)
    {
        return FALSE;
    }
    if (client->audio_websocket == NULL_POINTER || client->audio_websocket->socket == NULL_POINTER)
    {
        return FALSE;
    }
    return ws_media_queue_frame(client->audio_websocket->socket, data, size, is_music == TRUE ? WS_MEDIA_KIND_MUSIC : WS_MEDIA_KIND_VOICE) == 0;
}

/** @brief whether this socket is the client's attached audio socket */
boole audio_websocket__owns_socket(client_t* client, ws_cli_conn_t* socket)
{
    return client->audio_websocket != NULL_POINTER && client->audio_websocket->socket == socket;
}

/** @brief the client asked for an audio socket: it gets a session over its chat connection when the socket is served and the client may have one. Clients write lock required. */
void audio_websocket__request(client_t* client)
{
    if (_audio_websocket_internal__is_served() == FALSE
        || client->is_authenticated == FALSE || client->is_dh_shared_secret_agreed_upon == FALSE
        || client->is_music_bot == TRUE || client->is_idle == TRUE)
    {
        DBG_AUDIO_WEBSOCKET log_info("%s %llu %s", "session request of client", client->client_id, "refused: websocket audio is not served, or the client is not authenticated, a music bot, or idle");
        return;
    }
    _audio_websocket_internal__issue_session(client);
}

/** @brief a socket connected to /audio: closed at once when the audio socket is not served */
void audio_websocket__onopen(ws_cli_conn_t* socket)
{
    if (_audio_websocket_internal__is_served() == FALSE)
    {
        DBG_AUDIO_WEBSOCKET log_info("%s", "an /audio socket opened while websocket audio is not served; closed");
        ws_abort_client(socket);
    }
}

/**
 * @brief a frame arrived on an audio socket
 *
 *        on a socket that is not attached yet the frame must be the proof frame, which ties the socket to a
 *        client; the client then counts as a listener. on an attached socket an empty frame is the client's
 *        answer to the server's keepalive and every other frame is a voice frame, which goes to the relay
 *
 * @param ws_cli_conn_t* socket -> the socket the frame arrived on
 * @param unsigned char* data -> the frame
 * @param uint64 size -> its length in bytes
 * @param int type -> the websocket frame type; only binary frames are accepted
 *
 * @return void
 */
void audio_websocket__onmessage(ws_cli_conn_t* socket, unsigned char* data, uint64 size, int type)
{
    uint64 id = ws_media_client_id(socket);
    audio_websocket_session_t* session = NULL_POINTER;
    client_t* client = NULL_POINTER;
    boole valid = FALSE;
    boole relay = FALSE;
    uint64 now = base__get_timestamp_ms();

    if (type != WS_FR_OP_BIN || size > WS_AUDIO_FRAME_MAX)
    {
        DBG_AUDIO_WEBSOCKET log_info("%s %llu %s", "audio frame of", size, "bytes refused: not binary or too large; socket closed");
        ws_abort_client(socket);
        return;
    }
    if (id == UINT64_MAX)
    {
        clib__write_lock(&g_clients_global_rwlock_guard);
        id = _audio_websocket_internal__attach(socket, data, size, now);
        if (id != UINT64_MAX)
        {
            valid = TRUE;
            client = &g_clients_array[id];
            // a client with an audio connection can hear the others: it is shown as a listener until it
            // reports its real state, which the client does right after the attach
            if (client->audio_state == AUDIO_STATE__AUDIO_COMPLETELY_DISABLED)
            {
                client->audio_state = AUDIO_STATE__PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS;
                server_msg__send_audio_state_of_client_to_all_clients(client, client->audio_state);
            }
        }
        clib__unlock(&g_clients_global_rwlock_guard);
        if (valid == FALSE)
        {
            ws_abort_client(socket);
        }
        return;
    }

    clib__read_lock(&g_clients_global_rwlock_guard);
    if (id >= g_server_settings.max_client_count || size == 1)
    {
        goto done;
    }
    client = &g_clients_array[id];
    session = client->audio_websocket;
    if (client->is_existing == FALSE || client->is_authenticated == FALSE
        || client->is_idle == TRUE || session == NULL_POINTER || session->socket != socket)
    {
        goto done;
    }
    if (_audio_websocket_internal__is_frame_allowed(session, now, AUDIO_WEBSOCKET_MAX_FRAMES_PER_SECOND) == FALSE)
    {
        DBG_AUDIO_WEBSOCKET log_info("%s %llu %s", "client", id, "sent more than 250 audio frames within a second; socket closed");
        goto done;
    }
    valid = TRUE;
    if (size > 0)
    {
        relay = TRUE;
    }
done:
    clib__unlock(&g_clients_global_rwlock_guard);
    if (valid == FALSE)
    {
        ws_abort_client(socket);
    }
    else if (relay == TRUE)
    {
        audio_channel__relay_voice(id, socket, 0, data, size);
    }
}

/**
 * @brief an audio socket closed
 *
 *        the session ends. when the client has no connected data channel either, it cannot hear anybody,
 *        and everybody is told that its audio is disabled
 *
 * @param ws_cli_conn_t* socket -> the socket that closed
 *
 * @return void
 */
void audio_websocket__onclose(ws_cli_conn_t* socket)
{
    uint64 id = ws_media_client_id(socket);
    client_t* client = NULL_POINTER;

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__read_lock(&g_webrtc_muggles_rwlock_guard);
    if (id < g_server_settings.max_client_count)
    {
        client = &g_clients_array[id];
        if (audio_websocket__owns_socket(client, socket) == TRUE)
        {
            DBG_AUDIO_WEBSOCKET log_info("%s %llu %s", "audio websocket of client", id, "closed");
            audio_websocket__disconnect(client);
            if (g_server_settings.is_audio_over_webrtc_allowed == FALSE
                || audio_webrtc__is_connected(id) == FALSE)
            {
                client->audio_state = AUDIO_STATE__AUDIO_COMPLETELY_DISABLED;
                server_msg__send_audio_state_of_client_to_all_clients(client, client->audio_state);
            }
        }
    }
    clib__unlock(&g_webrtc_muggles_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/** @brief ends the client's audio session and closes its audio socket, also before a client slot is reused. Clients write lock required. */
void audio_websocket__disconnect(client_t* client)
{
    audio_websocket_session_t** slot = &client->audio_websocket;
    audio_websocket_session_t* session = *slot;

    *slot = NULL_POINTER;
    client->is_audio_websocket_authenticated = FALSE;
    if (session == NULL_POINTER)
    {
        return;
    }
    if (session->socket != NULL_POINTER)
    {
        ws_abort_client(session->socket);
    }
    // the key lives in the session: wiped before the memory is given back
    mbedtls_md_free(&session->receive_mac);
    mbedtls_platform_zeroize(session, sizeof(*session));
    free(session);
}

/** @brief drops the audio frames waiting in the queue of the client's audio socket, used when the client changes channel */
void audio_websocket__clear_queue(client_t* client)
{
    if (client->audio_websocket != NULL_POINTER && client->audio_websocket->socket != NULL_POINTER)
    {
        ws_media_clear_queue(client->audio_websocket->socket);
    }
}
