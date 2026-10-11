// This file lets Lemon Chat clients send and receive video through the /video WebSocket.
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
// Received packets go to video_stream.c, which checks the stream and sends them to its viewers.
// video_webrtc.c provides the WebRTC connection for the same kind of packets.
#include "definitions.h"
#include "../third-party/dave-g-json/cJSON.h"
#include "base.h"
#include "metadata_crypto.h"
#include "memory_manager.h"
#include "../third-party/mbedtls-3.6.6/include/mbedtls/md.h"
#include "../third-party/mbedtls-3.6.6/include/mbedtls/platform_util.h"
#include <string.h>
#include "media_auth.h"
#include "video_webrtc.h"
#include "video_stream.h"
#include "video_websocket.h"
#include "../third-party/rxi-log/log.h"

// a streamer's keyframe may arrive as a burst of up to 256 packets; this limit only stops a flood
#define VIDEO_WEBSOCKET_MAX_FRAMES_PER_SECOND 2000

// Keep the ticket, authentication key, and frame count with this client's socket.
typedef struct video_websocket_session
{
    ws_cli_conn_t* socket;            // the attached socket; NULL_POINTER until the proof frame arrived
    unsigned char auth_key[32];       // the key of this session; the proof frame and the acknowledgement are checked with it
    unsigned char ticket[16];         // the one-time ticket of this session
    mbedtls_md_context_t receive_mac; // the HMAC-SHA256 state set up with auth_key
    uint64 expires_at;                // the proof frame must arrive before this time
    uint64 requested_at;              // when the session was issued; a client gets one per 3 seconds at most
    uint64 rate_window;               // start of the current one-second window in which received frames are counted
    uint32_t rate_count;              // frames received in that window
} video_websocket_session_t;

static uint32_t _video_websocket_internal__read_u32(const unsigned char* bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | bytes[3];
}

static boole _video_websocket_internal__authentication_code(mbedtls_md_context_t* context, const unsigned char* data, size_t size, unsigned char* out_code)
{
    return mbedtls_md_hmac_reset(context) == 0
        && mbedtls_md_hmac_update(context, data, size) == 0
        && mbedtls_md_hmac_finish(context, out_code) == 0;
}

static boole _video_websocket_internal__is_equal(const unsigned char* left, const unsigned char* right, size_t size)
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
static boole _video_websocket_internal__issue_session(client_t* client)
{
    video_websocket_session_t** slot = &client->video_websocket;
    video_websocket_session_t* session = *slot;
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
        DBG_VIDEO_WEBSOCKET log_info("%s %s %llu %s", "video", "session request of client", client->client_id, "ignored: the last one is under 3 s old");
        return FALSE;
    }
    // a new session replaces the old one: the old socket, when there is one, is closed
    video_websocket__disconnect(client);
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
        video_websocket__disconnect(client);
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
        cJSON_AddStringToObject(message, "type", "video_websocket_session");
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
            DBG_VIDEO_WEBSOCKET log_info("%s %s %llu", "video", "websocket session issued to client", client->client_id);
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
static uint64 _video_websocket_internal__attach(ws_cli_conn_t* socket, const unsigned char* data, uint64 size, uint64 now)
{
    unsigned char code[32] = {0};
    unsigned char acknowledgement[49] = {0};
    video_websocket_session_t* session = NULL_POINTER;
    client_t* client = NULL_POINTER;
    uint64 id = 0;

    if (size != 53 || data[0] != 1)
    {
        return UINT64_MAX;
    }
    id = _video_websocket_internal__read_u32(data + 1);
    if (id >= g_server_settings.max_client_count)
    {
        return UINT64_MAX;
    }
    client = &g_clients_array[id];
    session = client->video_websocket;
    // the client must be logged in and not idle, must have asked for a session that has not expired and
    // has no socket yet, and the ticket and the authentication code must be the ones of that session
    if (client->is_existing == FALSE || client->is_authenticated == FALSE || client->is_idle == TRUE
        || session == NULL_POINTER || session->socket != NULL_POINTER || now > session->expires_at
        || _video_websocket_internal__is_equal(session->ticket, data + 5, 16) == FALSE
        || _video_websocket_internal__authentication_code(&session->receive_mac, data, 21, code) == FALSE
        || _video_websocket_internal__is_equal(code, data + 21, 32) == FALSE)
    {
        DBG_VIDEO_WEBSOCKET log_info("%s %s %llu %s", "video", "proof frame for client", id, "refused: no session, wrong ticket or code, expired, idle, or already attached");
        return UINT64_MAX;
    }
    // the acknowledgement: the ticket and an authentication code over it, so the client knows the server
    // that answers is the one that issued the session
    acknowledgement[0] = 2;
    memcpy(acknowledgement + 1, session->ticket, 16);
    if (_video_websocket_internal__authentication_code(&session->receive_mac, acknowledgement, 17, acknowledgement + 17) == FALSE)
    {
        return UINT64_MAX;
    }
    session->socket = socket;
    ws_media_bind(socket, id);
    client->is_video_websocket_authenticated = TRUE;
    if (ws_media_queue_frame(socket, acknowledgement, sizeof(acknowledgement), WS_MEDIA_KIND_CONTROL) != 0)
    {
        return UINT64_MAX;
    }
    DBG_VIDEO_WEBSOCKET log_info("%s %s %llu", "video", "websocket attached for client", id);
    return id;
}

