#include "util.h"
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
// Received packets go to video_channel.c, which checks the stream and sends them to its viewers.
// video_webrtc.c provides the WebRTC connection for the same kind of packets.
#include "definitions.h"
#include "clib/clib.h"
#include "../third-party/dave-g-json/cJSON.h"
#include "base.h"
#include "crypto/crypto_metadata.h"
#include "msg/server/server_msg.h"
#include "memory_manager.h"
#include "../third-party/mbedtls-3.6.6/include/mbedtls/md.h"
#include "../third-party/mbedtls-3.6.6/include/mbedtls/platform_util.h"
#include <string.h>
#include "crypto/crypto_media.h"
#include "video_webrtc.h"
#include "core/video_channel.h"
#include "video_websocket.h"
#include "../third-party/rxi-log/log.h"

// a streamer's keyframe may arrive as a burst of up to 256 packets; this limit only stops a flood
#define VIDEO_WEBSOCKET_MAX_FRAMES_PER_SECOND 2000

// Keep the ticket, authentication key, and frame count with this client's socket.
typedef struct video_websocket_session
{
    ws_cli_conn_t *socket; // the attached socket; NULL_POINTER until the proof frame arrived
    ubyte auth_key[32]; // the key of this session; the proof frame and the acknowledgement are checked with it
    ubyte ticket[16]; // the one-time ticket of this session
    mbedtls_md_context_t receive_mac; // the HMAC-SHA256 state set up with auth_key
    uint64 expires_at; // the proof frame must arrive before this time
    uint64 requested_at; // when the session was issued; a client gets one per 3 seconds at most
    uint64 rate_window; // start of the current one-second window in which received frames are counted
    uint64 rate_count; // frames received in that window
} video_websocket_session_t;

// declarations
static uint64 _video_websocket_internal__read_u32(const ubyte *bytes);
static boole _video_websocket_internal__authentication_code(mbedtls_md_context_t *context, const ubyte *data, uint64 size, ubyte *out_code);
static boole _video_websocket_internal__is_equal(const ubyte *left, const ubyte *right, uint64 size);
static boole _video_websocket_internal__issue_session(client_t *client);
static uint64 _video_websocket_internal__attach(ws_cli_conn_t *socket, const ubyte *data, uint64 size, uint64 now);
static boole _video_websocket_internal__is_frame_allowed(video_websocket_session_t *session, uint64 now, uint64 limit_per_second);
static boole _video_websocket_internal__is_served(void);

/**
 * @brief Reads a four-byte number with the most significant byte first.
 *
 * @param const ubyte* bytes -> buffer containing at least four bytes
 *
 * @return uint64 -> number stored in those bytes.
 */
static uint64 _video_websocket_internal__read_u32(const ubyte *bytes)
{
    return ((uint64)bytes[0] << 24) | ((uint64)bytes[1] << 16) | ((uint64)bytes[2] << 8) | bytes[3];
}

/**
 * @brief Calculates the HMAC-SHA256 check value with the session key already set in the context.
 *
 * @param mbedtls_md_context_t* context -> initialized HMAC context, reused for this calculation
 * @param const ubyte* data -> bytes to check
 * @param uint64 size -> number of bytes in data
 * @param ubyte* out_code -> buffer with room for the 32-byte result
 *
 * @attention Do not use the same context concurrently from another thread.
 *
 * @return boole -> TRUE when the check value was written; FALSE if an HMAC operation fails.
 */
static boole _video_websocket_internal__authentication_code(mbedtls_md_context_t *context, const ubyte *data, uint64 size, ubyte *out_code)
{
    if (mbedtls_md_hmac_reset(context) != 0)
    {
        return FALSE;
    }
    if (mbedtls_md_hmac_update(context, data, size) != 0)
    {
        return FALSE;
    }
    return mbedtls_md_hmac_finish(context, out_code) == 0;
}

/**
 * @brief Compares every byte of two buffers without stopping at the first difference.
 *
 * @param const ubyte* left -> first buffer, readable for size bytes
 * @param const ubyte* right -> second buffer, readable for size bytes
 * @param uint64 size -> number of bytes to compare
 *
 * @return boole -> TRUE when all bytes match, including an empty comparison; otherwise FALSE.
 */
static boole _video_websocket_internal__is_equal(const ubyte *left, const ubyte *right, uint64 size)
{
    ubyte difference = 0;
    uint64 i = 0;

    for (i = 0; i < size; i++)
    {
        difference |= left[i] ^ right[i];
    }
    return difference == 0;
}

