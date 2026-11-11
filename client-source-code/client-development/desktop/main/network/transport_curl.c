#include "platform/memory.h"
// Linux/macOS TCP WebSockets. One worker owns every libcurl operation for a connection.
#define _POSIX_C_SOURCE 200809L
#include <curl/curl.h>
#include <pthread.h>
#include <poll.h>
#include <stdlib.h>
#include "network/transport.h"
#include "network/endpoint.h"
#include "network/protocol.h"
#include "platform/system.h"
#include "clib.h"

struct desktop_transport_t
{
    pthread_mutex_t lock;
    pthread_t worker;
    boole started;
    boole finished;
    boole stopping;
    char url[4096];
    desktop_transport_status_t status;
    desktop_message_queue_t incoming;
    desktop_message_queue_t outgoing;
};

static pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;
static boole g_curl_ready = FALSE;

// declarations
static void _transport_internal__initialize(void);
static void _transport_internal__status(desktop_transport_t *transport, desktop_transport_state_t state, const char *detail);
static boole _transport_internal__stopping(desktop_transport_t *transport);
static int _transport_internal__progress(void *context, curl_off_t total_down, curl_off_t down, curl_off_t total_up, curl_off_t up);
static void *_transport_internal__worker(void *context);
static boole _transport_internal__send(desktop_transport_t *transport, const char *bytes, uint64 size, boole binary, uint64 expires);

/**
 * @brief initializes libcurl once and verifies that this build supports WebSockets.
 * @return void
 */
static void _transport_internal__initialize(void)
{
    const curl_version_info_data *information = NULL_POINTER;
    const char *const *protocol = NULL;
    boole ws = 0;
    boole wss = 0;

    // initialize libcurl
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    {
        return;
    }

    // search for WebSocket protocols
    information = curl_version_info(CURLVERSION_NOW);
    if (information && information->protocols)
    {
        for (protocol = information->protocols; *protocol; protocol++)
        {
            ws |= clib__strcmp(*protocol, "ws") == 0;
            wss |= clib__strcmp(*protocol, "wss") == 0;
        }
    }

    // store the result
    g_curl_ready = ws && wss;
}

/**
 * @brief updates the visible connection state under the transport lock.
 * @param desktop_transport_t* transport -> owned transport
 * @param desktop_transport_state_t state -> new state
 * @param const char* detail -> bounded status description
 * @return void
 */
static void _transport_internal__status(desktop_transport_t *transport, desktop_transport_state_t state, const char *detail)
{
    // store state and detail
    pthread_mutex_lock(&transport->lock);
    transport->status.state = state;
    clib__string_copy(transport->status.detail, sizeof(transport->status.detail), detail);
    pthread_mutex_unlock(&transport->lock);
}

/**
 * @brief reads cancellation without exposing the worker's libcurl handle to another thread.
 * @param desktop_transport_t* transport -> owned transport
 * @return boole -> nonzero when cancellation was requested
 */
static boole _transport_internal__stopping(desktop_transport_t *transport)
{
    boole stopping = 0;

    // read the stop flag
    pthread_mutex_lock(&transport->lock);
    stopping = transport->stopping;
    pthread_mutex_unlock(&transport->lock);
    return stopping;
}

/**
 * @brief lets the connection handshake observe cancellation.
 * @param void* context -> transport passed to libcurl
 * @param curl_off_t total_down -> unused transfer size
 * @param curl_off_t down -> unused transfer progress
 * @param curl_off_t total_up -> unused upload size
 * @param curl_off_t up -> unused upload progress
 * @return int -> nonzero to cancel the handshake
 */
static int _transport_internal__progress(void *context, curl_off_t total_down, curl_off_t down, curl_off_t total_up, curl_off_t up)
{
    // mark unused arguments
    (void)total_down;
    (void)down;
    (void)total_up;
    (void)up;

    // return the stop flag
    return _transport_internal__stopping(context);
}

/**
 * @brief connects, reassembles bounded messages and sends queued frames on one worker.
 * @param void* context -> transport that owns this worker
 * @return void* -> NULL after cleanup
 */