static boole _video_websocket_internal__is_frame_allowed(video_websocket_session_t* session, uint64 now, uint32_t limit_per_second)
{
    // a one second window: when the current window is older than a second a new one starts at zero
    if (now - session->rate_window >= 1000)
    {
        session->rate_window = now;
        session->rate_count = 0;
    }
    return ++session->rate_count <= limit_per_second;
}

/** @brief whether the server offers the video socket at all: the websocket connections must be allowed and video streaming must be on */
static boole _video_websocket_internal__is_served(void)
{
    return g_server_settings.is_audio_over_websocket_allowed == TRUE && g_server_settings.is_video_streaming_active == TRUE;
}

/** @brief puts one video frame that is meant for this client into the queue of its video socket. Clients lock required. */
boole video_websocket__send(client_t* client, const unsigned char* frame, uint64 size)
{
    if (size < MEDIA_FRAME_OVERHEAD || size > WS_VIDEO_FRAME_MAX)
    {
        return FALSE;
    }
    if (client->video_websocket == NULL_POINTER || client->video_websocket->socket == NULL_POINTER)
    {
        return FALSE;
    }
    return ws_media_queue_frame(client->video_websocket->socket, frame, size, WS_MEDIA_KIND_VIDEO) == 0;
}

/** @brief whether this socket is the client's attached video socket */
boole video_websocket__owns_socket(client_t* client, ws_cli_conn_t* socket)
{
    return client->video_websocket != NULL_POINTER && client->video_websocket->socket == socket;
}

/** @brief the client asked for a video socket: it gets a session over its chat connection when the socket is served and the client may have one. Clients write lock required. */
void video_websocket__request(client_t* client)
{
    if (_video_websocket_internal__is_served() == FALSE
        || client->is_authenticated == FALSE || client->is_dh_shared_secret_agreed_upon == FALSE
        || client->is_music_bot == TRUE || client->is_idle == TRUE)
    {
        DBG_VIDEO_WEBSOCKET log_info("%s %llu %s", "video session request of client", client->client_id, "refused: websocket video is not served, or the client is not authenticated, a music bot, or idle");
        return;
    }
    _video_websocket_internal__issue_session(client);
}

/** @brief a socket connected to /video: closed at once when the video socket is not served */
void video_websocket__onopen(ws_cli_conn_t* socket)
{
    if (_video_websocket_internal__is_served() == FALSE)
    {
        DBG_VIDEO_WEBSOCKET log_info("%s", "a /video socket opened while websocket video is not served; closed");
        ws_abort_client(socket);
    }
}

/**
 * @brief a frame arrived on a video socket
 *
 *        on a socket that is not attached yet the frame must be the proof frame, which ties the socket to a
 *        client. on an attached socket an empty frame is the client's answer to the server's keepalive and
 *        every other frame is a video packet, which goes to the relay
 *
 * @param ws_cli_conn_t* socket -> the socket the frame arrived on
 * @param unsigned char* data -> the frame
 * @param uint64 size -> its length in bytes
 * @param int type -> the websocket frame type; only binary frames are accepted
 *
 * @return void
 */