/**
 * @brief Creates a ticket and secret key for the client's video WebSocket, then queues those
 *        details for delivery through the client's encrypted chat connection.
 *
 * @param client_t* client -> client requesting a new video WebSocket session
 *
 * @attention The caller must hold the clients write lock while this function replaces the previous session and copies the offer into the outgoing queue.
 *
 * @return boole -> TRUE when the session offer was queued; FALSE when the request is too frequent or the offer could not be prepared or queued
 */
static boole _video_websocket_internal__issue_session(client_t *client)
{
    video_websocket_session_t **slot = &client->video_websocket;
    video_websocket_session_t *session = *slot;
    uint64 now = util__get_timestamp_ms();
    cJSON *root = NULL_POINTER;
    cJSON *message = NULL_POINTER;
    char *plaintext = NULL_POINTER;
    char key_hex[65] = { 0 };
    char ticket_hex[33] = { 0 };
    uint64 i = 0;
    boole is_issued = FALSE;
    boole status = FALSE;

    // a client that asks again and again gets one session per 3 seconds at most
    if (session != NULL_POINTER)
    {
        status = now - session->requested_at < 3000;
    }
    if (status == TRUE)
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
    status = (((util__fill_secure_random_bytes(session->auth_key, sizeof(session->auth_key))) && (util__fill_secure_random_bytes(session->ticket, sizeof(session->ticket)))) && (mbedtls_md_setup(&session->receive_mac, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1) == 0)) && (mbedtls_md_hmac_starts(&session->receive_mac, session->auth_key, 32) == 0);
    if (status == FALSE)
    {
        video_websocket__disconnect(client);
        return FALSE;
    }

    // the client gets both values as hex text inside an encrypted chat message
    for (i = 0; i < sizeof(session->auth_key); i++)
    {
        clib__string_format(key_hex + i * 2, 3, "%02x", session->auth_key[i]);
    }
    for (i = 0; i < sizeof(session->ticket); i++)
    {
        clib__string_format(ticket_hex + i * 2, 3, "%02x", session->ticket[i]);
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
        is_issued = server_msg__delivery__queue_text_to_client(client, plaintext);
        if (is_issued == TRUE)
        {
            DBG_VIDEO_WEBSOCKET log_info("%s %s %llu", "video", "websocket session issued to client", client->client_id);
        }

        // the key was in the plaintext: set to zero before the memory is given back
        mbedtls_platform_zeroize(plaintext, clib__strlen(plaintext));
        cJSON_free(plaintext);
    }
    cJSON_Delete(root);
    mbedtls_platform_zeroize(key_hex, sizeof(key_hex));
    return is_issued;
}

/**
 * @brief Checks the first frame and attaches the socket to the client that owns its ticket.
 *
 * @param ws_cli_conn_t* socket -> newly opened video WebSocket
 * @param const ubyte* data -> first frame containing the client ID, ticket, and check value
 * @param uint64 size -> number of bytes in the first frame
 * @param uint64 now -> current timestamp in milliseconds, used to check ticket expiry
 *
 * @attention The caller holds the clients write lock. If reply queueing fails after attachment, the caller must close the socket.
 *
 * @return uint64 -> attached client ID, or UINT64_MAX if the proof or reply queueing fails.
 */