static void *_transport_internal__worker(void *context)
{
    desktop_transport_t *transport = context;
    CURL *connection = NULL_POINTER;
    CURLcode result = CURLE_OK;
    CURLcode received_result = CURLE_OK;
    curl_socket_t socket = CURL_SOCKET_BAD;
    const struct curl_ws_frame *frame = NULL_POINTER;
    struct pollfd poll_socket = { 0 };
    char chunk[16384] = { 0 };
    char message_type[64] = { 0 };
    char detail[256] = { 0 };
    char *message = NULL_POINTER;
    desktop_message_t outgoing = { 0 };
    uint64 used = 0;
    ubyte close_payload[2] = { 0 };
    size_t close_bytes = 0;
    size_t received = 0;
    uint64 outgoing_used = 0;
    size_t sent = 0;
    uint64 send_deadline = 0;
    boole binary = 0;
    boole envelope = 0;
    boole progress = 0;
    boole stopped = 0;
    boole outgoing_ready = 0;
    boole incoming_pending = FALSE;

    // allocate connection and buffer
    connection = curl_easy_init();
    message = calloc(1, DESKTOP_MESSAGE_LIMIT + 1);
    if (!connection || !message)
    {
        result = CURLE_OUT_OF_MEMORY;
        goto label_finished;
    }
    // TLS verification remains enabled. only HTTP/1 WebSocket upgrades over TCP are used
    curl_easy_setopt(connection, CURLOPT_URL, transport->url);
    curl_easy_setopt(connection, CURLOPT_PROTOCOLS_STR, "ws,wss");
    curl_easy_setopt(connection, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(connection, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(connection, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(connection, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(connection, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(connection, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(connection, CURLOPT_NOPROXY, "localhost,127.0.0.1,::1");
    curl_easy_setopt(connection, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(connection, CURLOPT_XFERINFOFUNCTION, _transport_internal__progress);
    curl_easy_setopt(connection, CURLOPT_XFERINFODATA, transport);

    // open the connection
    result = curl_easy_perform(connection);
    if (result != CURLE_OK || _transport_internal__stopping(transport))
    {
        goto label_finished;
    }

    // read the socket
    result = curl_easy_getinfo(connection, CURLINFO_ACTIVESOCKET, &socket);
    if (result != CURLE_OK || socket == CURL_SOCKET_BAD)
    {
        result = CURLE_COULDNT_CONNECT;
        goto label_finished;
    }

    // store the connected state
    _transport_internal__status(transport, DESKTOP_TRANSPORT_CONNECTED, "WebSocket connected");

    // loop until stopped
    while (!_transport_internal__stopping(transport))
    {
        // reset the progress flag
        progress = 0;
        // pause further reads while the session drains its bounded queue
        if (incoming_pending)
        {
            // queue the received message
            pthread_mutex_lock(&transport->lock);
            if (desktop_message_queue__has_space(&transport->incoming, used))
            {
                if (!desktop_message_queue__push(&transport->incoming, message, used, binary))
                {
                    result = CURLE_OUT_OF_MEMORY;
                }
                else
                {
                    transport->status.messages_received++;
                    clib__string_copy(transport->status.last_type, sizeof(transport->status.last_type), envelope ? message_type : binary ? "binary" : "unrecognized envelope");
                    incoming_pending = FALSE;
                    progress = TRUE;
                }
            }
            pthread_mutex_unlock(&transport->lock);

            // stop on allocation failure
            if (result == CURLE_OUT_OF_MEMORY)
            {
                break;
            }

            // clear the message buffer
            if (!incoming_pending)
            {
                clib__null_memory(message, used);
                used = 0;
                binary = FALSE;
            }
        }

        // take next outgoing message
        if (!outgoing_ready)
        {
            pthread_mutex_lock(&transport->lock);
            desktop_message_queue__expire(&transport->outgoing, desktop_system__milliseconds());
            outgoing_ready = desktop_message_queue__pop(&transport->outgoing, &outgoing);
            pthread_mutex_unlock(&transport->lock);
            outgoing_used = 0;
            send_deadline = desktop_system__milliseconds() + 5000;
        }

        // send the outgoing message
        if (outgoing_ready)
        {
            // send the remaining bytes
            sent = 0;
            result = curl_ws_send(connection, outgoing.data + outgoing_used, outgoing.size - outgoing_used, &sent, 0, outgoing.binary ? CURLWS_BINARY : CURLWS_TEXT);
            if (result != CURLE_OK && result != CURLE_AGAIN)
            {
                break;
            }

            // count the sent bytes
            outgoing_used += sent;
            progress |= sent != 0;

            // check completion and deadline
            if (result == CURLE_OK && outgoing_used == outgoing.size)
            {
                desktop_message__release(&outgoing);
                outgoing_ready = 0;
                progress = 1;
            }
            else if (desktop_system__milliseconds() >= send_deadline)
            {
                result = CURLE_OPERATION_TIMEDOUT;
                break;
            }
        }

        // receive the next chunk
        clib__null_memory(chunk, sizeof(chunk));
        received = 0;
        frame = NULL;
        received_result = incoming_pending ? CURLE_AGAIN : curl_ws_recv(connection, chunk, sizeof(chunk), &received, &frame);
        if (received_result != CURLE_OK && received_result != CURLE_AGAIN)
        {
            result = received_result;
            break;
        }

        // read the received frame
        if (received_result == CURLE_OK && frame)
        {
            progress = 1;

            // read a close frame
            if (frame->flags & CURLWS_CLOSE)
            {
                // curl can return a control frame in several receive chunks
                if (frame->offset < 2 && received > 0)
                {
                    close_bytes = received < 2 - (size_t)frame->offset ? received : 2 - (size_t)frame->offset;
                    clib__memcpy(close_payload + frame->offset, chunk, close_bytes);
                }

                // wait for remaining bytes
                if (frame->bytesleft > 0)
                {
                    continue;
                }

                // store the close code
                if (frame->offset + received >= 2)
                {
                    pthread_mutex_lock(&transport->lock);
                    transport->status.close_code = ((unsigned int)close_payload[0] << 8) | close_payload[1];
                    pthread_mutex_unlock(&transport->lock);
                }

                // stop after the close
                result = CURLE_OK;
                break;
            }
            // control frames can occur between data fragments; they never enter message storage
            if (frame->flags & (CURLWS_TEXT | CURLWS_BINARY))
            {
                // check the message limit
                if (received > DESKTOP_MESSAGE_LIMIT - used)
                {
                    result = CURLE_FILESIZE_EXCEEDED;
                    break;
                }

                // append the chunk
                clib__memcpy(message + used, chunk, received);
                used += received;
                binary |= (frame->flags & CURLWS_BINARY) != 0;

                // message is complete
                if (frame->bytesleft == 0 && !(frame->flags & CURLWS_CONT))
                {
                    clib__null_memory(message_type, sizeof(message_type));
                    envelope = !binary && desktop_protocol__message_type(message, used, message_type, sizeof(message_type));
                    incoming_pending = TRUE;
                }
            }
        }

        // clear the result
        result = CURLE_OK;

        // wait for socket activity
        if (!progress)
        {
            clib__null_memory(&poll_socket, sizeof(poll_socket));
            poll_socket.fd = incoming_pending && !outgoing_ready ? -1 : socket;
            poll_socket.events = (incoming_pending ? 0 : POLLIN) | (outgoing_ready ? POLLOUT : 0);
            poll(&poll_socket, 1, 5);
        }
    }
label_finished:
    // close the connection
    if (connection)
    {
        curl_easy_cleanup(connection);
    }

    // free the buffers
    desktop_memory__erase(message, DESKTOP_MESSAGE_LIMIT + 1);
    free(message);
    desktop_message__release(&outgoing);

    // store the final state
    stopped = _transport_internal__stopping(transport);
    if (stopped || result == CURLE_OK)
    {
        _transport_internal__status(transport, DESKTOP_TRANSPORT_DISCONNECTED, "Disconnected");
    }
    else
    {
        pthread_mutex_lock(&transport->lock);
        transport->status.certificate_rejected = result == CURLE_PEER_FAILED_VERIFICATION || result == CURLE_SSL_PINNEDPUBKEYNOTMATCH || result == CURLE_SSL_ISSUER_ERROR;
        pthread_mutex_unlock(&transport->lock);
        clib__string_format(detail, sizeof(detail), "Connection failed: %s", curl_easy_strerror(result));
        _transport_internal__status(transport, DESKTOP_TRANSPORT_FAILED, detail);
    }

    // set the finished flag
    pthread_mutex_lock(&transport->lock);
    desktop_message_queue__clear(&transport->outgoing);
    transport->finished = 1;
    pthread_mutex_unlock(&transport->lock);
    return NULL;
}

/**
 * @brief creates a transport with bounded pending-message storage.
 * @return desktop_transport_t* -> owned transport, or NULL on allocation/lock failure
 */
desktop_transport_t *desktop_transport__create(void)
{
    desktop_transport_t *transport = NULL_POINTER;

    // initialize libcurl once
    pthread_once(&g_curl_once, _transport_internal__initialize);

    // allocate the transport
    transport = calloc(1, sizeof(*transport));
    if (!transport)
    {
        return NULL;
    }

    // create the lock
    if (pthread_mutex_init(&transport->lock, NULL) != 0)
    {
        desktop_memory__erase(transport, sizeof(*transport));
        free(transport);
        return NULL;
    }

    // set the ready status
    _transport_internal__status(transport, DESKTOP_TRANSPORT_DISCONNECTED, "Ready to connect");
    return transport;
}

/**
 * @brief requests cancellation without accessing libcurl from the calling thread.
 * @param desktop_transport_t* transport -> owned transport
 * @return void
 */
/* Requests cancellation without waiting. Destroy joins before releasing storage. */
void desktop_transport__disconnect(desktop_transport_t *transport)
{
    // nothing to disconnect
    if (!transport)
    {
        return;
    }

    // set the stop flag
    pthread_mutex_lock(&transport->lock);
    transport->stopping = 1;
    pthread_mutex_unlock(&transport->lock);
}

/**
 * @brief cancels and joins the worker before releasing its buffers and lock.
 * @param desktop_transport_t* transport -> owned transport
 * @return void
 */
void desktop_transport__destroy(desktop_transport_t *transport)
{
    // nothing to destroy
    if (!transport)
    {
        return;
    }

    // stop the worker thread
    desktop_transport__disconnect(transport);
    if (transport->started)
    {
        pthread_join(transport->worker, NULL);
    }

    // free the transport
    pthread_mutex_destroy(&transport->lock);
    desktop_message_queue__clear(&transport->incoming);
    desktop_message_queue__clear(&transport->outgoing);
    desktop_memory__erase(transport, sizeof(*transport));
    free(transport);
}

/**
 * @brief starts a validated TCP WebSocket connection when no worker is still running.
 * @param desktop_transport_t* transport -> owned transport; one caller controls connect/destroy
 * @param const char* url -> ws:// or wss:// endpoint
 * @return boole -> 1 when started; 0 on invalid input or a startup failure
 */
/* Starts a worker; the GUI thread never performs a connection or receive. */
boole desktop_transport__connect(desktop_transport_t *transport, const char *url)
{
    desktop_endpoint_t endpoint = { 0 };
    boole busy = 0;

    // check the transport
    if (!transport)
    {
        return 0;
    }

    // refuse while worker runs
    pthread_mutex_lock(&transport->lock);
    busy = transport->started && !transport->finished;
    pthread_mutex_unlock(&transport->lock);
    if (busy)
    {
        return 0;
    }

    // wait for finished worker
    if (transport->started)
    {
        pthread_join(transport->worker, NULL);
        transport->started = 0;
    }

    // check libcurl support
    if (!g_curl_ready)
    {
        _transport_internal__status(transport, DESKTOP_TRANSPORT_FAILED, "Install a libcurl build with ws and wss support.");
        return 0;
    }

    // check the address
    if (!desktop_endpoint__parse(url, &endpoint) || !clib__string_copy(transport->url, sizeof(transport->url), url))
    {
        _transport_internal__status(transport, DESKTOP_TRANSPORT_FAILED, "Enter a ws:// or wss:// address with a valid host and port.");
        return 0;
    }

    // reset the transport state
    pthread_mutex_lock(&transport->lock);
    transport->stopping = 0;
    desktop_message_queue__clear(&transport->incoming);
    desktop_message_queue__clear(&transport->outgoing);
    transport->finished = 0;
    clib__null_memory(&transport->status, sizeof(transport->status));
    pthread_mutex_unlock(&transport->lock);

    // start the worker thread
    _transport_internal__status(transport, DESKTOP_TRANSPORT_CONNECTING, "Connecting...");
    if (pthread_create(&transport->worker, NULL, _transport_internal__worker, transport) != 0)
    {
        _transport_internal__status(transport, DESKTOP_TRANSPORT_FAILED, "Could not start the connection worker.");
        return 0;
    }
    transport->started = 1;
    return 1;
}

/**
 * @brief copies the current status under the transport lock.
 * @param desktop_transport_t* transport -> owned transport
 * @param desktop_transport_status_t* status -> destination status
 * @return void
 */
void desktop_transport__status(desktop_transport_t *transport, desktop_transport_status_t *status)
{
    // copy the status
    pthread_mutex_lock(&transport->lock);
    *status = transport->status;
    status->finished = !transport->started || transport->finished;
    pthread_mutex_unlock(&transport->lock);
}

/**
 * @brief queues one bounded text message for the connection worker.
 * @param desktop_transport_t* transport -> owned transport
 * @param const char* bytes -> message bytes
 * @param uint64 size -> message length
 * @return boole -> 1 when queued; 0 if disconnected, invalid or the queue is full
 */
/* Copies into a bounded queue. The connection worker owns all network operations. */
boole desktop_transport__send(desktop_transport_t *transport, const char *bytes, uint64 size)
{
    return _transport_internal__send(transport, bytes, size, FALSE, 0);
}

/**
 * @brief queues a binary media frame or empty keepalive for the sole socket worker.
 * @param desktop_transport_t* transport -> owned transport
 * @param const ubyte* bytes -> readable bytes; may be NULL for an empty frame
 * @param uint64 size -> bounded byte count
 * @return boole -> 1 when queued
 */
boole desktop_transport__send_binary(desktop_transport_t *transport, const ubyte *bytes, uint64 size)
{
    return _transport_internal__send(transport, (const char *)bytes, size, TRUE, 0);
}

/**
 * @brief copies a complete message into the bounded outgoing queue.
 * @param desktop_transport_t* transport -> owned transport
 * @param const char* bytes -> readable message data, or NULL for an empty binary frame
 * @param uint64 size -> byte count
 * @param boole binary -> nonzero for binary framing
 * @param uint64 expires -> monotonic expiry, or zero for reliable delivery
 * @return boole -> 1 when the queue accepted the message
 */
static boole _transport_internal__send(desktop_transport_t *transport, const char *bytes, uint64 size, boole binary, uint64 expires)
{
    boole accepted = 0;
    uint64 now = desktop_system__milliseconds();

    // check arguments
    if (!transport || (!bytes && size) || (!binary && !size) || size > DESKTOP_MESSAGE_LIMIT || (expires && now >= expires))
    {
        return 0;
    }

    // queue when connected
    pthread_mutex_lock(&transport->lock);
    if (!transport->stopping && transport->status.state == DESKTOP_TRANSPORT_CONNECTED)
    {
        desktop_message_queue__expire(&transport->outgoing, now);
        accepted = desktop_message_queue__push_until(&transport->outgoing, bytes ? bytes : "", size, binary, expires);
    }
    pthread_mutex_unlock(&transport->lock);
    return accepted;
}

/**
 * @brief queues one live frame with a deadline shared with the Windows transport.
 * @param desktop_transport_t* transport -> connected audio transport
 * @param const ubyte* bytes -> encrypted voice frame
 * @param uint64 size -> frame size
 * @param uint64 expires -> nonzero monotonic expiry
 * @return boole -> TRUE when queued
 */
boole desktop_transport__send_media(desktop_transport_t *transport, const ubyte *bytes, uint64 size, uint64 expires)
{
    return expires && _transport_internal__send(transport, (const char *)bytes, size, TRUE, expires);
}

/**
 * @brief drops unsent live frames while retaining reliable attachment messages.
 * @param desktop_transport_t* transport -> owned audio transport
 * @return void
 */
void desktop_transport__discard_media(desktop_transport_t *transport)
{
    // discard unsent media
    pthread_mutex_lock(&transport->lock);
    desktop_message_queue__expire(&transport->outgoing, 0);
    pthread_mutex_unlock(&transport->lock);
}

/**
 * @brief transfers the next received message to the protocol worker.
 * @param desktop_transport_t* transport -> connected transport
 * @param desktop_message_t* message -> caller-owned output, released with desktop_message__release
 * @return boole -> 1 when available
 */
boole desktop_transport__receive(desktop_transport_t *transport, desktop_message_t *message)
{
    boole received = 0;

    // take the next message
    pthread_mutex_lock(&transport->lock);
    received = desktop_message_queue__pop(&transport->incoming, message);
    pthread_mutex_unlock(&transport->lock);
    return received;
}