void video_websocket__onmessage(ws_cli_conn_t* socket, unsigned char* data, uint64 size, int type)
{
    uint64 id = ws_media_client_id(socket);
    video_websocket_session_t* session = NULL_POINTER;
    client_t* client = NULL_POINTER;
    boole valid = FALSE;
    boole relay = FALSE;
    uint64 now = base__get_timestamp_ms();

    if (type != WS_FR_OP_BIN || size > WS_VIDEO_FRAME_MAX)
    {
        DBG_VIDEO_WEBSOCKET log_info("%s %llu %s", "video frame of", size, "bytes refused: not binary or too large; socket closed");
        ws_abort_client(socket);
        return;
    }
    if (id == UINT64_MAX)
    {
        clib__write_lock(&g_clients_global_rwlock_guard);
        valid = _video_websocket_internal__attach(socket, data, size, now) != UINT64_MAX;
        clib__unlock(&g_clients_global_rwlock_guard);
        if (valid == FALSE)
        {
            ws_abort_client(socket);
        }
        return;
    }

    clib__read_lock(&g_clients_global_rwlock_guard);
    if (id >= g_server_settings.max_client_count)
    {
        goto done;
    }
    client = &g_clients_array[id];
    session = client->video_websocket;
    if (client->is_existing == FALSE || client->is_authenticated == FALSE
        || client->is_idle == TRUE || session == NULL_POINTER || session->socket != socket)
    {
        goto done;
    }
    if (_video_websocket_internal__is_frame_allowed(session, now, VIDEO_WEBSOCKET_MAX_FRAMES_PER_SECOND) == FALSE)
    {
        DBG_VIDEO_WEBSOCKET log_info("%s %llu %s", "client", id, "sent more video frames within a second than allowed; socket closed");
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
        video_stream__relay_packet(id, socket, 0, data, size);
    }
}

/**
 * @brief a video socket closed
 *
 *        the session ends. when the client has no connected video data channel either, it has no video
 *        connection left: its stream ends when it was the streamer, or its viewing ends when it was a viewer
 *
 * @param ws_cli_conn_t* socket -> the socket that closed
 *
 * @return void
 */
void video_websocket__onclose(ws_cli_conn_t* socket)
{
    uint64 id = ws_media_client_id(socket);
    client_t* client = NULL_POINTER;

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__read_lock(&g_webrtc_muggles_rwlock_guard);
    clib__write_lock(&g_channels_global_rwlock_guard);
    if (id < g_server_settings.max_client_count)
    {
        client = &g_clients_array[id];
        if (video_websocket__owns_socket(client, socket) == TRUE)
        {
            DBG_VIDEO_WEBSOCKET log_info("%s %llu %s", "video websocket of client", id, "closed");
            video_websocket__disconnect(client);
            if (video_webrtc__is_connected(id) == FALSE)
            {
                video_stream__process_transport_lost(id);
            }
        }
    }
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_webrtc_muggles_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/** @brief ends the client's video session and closes its video socket. Clients write lock required. */
void video_websocket__disconnect(client_t* client)
{
    video_websocket_session_t** slot = &client->video_websocket;
    video_websocket_session_t* session = *slot;

    *slot = NULL_POINTER;
    client->is_video_websocket_authenticated = FALSE;
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

/** @brief ends every client's video session, used when video streaming or the websocket connections are switched off. Clients write lock required. */
void video_websocket__disconnect_all(void)
{
    uint64 i = 0;

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (g_clients_array[i].video_websocket != NULL_POINTER)
        {
            video_websocket__disconnect(&g_clients_array[i]);
        }
    }
}

/** @brief drops the video frames waiting in the queue of the client's video socket, used when the client changes channel */
void video_websocket__clear_queue(client_t* client)
{
    if (client->video_websocket != NULL_POINTER && client->video_websocket->socket != NULL_POINTER)
    {
        ws_media_clear_queue(client->video_websocket->socket);
    }
}