static uint64 _video_websocket_internal__attach(ws_cli_conn_t *socket, const ubyte *data, uint64 size, uint64 now)
{
    ubyte code[32] = { 0 };
    ubyte acknowledgement[49] = { 0 };
    video_websocket_session_t *session = NULL_POINTER;
    client_t *client = NULL_POINTER;
    uint64 id = 0;

    // Require the complete proof packet before reading its fields.
    if (size != 53)
    {
        return UINT64_MAX;
    }

    // Accept the session proof packet type only.
    if (data[0] != 1)
    {
        return UINT64_MAX;
    }

    // Read the client ID and check it before indexing the clients array.
    id = _video_websocket_internal__read_u32(data + 1);
    if (id >= g_server_settings.max_client_count)
    {
        return UINT64_MAX;
    }
    client = &g_clients_array[id];
    session = client->video_websocket;

    // the client must be logged in and not idle, must have asked for a session that has not expired and
    // has no socket yet, and the ticket and the authentication code must be the ones of that session
    if (client->is_existing == FALSE || client->is_authenticated == FALSE || client->is_idle == TRUE || session == NULL_POINTER)
    {
        goto rejected;
    }

    // Reject expired tickets and sessions already attached to another socket.
    if (session->socket != NULL_POINTER || now > session->expires_at)
    {
        goto rejected;
    }

    // Require the ticket issued to this client.
    if (_video_websocket_internal__is_equal(session->ticket, data + 5, 16) == FALSE)
    {
        goto rejected;
    }

    // Calculate the expected authentication code for the packet.
    if (_video_websocket_internal__authentication_code(&session->receive_mac, data, 21, code) == FALSE)
    {
        goto rejected;
    }

    // Reject a proof whose authentication code does not match.
    if (_video_websocket_internal__is_equal(code, data + 21, 32) == FALSE)
    {
        goto rejected;
    }

    // the acknowledgement: the ticket and an authentication code over it, so the client knows the server
    // that answers is the one that issued the session
    acknowledgement[0] = 2;
    clib__memcpy(acknowledgement + 1, session->ticket, 16);
    if (_video_websocket_internal__authentication_code(&session->receive_mac, acknowledgement, 17, acknowledgement + 17) == FALSE)
    {
        return UINT64_MAX;
    }

    // Attach the verified socket and queue the acknowledgement.
    session->socket = socket;
    ws_media_bind(socket, id);
    client->is_video_websocket_authenticated = TRUE;
    if (ws_media_queue_frame(socket, acknowledgement, sizeof(acknowledgement), WS_MEDIA_KIND_CONTROL) != 0)
    {
        return UINT64_MAX;
    }
    DBG_VIDEO_WEBSOCKET log_info("%s %s %llu", "video", "websocket attached for client", id);
    return id;

rejected:
    DBG_VIDEO_WEBSOCKET log_info("%s %s %llu %s", "video", "proof frame for client", id, "refused: no session, wrong ticket or code, expired, idle, or already attached");
    return UINT64_MAX;
}

/**
 * @brief Counts the incoming frame and checks the session's one-second limit.
 *
 * @param video_websocket_session_t* session -> session whose frame count and time window are updated
 * @param uint64 now -> current timestamp in milliseconds
 * @param uint64 limit_per_second -> maximum frames allowed in one time window
 *
 * @attention Only this socket's reader may update the session counters.
 *
 * @return boole -> TRUE when this frame is within the limit; otherwise FALSE.
 */
static boole _video_websocket_internal__is_frame_allowed(video_websocket_session_t *session, uint64 now, uint64 limit_per_second)
{
    // a one second window: when the current window is older than a second a new one starts at zero
    if (now - session->rate_window >= 1000)
    {
        session->rate_window = now;
        session->rate_count = 0;
    }
    return ++session->rate_count <= limit_per_second;
}

/**
 * @brief Checks whether WebSocket connections and video streaming are enabled.
 *
 * @return boole -> TRUE when the server offers the video WebSocket; otherwise FALSE.
 */
static boole _video_websocket_internal__is_served(void)
{
    return g_server_settings.is_audio_over_websocket_allowed == TRUE && g_server_settings.is_video_streaming_active == TRUE;
}

/**
 * @brief Queues one video packet for the receiving client's attached WebSocket.
 *
 * @param client_t* client -> receiving client
 * @param const ubyte* frame -> packet to copy into the socket queue
 * @param uint64 size -> packet length in bytes
 *
 * @attention The caller holds the clients lock. The caller keeps frame and may reuse it after this call.
 *
 * @return boole -> TRUE when the packet was queued; FALSE if its size, socket, or queue is invalid.
 */
boole video_websocket__send(client_t *client, const ubyte *frame, uint64 size)
{
    if (size < MEDIA_FRAME_OVERHEAD || size > WS_VIDEO_FRAME_MAX)
    {
        return FALSE;
    }
    if (client->video_websocket == NULL_POINTER)
    {
        return FALSE;
    }
    if (client->video_websocket->socket == NULL_POINTER)
    {
        return FALSE;
    }
    return ws_media_queue_frame(client->video_websocket->socket, frame, size, WS_MEDIA_KIND_VIDEO) == 0;
}

/**
 * @brief Checks whether the session stores the supplied video socket.
 *
 * @param client_t* client -> client whose session is inspected
 * @param ws_cli_conn_t* socket -> socket pointer to compare with the session
 *
 * @attention The caller holds the clients lock.
 *
 * @return boole -> TRUE when the session exists and stores this pointer; otherwise FALSE.
 */
boole video_websocket__owns_socket(client_t *client, ws_cli_conn_t *socket)
{
    boole status = FALSE;

    if (client->video_websocket != NULL_POINTER)
    {
        status = client->video_websocket->socket == socket;
    }
    return status;
}

/**
 * @brief Issues video connection details when the server policy and client state allow it.
 *
 * @param client_t* client -> authenticated, active client requesting a video WebSocket
 *
 * @attention The caller holds the clients write lock.
 *
 * @return void
 */
void video_websocket__request(client_t *client)
{
    boole status0 = FALSE;
    boole status1 = FALSE;
    boole status2 = FALSE;
    boole status3 = FALSE;
    boole status4 = FALSE;

    status0 = _video_websocket_internal__is_served() == FALSE;
    if (status0 == FALSE)
    {
        status1 = client->is_authenticated == FALSE;
        status2 = client->is_dh_shared_secret_agreed_upon == FALSE;
        status3 = client->is_music_bot == TRUE;
        status4 = client->is_idle == TRUE;
    }
    if (status0 == TRUE || status1 == TRUE || status2 == TRUE || status3 == TRUE || status4 == TRUE)
    {
        DBG_VIDEO_WEBSOCKET log_info("%s %llu %s", "video session request of client", client->client_id, "refused: websocket video is not served, or the client is not authenticated, a music bot, or idle");
        return;
    }
    _video_websocket_internal__issue_session(client);
}

/**
 * @brief Closes a newly opened video socket when the server has disabled this connection.
 *
 * @param ws_cli_conn_t* socket -> WebSocket opened on /video
 *
 * @return void
 */
void video_websocket__onopen(ws_cli_conn_t *socket)
{
    boole status = FALSE;

    clib__read_lock(&g_clients_global_rwlock_guard);
    status = _video_websocket_internal__is_served();
    clib__unlock(&g_clients_global_rwlock_guard);
    if (status == FALSE)
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
 * @param ubyte* data -> the frame
 * @param uint64 size -> its length in bytes
 * @param int type -> the websocket frame type; only binary frames are accepted
 *
 * @return void
 */
void video_websocket__onmessage(ws_cli_conn_t *socket, ubyte *data, uint64 size, int type)
{
    uint64 id = ws_media_client_id(socket);
    video_websocket_session_t *session = NULL_POINTER;
    client_t *client = NULL_POINTER;
    boole valid = FALSE;
    boole relay = FALSE;
    uint64 now = util__get_timestamp_ms();

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
    if (client->is_existing == FALSE || client->is_authenticated == FALSE || client->is_idle == TRUE || session == NULL_POINTER)
    {
        goto done;
    }
    if (session->socket != socket)
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
        video_channel__relay_packet(id, socket, 0, data, size);
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
void video_websocket__onclose(ws_cli_conn_t *socket)
{
    uint64 id = ws_media_client_id(socket);
    client_t *client = NULL_POINTER;

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__read_lock(&g_webrtc_peers_rwlock_guard);
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
                video_channel__process_transport_lost(id);
            }
        }
    }
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_webrtc_peers_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Ends the client's video session, closes its socket, and erases its key.
 *
 * @param client_t* client -> client whose video session is removed
 *
 * @attention The caller holds the clients write lock; this also runs before a client slot is reused.
 *
 * @return void
 */
void video_websocket__disconnect(client_t *client)
{
    video_websocket_session_t **slot = &client->video_websocket;
    video_websocket_session_t *session = *slot;

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

    // the key is in the session: set to zero before the memory is given back
    mbedtls_md_free(&session->receive_mac);
    mbedtls_platform_zeroize(session, sizeof(*session));
    free(session);
}

/**
 * @brief Ends every video WebSocket session when video or WebSocket connections are disabled.
 *
 * @attention The caller holds the clients write lock.
 *
 * @return void
 */
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

/**
 * @brief Drops queued video packets when a client changes channel.
 *
 * @param client_t* client -> client whose attached video socket queue is cleared
 *
 * @attention The caller holds the clients lock.
 *
 * @return void
 */
void video_websocket__clear_queue(client_t *client)
{
    boole status = FALSE;

    if (client->video_websocket != NULL_POINTER)
    {
        status = client->video_websocket->socket != NULL_POINTER;
    }
    if (status == TRUE)
    {
        ws_media_clear_queue(client->video_websocket->socket);
    }
}
