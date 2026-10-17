/*
 * Copyright (C) 2016-2022  Davidson Francis <davidsondfgl@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

/* clang-format off */
#ifndef _WIN32
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef int socklen_t;
#endif
/* clang-format on */

/* Windows and macOS seems to not have MSG_NOSIGNAL */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#include <unistd.h>

#include "../include/utf8.h"
#include "../include/ws.h"

/**
 * @dir src/
 * @brief wsServer source code
 *
 * @file ws.c
 * @brief wsServer main routines.
 */

/**
 * @brief Websocket events.
 */
static struct ws_events cli_events;

// the frames waiting for a media socket's writer thread are kept in a byte buffer that is written in a circle
// (when the end is reached, writing continues at the start; the names below call it the ring), each as a record of
// [8 B queued_at][2 B size][1 B kind][data]. a burst fits whole: a music bot's lead at song start is a few
// hundred frames of about 150 bytes, a video keyframe is up to 256 packets. a frame that waited past its
// kind's stale limit is dropped when its turn comes; a control frame (the attachment acknowledgement)
// never goes stale and is never dropped for room; video is never stale either, a viewer that falls behind
// loses the oldest packets and asks for a keyframe
#define WS_AUDIO_QUEUE_BYTES (64 * 1024)
#define WS_VIDEO_QUEUE_BYTES (512 * 1024)
#define WS_MEDIA_RECORD_HEADER 11
#define WS_MEDIA_STALE_VOICE_MS 150
#define WS_MEDIA_STALE_MUSIC_MS 2500
// every attached media socket gets an empty frame on this interval, including during continuous playback.
// the client answers with an empty frame, keeping the server's receive timeout alive even for listeners.
// no client timer is needed, so background-page timer throttling cannot suppress the reply.
#define WS_AUDIO_KEEPALIVE_INTERVAL_MS 5000
// a media socket must present its attachment proof within the hello timeout; from the bind on it may be idle
// for the idle timeout (the keepalive above keeps an attached socket within it)
#define WS_AUDIO_HELLO_TIMEOUT_MS 5000
#define WS_AUDIO_IDLE_TIMEOUT_MS 15000

static int set_socket_timeout(int sock, int option, uint32_t milliseconds);

struct ws_media_buffer
{
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_t writer;
    bool stopping;
    bool is_bound;          // the socket is attached to a client
    uint64_t next_keepalive_at; // monotonic deadline, protected by mutex; outgoing media never postpones it
    size_t head;            // offset of the oldest record in the ring
    size_t used;            // bytes of the ring in use
    size_t ring_bytes;      // the ring's size, by the socket's purpose
    size_t frame_max;       // the largest frame received or queued, by the socket's purpose
    unsigned char* received;
    unsigned char* ring;
};

#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 1025
#endif

/**
 * @brief Client socks.
 */
struct ws_connection
{

    struct ws_chat_queue* chat;
    bool chat_mode;

    int media_purpose;      // WS_MEDIA_PURPOSE_NONE for the chat socket, else the /audio or /video socket
    uint64_t media_client_id;
    struct ws_media_buffer* media;
	int client_sock; /**< Client socket FD.        */
	int state; /**< WebSocket current state. */

	/* Timeout thread and locks. */
	pthread_mutex_t mtx_state;
	pthread_cond_t cnd_state_close;
	pthread_t thrd_tout;
	bool close_thrd;

	/* Send lock. */
	pthread_mutex_t mtx_snd;

	/* IP address. */
	char ip[INET6_ADDRSTRLEN];

	bool is_using_forwarded_ip_address;
	/* IP address and port. */
	char forwarded_ip_address[INET6_ADDRSTRLEN]; /* NI_MAXHOST. */

	/* Ping/Pong IDs and locks. */
	int32_t last_pong_id;
	int32_t current_ping_id;
	pthread_mutex_t mtx_ping;
};

/**
 * @brief Clients list.
 */
static struct ws_connection* client_socks = NULL;
static uint32_t client_capacity = 0;

size_t ws_client_slot_size(void)
{
    return sizeof(struct ws_connection);
}

int ws_init_client_slots(uint32_t capacity)
{
    uint32_t i;
    /* Slot addresses are retained by connection threads and must never move. */
    if (client_socks != NULL || capacity == 0 || capacity > SIZE_MAX / sizeof(*client_socks))
    {
        return -1;
    }
    client_socks = calloc(capacity, sizeof(*client_socks));
    if (client_socks == NULL) { return -1; }
    client_capacity = capacity;
    for (i = 0; i < capacity; i++) { client_socks[i].client_sock = -1; }
    return 0;
}

/**
 * @brief Timeout to a single send().
 */
static uint32_t timeout;

static void _ws_internal__chat_abort(ws_cli_conn_t* client);
static int _ws_internal__chat_start(ws_cli_conn_t* client);
static void _ws_internal__chat_stop(ws_cli_conn_t* client);
static int _ws_internal__chat_queue_frame(ws_cli_conn_t* client, const char* data, size_t size, int type);

static uint64_t audio_clock_ms(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int ws_media_purpose(ws_cli_conn_t* client)
{
    return client->media_purpose;
}

uint64_t ws_media_client_id(ws_cli_conn_t* client)
{
    return client->media_client_id;
}

void ws_media_bind(ws_cli_conn_t* client, uint64_t id)
{
    client->media_client_id = id;
    set_socket_timeout(client->client_sock, SO_RCVTIMEO, WS_AUDIO_IDLE_TIMEOUT_MS);
    if (client->media != NULL)
    {
        pthread_mutex_lock(&client->media->mutex);
        client->media->next_keepalive_at = audio_clock_ms() + WS_AUDIO_KEEPALIVE_INTERVAL_MS;
        client->media->is_bound = true;
        pthread_mutex_unlock(&client->media->mutex);
    }
}

// Shutdown wakes the reader. Only that reader may release and reuse the slot.
void ws_abort_client(ws_cli_conn_t* client)
{
    if (client != NULL && client->chat_mode)
    {
        _ws_internal__chat_abort(client);
        return;
    }
    if (client != NULL && client->client_sock != -1)
    {
        shutdown(client->client_sock, 2);
    }
}

/** @brief copies bytes into the ring buffer at an offset, continuing at its start when its end is reached */
static void media_ring_write(struct ws_media_buffer* media, size_t offset, const unsigned char* source, size_t size)
{
    size_t first = 0;

    offset = offset % media->ring_bytes;
    first = media->ring_bytes - offset;
    if (first > size)
    {
        first = size;
    }
    memcpy(media->ring + offset, source, first);
    if (size > first)
    {
        memcpy(media->ring, source + first, size - first);
    }
}

/** @brief copies bytes out of the ring buffer from an offset, continuing at its start when its end is reached */
static void media_ring_read(struct ws_media_buffer* media, size_t offset, unsigned char* destination, size_t size)
{
    size_t first = 0;

    offset = offset % media->ring_bytes;
    first = media->ring_bytes - offset;
    if (first > size)
    {
        first = size;
    }
    memcpy(destination, media->ring + offset, first);
    if (size > first)
    {
        memcpy(destination + first, media->ring, size - first);
    }
}

/** @brief reads the header of the record at an offset: when it was queued, its payload size and its kind */
static void media_ring_header(struct ws_media_buffer* media, size_t offset, uint64_t* queued_at, size_t* size, int* kind)
{
    unsigned char header[WS_MEDIA_RECORD_HEADER];
    int i = 0;

    media_ring_read(media, offset, header, sizeof(header));
    *queued_at = 0;
    for (i = 0; i < 8; i++)
    {
        *queued_at = (*queued_at << 8) | header[i];
    }
    *size = ((size_t)header[8] << 8) | header[9];
    *kind = header[10];
}

/** @brief drops every queued media frame; the control frames stay. used when the client changes channel, so nothing of the old channel follows it */
void ws_media_clear_queue(ws_cli_conn_t* client)
{
    struct ws_media_buffer* media = client->media;
    unsigned char* kept = NULL;
    size_t kept_size = 0;
    size_t offset = 0;
    size_t walked = 0;
    uint64_t queued_at = 0;
    size_t size = 0;
    int kind = 0;

    if (media == NULL)
    {
        return;
    }
    pthread_mutex_lock(&media->mutex);
    kept = malloc(media->used > 0 ? media->used : 1);
    if (kept != NULL)
    {
        offset = media->head;
        while (walked < media->used)
        {
            media_ring_header(media, offset, &queued_at, &size, &kind);
            if (kind == WS_MEDIA_KIND_CONTROL)
            {
                media_ring_read(media, offset, kept + kept_size, WS_MEDIA_RECORD_HEADER + size);
                kept_size += WS_MEDIA_RECORD_HEADER + size;
            }
            offset += WS_MEDIA_RECORD_HEADER + size;
            walked += WS_MEDIA_RECORD_HEADER + size;
        }
        media->head = 0;
        media->used = kept_size;
        if (kept_size > 0)
        {
            memcpy(media->ring, kept, kept_size);
        }
        free(kept);
    }
    pthread_mutex_unlock(&media->mutex);
}

/**
 * @brief queues one frame for the socket's writer thread
 *
 *        when the ring is full the oldest media frames make room; a control frame at the head is never
 *        dropped, the new frame is dropped instead
 *
 * @param ws_cli_conn_t* client -> the media socket
 * @param const unsigned char* data -> the frame payload
 * @param size_t size -> its length, at most the socket's frame limit
 * @param int kind -> WS_MEDIA_KIND_CONTROL, WS_MEDIA_KIND_VOICE, WS_MEDIA_KIND_MUSIC or WS_MEDIA_KIND_VIDEO
 *
 * @return int -> 0 when queued or dropped for a control frame, -1 when the socket has no queue or the frame is too large
 */
int ws_media_queue_frame(ws_cli_conn_t* client, const unsigned char* data, size_t size, int kind)
{
    struct ws_media_buffer* media = client->media;
    unsigned char header[WS_MEDIA_RECORD_HEADER];
    size_t record_size = WS_MEDIA_RECORD_HEADER + size;
    uint64_t now = audio_clock_ms();
    uint64_t oldest_queued_at = 0;
    size_t oldest_size = 0;
    int oldest_kind = 0;
    int i = 0;

    if (media == NULL || size > media->frame_max || record_size > media->ring_bytes)
    {
        return -1;
    }
    pthread_mutex_lock(&media->mutex);
    if (media->stopping)
    {
        pthread_mutex_unlock(&media->mutex);
        return -1;
    }
    while (media->used + record_size > media->ring_bytes)
    {
        media_ring_header(media, media->head, &oldest_queued_at, &oldest_size, &oldest_kind);
        if (oldest_kind == WS_MEDIA_KIND_CONTROL)
        {
            pthread_mutex_unlock(&media->mutex);
            return 0;
        }
        media->head = (media->head + WS_MEDIA_RECORD_HEADER + oldest_size) % media->ring_bytes;
        media->used -= WS_MEDIA_RECORD_HEADER + oldest_size;
    }
    for (i = 0; i < 8; i++)
    {
        header[i] = (unsigned char)(now >> (56 - i * 8));
    }
    header[8] = (unsigned char)(size >> 8);
    header[9] = (unsigned char)size;
    header[10] = (unsigned char)kind;
    media_ring_write(media, media->head + media->used, header, sizeof(header));
    if (size > 0)
    {
        media_ring_write(media, media->head + media->used + WS_MEDIA_RECORD_HEADER, data, size);
    }
    media->used += record_size;
    pthread_cond_signal(&media->ready);
    pthread_mutex_unlock(&media->mutex);
    return 0;
}

/* Only the HTTP upgrade read is timed; established idle clients use the
 * application's heartbeat policy. Windows expects milliseconds, POSIX timeval. */
static int set_socket_timeout(int sock, int option, uint32_t milliseconds)
{
#ifdef _WIN32
	DWORD value = milliseconds;
#else
	struct timeval value;
	value.tv_sec = milliseconds / 1000;
	value.tv_usec = (milliseconds % 1000) * 1000;
#endif
	return setsockopt(sock, SOL_SOCKET, option, (const char *)&value, sizeof(value));
}

/**
 * @brief Client validity macro
 */
#define CLIENT_VALID(cli) (client_socks != NULL && (cli) != NULL && (cli) >= &client_socks[0] && (cli) <= &client_socks[client_capacity - 1] && (cli)->client_sock > -1)

/**
 * @brief WebSocket frame data
 */
struct ws_frame_data
{
	/**
	 * @brief Frame read.
	 */
	unsigned char frm[MESSAGE_LENGTH];
	/**
	 * @brief Processed message at the moment.
	 */
	unsigned char *msg;
	/**
	 * @brief Control frame payload
	 */
	unsigned char msg_ctrl[125];
	/**
	 * @brief Current byte position.
	 */
	size_t cur_pos;
	/**
	 * @brief Amount of read bytes.
	 */
	size_t amt_read;
	/**
	 * @brief Frame type, like text or binary.
	 */
	int frame_type;
	/**
	 * @brief Frame size.
	 */
	uint64_t frame_size;
	/**
	 * @brief Error flag, set when a read was not possible.
	 */
	int error;
	/**
	 * @brief Client connection structure.
	 */
	ws_cli_conn_t *client;
};

/**
 * @brief Global mutex.
 */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Issues an error message and aborts the program.
 *
 * @param s Error message.
 */
#define panic(s)           \
	do                 \
	{                  \
		perror(s); \
		exit(-1);  \
	} while (0);

/**
 * @brief Shutdown and close a given socket.
 *
 * @param fd Socket file descriptor to be closed.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
static void close_socket(int fd)
{
#ifndef _WIN32
	shutdown(fd, SHUT_RDWR);
	close(fd);
#else
	closesocket(fd);
#endif
}

/**
 * @brief Shuts a socket down for reading and writing WITHOUT closing the
 * file descriptor or touching any client state.
 *
 * Used by the ping-timeout path: it wakes the client's own connection
 * thread out of recv(), which then leaves its read loop, fires the onclose
 * event and runs the normal close_client() teardown on its own thread -
 * the same code path as a protocol-level close.
 *
 * @param fd Socket file descriptor to be shut down.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
static void shutdown_socket(int fd)
{
#ifndef _WIN32
	shutdown(fd, SHUT_RDWR);
#else
	shutdown(fd, SD_BOTH);
#endif
}

// Chat messages wait here until a worker encrypts them and the socket can accept
// them. A slow receiver never makes an encryption worker wait inside send().
// Each connection owns its queue until its reader has stopped and every queued
// message has been released. The socket slot cannot be reused before that point.
#define WS_CHAT_WORKERS 4
#define WS_CHAT_CLIENT_BYTES (32U * 1024U * 1024U)
#define WS_CHAT_TOTAL_BYTES (128U * 1024U * 1024U)
#define WS_CHAT_CLIENT_MESSAGES 1024U
#define WS_CHAT_TOTAL_MESSAGES 65536U
#define WS_CHAT_WRITE_CHUNK 16384U
#define WS_CHAT_STALL_TIMEOUT_MS 10000U
#define WS_CHAT_CLOSE_DRAIN_MS 2000U

struct ws_chat_job
{
    struct ws_chat_job* next;
    struct ws_chat_queue* queue;
    void* context;
    char* (*encode)(void* context, size_t* size);
    void (*release_context)(void* context);
    void (*release_encoded)(char* encoded);
    char* encoded;
    size_t size;
    size_t charge;
    size_t written;
    unsigned char header[10];
    size_t header_size;
    int type;
    bool encoding;
    bool ready;
    uint64_t last_progress;
};

struct ws_chat_queue
{
    struct ws_chat_queue* next;
    ws_cli_conn_t* client;
    struct ws_chat_job* first;
    struct ws_chat_job* last;
    size_t bytes;
    size_t messages;
    bool stopped;
    bool writer_active;
    bool close_requested;
    uint64_t close_deadline;
};

#ifdef _WIN32
typedef WSAPOLLFD ws_pollfd_t;
#else
typedef struct pollfd ws_pollfd_t;
#endif

static pthread_mutex_t chat_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t chat_encode_ready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t chat_write_ready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t chat_cleanup_changed = PTHREAD_COND_INITIALIZER;
static pthread_once_t chat_once = PTHREAD_ONCE_INIT;
static struct ws_chat_queue* chat_first = NULL;
static struct ws_chat_queue* chat_last = NULL;
static size_t chat_bytes = 0;
static size_t chat_messages = 0;
static bool chat_workers_running = false;
static bool chat_workers_stopping = false;
static pthread_t chat_workers[WS_CHAT_WORKERS];
static pthread_t chat_writer;
static ws_pollfd_t* chat_pollfds = NULL;
static struct ws_chat_queue** chat_polled_queues = NULL;

/**
 * @brief Waits until one of the supplied sockets is ready for reading or writing.
 *
 * @param ws_pollfd_t* descriptors -> Socket entries with the events to wait for. The operating
 * system writes the events that occurred into these same entries.
 * @param size_t count -> Number of socket entries in descriptors.
 * @param int milliseconds -> Maximum time to wait, or -1 to wait without a time limit.
 *
 * @return int -> Number of sockets with events, 0 when the wait expires, or -1 on error.
 */
static int _ws_internal__chat_poll(ws_pollfd_t* descriptors, size_t count, int milliseconds)
{
#ifdef _WIN32
    return WSAPoll(descriptors, (ULONG)count, milliseconds);
#else
    return poll(descriptors, (nfds_t)count, milliseconds);
#endif
}

/**
 * @brief Checks whether the last socket operation can be tried again.
 *
 * @return bool -> true when the socket could not accept the operation yet or the
 * operation was interrupted; false for other errors.
 *
 * @attention Call this immediately after a failed socket operation on the same
 * thread, before another operation can replace the operating system error.
 */
static bool _ws_internal__socket_would_block(void)
{
#ifdef _WIN32
    int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

// Only chat sockets become nonblocking. Their existing reader still waits for
// incoming bytes, but the shared writer can leave a full socket alone and send
// to another client. Audio and video keep their existing receive timeouts.
/**
 * @brief Reads bytes from a socket and waits for more data when a chat socket
 * cannot provide any bytes yet.
 *
 * @param ws_cli_conn_t* client -> Connection whose socket supplies the incoming bytes.
 * @param void* buffer -> Destination buffer for the bytes that were received.
 * @param size_t length -> Maximum number of bytes that fit in buffer.
 *
 * @return ssize_t -> Number of bytes received, 0 when the peer closes its connection, or
 * -1 on error. Audio and video sockets keep their existing receive behavior.
 *
 * @attention The connection's reader owns this call. Do not hold application
 * locks while waiting for incoming bytes.
 */
static ssize_t _ws_internal__receive_bytes(ws_cli_conn_t* client, void* buffer, size_t length)
{
    ssize_t received;
    ws_pollfd_t descriptor;

    for (;;)
    {
        received = recv(client->client_sock, buffer, length, 0);
        if (received >= 0 || !client->chat_mode || !_ws_internal__socket_would_block())
        {
            return received;
        }
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.fd = client->client_sock;
        descriptor.events = POLLIN;
        if (_ws_internal__chat_poll(&descriptor, 1, -1) < 0 && !_ws_internal__socket_would_block())
        {
            return -1;
        }
    }
}

/**
 * @brief Waits up to ten milliseconds for a change in the outgoing chat queues.
 *
 * @param pthread_cond_t* condition -> Condition to wait on while temporarily releasing chat_mutex.
 *
 * @return void
 *
 * @attention The caller must hold chat_mutex. The mutex is held again when this
 * function returns, including when the wait ends without a notification.
 */
static void _ws_internal__chat_wait_briefly(pthread_cond_t* condition)
{
    struct timespec until;

    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 10000000;
    if (until.tv_nsec >= 1000000000)
    {
        until.tv_nsec -= 1000000000;
        until.tv_sec++;
    }
    pthread_cond_timedwait(condition, &chat_mutex, &until);
}

/**
 * @brief Builds the WebSocket frame header for an outgoing message.
 *
 * @param struct ws_chat_job* job -> Message containing the frame type, encoded bytes and byte count.
 * The generated header and its length are stored in this same message.
 *
 * @return bool -> true when the message can be sent as a frame; false when its size or
 * contents do not meet the frame requirements.
 *
 * @attention The caller must own this message while changing its header.
 */
static bool _ws_internal__chat_make_header(struct ws_chat_job* job)
{
    size_t i;

    if (job->size > MAX_FRAME_LENGTH || (job->size > 0 && job->encoded == NULL)
        || (job->type >= WS_FR_OP_CLSE && job->size > 125))
    {
        return false;
    }
    job->header[0] = WS_FIN | job->type;
    if (job->size <= 125)
    {
        job->header[1] = (unsigned char)job->size;
        job->header_size = 2;
    }
    else if (job->size <= 65535)
    {
        job->header[1] = 126;
        job->header[2] = (unsigned char)(job->size >> 8);
        job->header[3] = (unsigned char)job->size;
        job->header_size = 4;
    }
    else
    {
        job->header[1] = 127;
        for (i = 0; i < 8; i++)
        {
            job->header[2 + i] = (unsigned char)((uint64_t)job->size >> (56 - 8 * i));
        }
        job->header_size = 10;
    }
    return true;
}

// Removing a job from the list does not release the connection yet. Its count
// stays charged until its buffers and callback context have also been freed.
// This lets the reader safely wait without holding any application lock.
/**
 * @brief Frees one outgoing message and removes its reserved memory from the
 * queue's totals after all of its buffers have been released.
 *
 * @param struct ws_chat_job* job -> Detached message to release, including its callback data and any
 * encoded bytes. The message must no longer be in use by an encoder or writer.
 *
 * @return void
 *
 * @attention Do not hold chat_mutex when calling this function. Release callbacks
 * run before that mutex is acquired to update the queue's totals.
 */
static void _ws_internal__chat_release_job(struct ws_chat_job* job)
{
    struct ws_chat_queue* queue = job->queue;
    size_t charge = job->charge;

    if (job->release_context != NULL)
    {
        job->release_context(job->context);
    }
    if (job->encoded != NULL && job->release_encoded != NULL)
    {
        job->release_encoded(job->encoded);
    }
    free(job);
    if (queue != NULL)
    {
        pthread_mutex_lock(&chat_mutex);
        queue->bytes -= charge;
        queue->messages--;
        chat_bytes -= charge;
        chat_messages--;
        if (queue->stopped && queue->messages == 0)
        {
            pthread_cond_broadcast(&chat_cleanup_changed);
        }
        pthread_mutex_unlock(&chat_mutex);
    }
}

/**
 * @brief Frees a detached list of outgoing messages and their buffers.
 *
 * @param struct ws_chat_job* jobs -> First message to release, or NULL when there are no messages.
 *
 * @return void
 *
 * @attention Do not hold chat_mutex. No encoder or writer may still be using
 * any message in this list.
 */
static void _ws_internal__chat_release_list(struct ws_chat_job* jobs)
{
    struct ws_chat_job* next;

    while (jobs != NULL)
    {
        next = jobs->next;
        _ws_internal__chat_release_job(jobs);
        jobs = next;
    }
}

/**
 * @brief Adds a detached list of messages to another list awaiting cleanup.
 *
 * @param struct ws_chat_job** collected -> Address of the list that will receive the messages.
 * @param struct ws_chat_job* jobs -> First message to add, or NULL when there are no messages to add.
 *
 * @return void
 *
 * @attention The caller must own both lists. This function only links the lists
 * together; their messages and buffers are freed separately.
 */
static void _ws_internal__chat_collect(struct ws_chat_job** collected, struct ws_chat_job* jobs)
{
    struct ws_chat_job* last;

    if (jobs == NULL)
    {
        return;
    }
    last = jobs;
    while (last->next != NULL)
    {
        last = last->next;
    }
    last->next = *collected;
    *collected = jobs;
}

/**
 * @brief Removes the first message from a connection's outgoing queue.
 *
 * @param struct ws_chat_queue* queue -> Connection queue whose first message will be removed.
 *
 * @return struct ws_chat_job* -> Removed message, or NULL when the queue was empty. The caller must
 * release the returned message when it is no longer needed.
 *
 * @attention The caller must hold chat_mutex. The removed message remains counted
 * in the queue's reserved memory until its buffers have been released.
 */
static struct ws_chat_job* _ws_internal__chat_remove_first(struct ws_chat_queue* queue)
{
    struct ws_chat_job* job = queue->first;

    if (job != NULL)
    {
        queue->first = job->next;
        if (queue->first == NULL)
        {
            queue->last = NULL;
        }
        job->next = NULL;
    }
    return job;
}

// Called with chat_mutex held. The active encoder or writer keeps its first
// job; it will notice stopping and release that job itself. Everything after
// it can be cancelled immediately without waiting for either worker.
/**
 * @brief Stops a connection and detaches the queued messages that can be freed
 * immediately.
 *
 * @param struct ws_chat_queue* queue -> Connection queue to stop and cancel.
 *
 * @return struct ws_chat_job* -> Detached messages for the caller to release after unlocking chat_mutex,
 * or NULL when no messages can be released immediately.
 *
 * @attention The caller must hold chat_mutex. A message still being encoded or
 * written stays in the queue until its worker has finished using it.
 */
static struct ws_chat_job* _ws_internal__chat_abort_locked(struct ws_chat_queue* queue)
{
    struct ws_chat_job* cancelled;

    if (!queue->stopped)
    {
        queue->stopped = true;
        shutdown_socket(queue->client->client_sock);
    }
    cancelled = queue->first;
    if (cancelled != NULL && (cancelled->encoding || queue->writer_active))
    {
        cancelled = queue->first->next;
        queue->first->next = NULL;
        queue->last = queue->first;
    }
    else
    {
        queue->first = NULL;
        queue->last = NULL;
    }
    pthread_cond_broadcast(&chat_cleanup_changed);
    return cancelled;
}

/**
 * @brief Stops a chat connection and releases messages that are no longer in use.
 *
 * @param ws_cli_conn_t* client -> Connection whose outgoing queue and socket should be stopped.
 *
 * @return void
 *
 * @attention The caller must keep client alive and must not hold chat_mutex.
 * This function leaves final connection cleanup to the connection's reader.
 */
static void _ws_internal__chat_abort(ws_cli_conn_t* client)
{
    struct ws_chat_job* cancelled = NULL;

    pthread_mutex_lock(&chat_mutex);
    if (client->chat != NULL)
    {
        cancelled = _ws_internal__chat_abort_locked(client->chat);
    }
    pthread_mutex_unlock(&chat_mutex);
    _ws_internal__chat_release_list(cancelled);
}

/**
 * @brief Adds a message to a connection's outgoing queue while checking the
 * per-connection and server-wide memory and message limits.
 *
 * @param ws_cli_conn_t* client -> Connection that should receive the message.
 * @param struct ws_chat_job* job -> Message whose ownership transfers to this function on every return.
 *
 * @return int -> 0 when the message was queued or a duplicate close was handled;
 * -1 when the message could not be queued.
 *
 * @attention The caller must keep client alive and must not hold chat_mutex.
 * Exceeding a queue limit stops that connection and cancels its waiting messages.
 */
static int _ws_internal__chat_add_job(ws_cli_conn_t* client, struct ws_chat_job* job)
{
    struct ws_chat_queue* queue;
    struct ws_chat_job* cancelled = NULL;
    int result = -1;

    pthread_mutex_lock(&chat_mutex);
    queue = client != NULL ? client->chat : NULL;
    if (queue != NULL && !queue->stopped && !queue->close_requested)
    {
        if (job->charge > WS_CHAT_CLIENT_BYTES || queue->bytes > WS_CHAT_CLIENT_BYTES - job->charge
            || chat_bytes > WS_CHAT_TOTAL_BYTES - job->charge
            || queue->messages >= WS_CHAT_CLIENT_MESSAGES || chat_messages >= WS_CHAT_TOTAL_MESSAGES)
        {
            cancelled = _ws_internal__chat_abort_locked(queue);
        }
        else
        {
            job->queue = queue;
            if (queue->last != NULL)
            {
                queue->last->next = job;
            }
            else
            {
                queue->first = job;
            }
            queue->last = job;
            queue->bytes += job->charge;
            queue->messages++;
            chat_bytes += job->charge;
            chat_messages++;
            if (job->type == WS_FR_OP_CLSE)
            {
                queue->close_requested = true;
                queue->close_deadline = audio_clock_ms() + WS_CHAT_CLOSE_DRAIN_MS;
            }
            if (queue->first == job)
            {
                if (job->ready)
                {
                    pthread_cond_signal(&chat_write_ready);
                }
                else
                {
                    pthread_cond_signal(&chat_encode_ready);
                }
            }
            result = 0;
        }
    }
    else if (queue != NULL && !queue->stopped && queue->close_requested && job->type == WS_FR_OP_CLSE)
    {
        result = 1;
    }
    pthread_mutex_unlock(&chat_mutex);
    _ws_internal__chat_release_list(cancelled);
    if (result != 0)
    {
        _ws_internal__chat_release_job(job);
    }
    return result < 0 ? -1 : 0;
}

// The context belongs to this function on every return, including failure.
// Its encoder runs on a worker and returns the message bytes without a trailing
// NUL in encoded_size. The supplied release functions match the caller's own
// allocators. charged_bytes must include its context and peak encoding buffers.
/**
 * @brief Queues a text message for a worker to encode and then send in order
 * with the connection's other outgoing messages.
 *
 * @param ws_cli_conn_t* client -> Chat connection that should receive the message.
 * @param void* context -> Caller-created data passed to encode and release_context.
 * Its ownership transfers to this function even when queuing fails.
 * @param size_t charged_bytes -> Memory to reserve for the context and the largest total
 * of its encoding buffers. This function also reserves the queue entry's size.
 * @param char*(*)(void*, size_t*) encode -> Callback that returns the encoded buffer and writes its byte
 * count, excluding any trailing NUL, to encoded_size. NULL means encoding failed.
 * @param void(*)(void*) release_context -> Callback that releases context using its original
 * allocator. It runs after encoding or when the message is cancelled or rejected.
 * @param void(*)(char*) release_encoded -> Callback that releases a non-NULL encoded buffer using
 * its original allocator once sending or cancellation has finished.
 *
 * @return int -> 0 when the message was queued, or -1 when it could not be queued.
 *
 * @attention All callbacks are required. Keep client alive during this call and
 * do not hold chat_mutex. The encoder runs on a shared worker outside application
 * and transport locks, so context must own everything it will read. A successful
 * return means the message was queued, not that its peer has received it.
 */
int ws_queue_text(ws_cli_conn_t* client, void* context, size_t charged_bytes,
    char* (*encode)(void* context, size_t* encoded_size),
    void (*release_context)(void* context), void (*release_encoded)(char* encoded))
{
    struct ws_chat_job* job = NULL;

    if (encode != NULL && release_context != NULL && release_encoded != NULL
        && charged_bytes <= WS_CHAT_CLIENT_BYTES - sizeof(*job))
    {
        job = calloc(1, sizeof(*job));
    }
    if (job == NULL)
    {
        if (release_context != NULL)
        {
            release_context(context);
        }
        ws_abort_client(client);
        return -1;
    }
    job->context = context;
    job->encode = encode;
    job->release_context = release_context;
    job->release_encoded = release_encoded;
    job->charge = charged_bytes + sizeof(*job);
    job->type = WS_FR_OP_TXT;
    return _ws_internal__chat_add_job(client, job);
}

/**
 * @brief Frees encoded frame bytes that were allocated with malloc.
 *
 * @param char* bytes -> Buffer to release, or NULL when there is no buffer.
 *
 * @return void
 */
static void _ws_internal__chat_free_encoded(char* bytes)
{
    free(bytes);
}

/**
 * @brief Copies a complete frame's contents into the outgoing chat queue.
 *
 * @param ws_cli_conn_t* client -> Connection that should receive the frame.
 * @param const char* data -> Frame contents to copy. May be NULL for an empty frame.
 * @param size_t size -> Number of bytes to copy from data.
 * @param int type -> WebSocket frame type, such as text, ping, pong or close.
 *
 * @return int -> 0 when the frame was accepted, or -1 when it could not be queued.
 *
 * @attention The caller keeps ownership of data and must keep client alive.
 * Do not hold chat_mutex. Allocation or frame validation failure aborts the
 * connection instead of leaving its outgoing message sequence incomplete.
 */
static int _ws_internal__chat_queue_frame(ws_cli_conn_t* client, const char* data, size_t size, int type)
{
    struct ws_chat_job* job;

    if (size > WS_CHAT_CLIENT_BYTES - sizeof(*job) || (size != 0 && data == NULL))
    {
        ws_abort_client(client);
        return -1;
    }
    job = calloc(1, sizeof(*job));
    if (job == NULL)
    {
        ws_abort_client(client);
        return -1;
    }
    if (size != 0)
    {
        job->encoded = malloc(size);
        if (job->encoded == NULL)
        {
            free(job);
            ws_abort_client(client);
            return -1;
        }
        memcpy(job->encoded, data, size);
    }
    job->release_encoded = _ws_internal__chat_free_encoded;
    job->size = size;
    job->charge = size + sizeof(*job);
    job->type = type;
    job->ready = true;
    if (!_ws_internal__chat_make_header(job))
    {
        _ws_internal__chat_release_job(job);
        ws_abort_client(client);
        return -1;
    }
    return _ws_internal__chat_add_job(client, job);
}

/**
 * @brief Converts queued messages into outgoing bytes without holding the chat
 * queue mutex or waiting for sockets to accept data.
 *
 * @param void* unused -> Unused thread argument, passed as NULL when the worker starts.
 *
 * @return void* -> NULL when the shared workers are asked to stop.
 *
 * @attention Each connection has at most one message being encoded at a time.
 * The message count keeps its queue alive until encoding and cleanup finish.
 */
static void* _ws_internal__chat_encode_worker(void* unused)
{
    struct ws_chat_queue* queue;
    struct ws_chat_queue* previous;
    struct ws_chat_job* job;
    struct ws_chat_job* cancelled;
    char* encoded;
    size_t encoded_size;
    bool valid;

    (void)unused;
    for (;;)
    {
        pthread_mutex_lock(&chat_mutex);
        for (;;)
        {
            if (chat_workers_stopping)
            {
                pthread_mutex_unlock(&chat_mutex);
                return NULL;
            }
            previous = NULL;
            queue = chat_first;
            while (queue != NULL)
            {
                job = queue->first;
                if (!queue->stopped && job != NULL && !job->ready && !job->encoding)
                {
                    break;
                }
                previous = queue;
                queue = queue->next;
            }
            if (queue != NULL)
            {
                break;
            }
            pthread_cond_wait(&chat_encode_ready, &chat_mutex);
        }
        job = queue->first;
        job->encoding = true;
        // Move the chosen connection to the end so one busy sender cannot
        // keep taking every worker while other clients have messages waiting.
        if (queue != chat_last)
        {
            if (previous != NULL)
            {
                previous->next = queue->next;
            }
            else
            {
                chat_first = queue->next;
            }
            chat_last->next = queue;
            chat_last = queue;
            queue->next = NULL;
        }
        pthread_mutex_unlock(&chat_mutex);

        encoded_size = 0;
        encoded = job->encode(job->context, &encoded_size);
        job->release_context(job->context);
        job->context = NULL;
        job->release_context = NULL;
        job->encoded = encoded;
        job->size = encoded_size;
        valid = encoded != NULL && encoded_size <= job->charge - sizeof(*job) && _ws_internal__chat_make_header(job);

        pthread_mutex_lock(&chat_mutex);
        job->encoding = false;
        cancelled = NULL;
        if (!valid || queue->stopped)
        {
            cancelled = _ws_internal__chat_abort_locked(queue);
        }
        else
        {
            job->ready = true;
            pthread_cond_signal(&chat_write_ready);
        }
        pthread_mutex_unlock(&chat_mutex);
        _ws_internal__chat_release_list(cancelled);
    }
}

/**
 * @brief Sends one small part of the first queued frame without waiting for
 * the socket to accept more bytes.
 *
 * @param struct ws_chat_queue* queue -> Connection queue whose first frame is ready to be written.
 *
 * @return bool -> true when sending can continue, including when the socket must wait
 * until a later attempt; false when the connection can no longer be written.
 *
 * @attention The writer must mark the queue as writer_active before releasing
 * chat_mutex and calling this function. That mark keeps the socket and first
 * message alive until the writer finishes using them.
 */
static bool _ws_internal__chat_write_piece(struct ws_chat_queue* queue)
{
    struct ws_chat_job* job = queue->first;
    unsigned char first_piece[WS_CHAT_WRITE_CHUNK];
    const char* bytes;
    size_t remaining;
    size_t header_bytes;
    ssize_t written;

    remaining = job->header_size + job->size - job->written;
    if (remaining > WS_CHAT_WRITE_CHUNK)
    {
        remaining = WS_CHAT_WRITE_CHUNK;
    }
    if (job->written < job->header_size)
    {
        // Keep the WebSocket header and first data bytes in one send, so small
        // messages do not need a separate socket call just for the header.
        header_bytes = job->header_size - job->written;
        memcpy(first_piece, job->header + job->written, header_bytes);
        if (remaining > header_bytes)
        {
            memcpy(first_piece + header_bytes, job->encoded, remaining - header_bytes);
        }
        bytes = (const char*)first_piece;
    }
    else
    {
        bytes = job->encoded + job->written - job->header_size;
    }
    written = send(queue->client->client_sock, bytes, remaining, MSG_NOSIGNAL);
    if (written < 0 && _ws_internal__socket_would_block())
    {
        return true;
    }
    if (written <= 0)
    {
        return false;
    }
    job->written += (size_t)written;
    job->last_progress = audio_clock_ms();
    return true;
}

/**
 * @brief Sends queued frames to sockets that are ready and checks for connections
 * that have stopped accepting data or have exceeded their closing deadline.
 *
 * @param void* unused -> Unused thread argument, passed as NULL when the writer starts.
 *
 * @return void* -> NULL when the shared workers are asked to stop.
 *
 * @attention This is the only writer for queued chat frames. Each selected queue
 * is kept alive while its socket is checked and written outside chat_mutex.
 */
static void* _ws_internal__chat_socket_writer(void* unused)
{
    struct ws_chat_queue* queue;
    struct ws_chat_job* retired;
    struct ws_chat_job* job;
    uint64_t now;
    size_t count;
    size_t i;
    int poll_result;
    bool write_ok;

    (void)unused;
    for (;;)
    {
        pthread_mutex_lock(&chat_mutex);
        if (chat_workers_stopping)
        {
            pthread_mutex_unlock(&chat_mutex);
            return NULL;
        }
        count = 0;
        retired = NULL;
        now = audio_clock_ms();
        for (queue = chat_first; queue != NULL; queue = queue->next)
        {
            if (!queue->stopped && queue->close_requested && now >= queue->close_deadline)
            {
                _ws_internal__chat_collect(&retired, _ws_internal__chat_abort_locked(queue));
            }
            job = queue->first;
            if (queue->stopped || job == NULL || !job->ready || job->encoding)
            {
                continue;
            }
            if (job->last_progress == 0)
            {
                job->last_progress = now;
            }
            if (now - job->last_progress >= WS_CHAT_STALL_TIMEOUT_MS)
            {
                _ws_internal__chat_collect(&retired, _ws_internal__chat_abort_locked(queue));
                continue;
            }
            queue->writer_active = true;
            memset(&chat_pollfds[count], 0, sizeof(chat_pollfds[count]));
            chat_pollfds[count].fd = queue->client->client_sock;
            chat_pollfds[count].events = POLLOUT;
            chat_polled_queues[count] = queue;
            count++;
        }
        if (count == 0 && retired == NULL)
        {
            _ws_internal__chat_wait_briefly(&chat_write_ready);
        }
        pthread_mutex_unlock(&chat_mutex);
        _ws_internal__chat_release_list(retired);
        if (count == 0)
        {
            continue;
        }

        // Each listed queue is pinned by writer_active until this poll result
        // is consumed. Its reader cannot close or reuse its socket meanwhile.
        poll_result = _ws_internal__chat_poll(chat_pollfds, count, 10);
        retired = NULL;
        pthread_mutex_lock(&chat_mutex);
        for (i = 0; i < count; i++)
        {
            queue = chat_polled_queues[i];
            job = queue->first;
            if (!queue->stopped && poll_result > 0
                && (chat_pollfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            {
                _ws_internal__chat_collect(&retired, _ws_internal__chat_abort_locked(queue));
            }
            else if (!queue->stopped && poll_result > 0 && (chat_pollfds[i].revents & POLLOUT) != 0)
            {
                // The pin keeps this first job and its socket alive while the
                // mutex is released. Even a batch of hundreds of ready sockets
                // must not make application threads wait behind their sends.
                pthread_mutex_unlock(&chat_mutex);
                write_ok = _ws_internal__chat_write_piece(queue);
                pthread_mutex_lock(&chat_mutex);
                if (!write_ok)
                {
                    _ws_internal__chat_collect(&retired, _ws_internal__chat_abort_locked(queue));
                }
            }
            queue->writer_active = false;
            if (queue->stopped)
            {
                _ws_internal__chat_collect(&retired, _ws_internal__chat_abort_locked(queue));
                continue;
            }
            if (job->written == job->header_size + job->size)
            {
                if (job->type == WS_FR_OP_CLSE)
                {
                    queue->close_deadline = audio_clock_ms() + TIMEOUT_MS;
                    pthread_cond_broadcast(&chat_cleanup_changed);
                }
                _ws_internal__chat_collect(&retired, _ws_internal__chat_remove_first(queue));
                if (queue->first != NULL && !queue->first->ready && !queue->first->encoding)
                {
                    pthread_cond_signal(&chat_encode_ready);
                }
            }
        }
        if (poll_result < 0)
        {
            _ws_internal__chat_wait_briefly(&chat_write_ready);
        }
        pthread_mutex_unlock(&chat_mutex);
        _ws_internal__chat_release_list(retired);
    }
}

/**
 * @brief Allocates shared socket lists and starts the chat encoding and writing
 * threads, releasing those resources if startup fails. chat_workers_running
 * becomes true only after every thread starts successfully.
 *
 * @return void
 *
 * @attention Call only through chat_once so concurrent connections cannot start
 * duplicate workers or observe partially initialized shared resources.
 */
static void _ws_internal__chat_initialize_workers(void)
{
    size_t created = 0;

    chat_pollfds = calloc(client_capacity, sizeof(*chat_pollfds));
    chat_polled_queues = calloc(client_capacity, sizeof(*chat_polled_queues));
    if (chat_pollfds == NULL || chat_polled_queues == NULL)
    {
        goto failed;
    }
    for (created = 0; created < WS_CHAT_WORKERS; created++)
    {
        if (pthread_create(&chat_workers[created], NULL, _ws_internal__chat_encode_worker, NULL) != 0)
        {
            goto failed;
        }
    }
    if (pthread_create(&chat_writer, NULL, _ws_internal__chat_socket_writer, NULL) != 0)
    {
        goto failed;
    }
    chat_workers_running = true;
    return;

failed:
    pthread_mutex_lock(&chat_mutex);
    chat_workers_stopping = true;
    pthread_cond_broadcast(&chat_encode_ready);
    pthread_cond_broadcast(&chat_write_ready);
    pthread_cond_broadcast(&chat_cleanup_changed);
    pthread_mutex_unlock(&chat_mutex);
    while (created > 0)
    {
        created--;
        pthread_join(chat_workers[created], NULL);
    }
    free(chat_pollfds);
    free(chat_polled_queues);
    chat_pollfds = NULL;
    chat_polled_queues = NULL;
}

/**
 * @brief Creates an outgoing queue for a chat connection and configures its
 * socket so writes never wait for the receiver.
 *
 * @param ws_cli_conn_t* client -> Connection whose WebSocket handshake has completed.
 *
 * @return int -> 0 when the queue and socket are ready, or -1 on startup failure.
 *
 * @attention The connection's reader calls this before publishing the open
 * connection to the application. Audio and video connections use their own path.
 */
static int _ws_internal__chat_start(ws_cli_conn_t* client)
{
    struct ws_chat_queue* queue;
    int enabled = 1;
#ifdef _WIN32
    u_long nonblocking = 1;
#else
    int flags;
#endif

    pthread_once(&chat_once, _ws_internal__chat_initialize_workers);
    if (!chat_workers_running)
    {
        return -1;
    }
    queue = calloc(1, sizeof(*queue));
    if (queue == NULL)
    {
        return -1;
    }
#ifdef _WIN32
    if (ioctlsocket(client->client_sock, FIONBIO, &nonblocking) != 0)
#else
    flags = fcntl(client->client_sock, F_GETFL, 0);
    if (flags < 0 || fcntl(client->client_sock, F_SETFL, flags | O_NONBLOCK) != 0)
#endif
    {
        free(queue);
        return -1;
    }
    setsockopt(client->client_sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&enabled, sizeof(enabled));
    queue->client = client;
    pthread_mutex_lock(&chat_mutex);
    client->chat_mode = true;
    client->chat = queue;
    if (chat_last != NULL)
    {
        chat_last->next = queue;
    }
    else
    {
        chat_first = queue;
    }
    chat_last = queue;
    pthread_mutex_unlock(&chat_mutex);
    return 0;
}

/**
 * @brief Stops a connection's outgoing queue and waits until no worker can still
 * use its messages or socket before freeing the queue.
 *
 * @param ws_cli_conn_t* client -> Connection whose reader has finished processing incoming data.
 *
 * @return void
 *
 * @attention Only the connection's reader performs this cleanup, outside all
 * application locks. A queued close is allowed a bounded time to be sent before
 * the remaining work is cancelled and the socket slot can be reused safely.
 */
static void _ws_internal__chat_stop(ws_cli_conn_t* client)
{
    struct ws_chat_queue* queue;
    struct ws_chat_queue* previous;
    struct ws_chat_queue* current;
    struct ws_chat_job* cancelled;

    pthread_mutex_lock(&chat_mutex);
    queue = client->chat;
    if (queue == NULL)
    {
        pthread_mutex_unlock(&chat_mutex);
        return;
    }
    // A received close gets its reply, and a queued rejection gets its close,
    // before teardown. This wait is outside the application's onclose callback.
    while (!queue->stopped && queue->close_requested && queue->first != NULL
        && audio_clock_ms() < queue->close_deadline)
    {
        _ws_internal__chat_wait_briefly(&chat_cleanup_changed);
    }
    cancelled = _ws_internal__chat_abort_locked(queue);
    pthread_mutex_unlock(&chat_mutex);
    _ws_internal__chat_release_list(cancelled);

    pthread_mutex_lock(&chat_mutex);
    while (queue->messages != 0 || queue->writer_active)
    {
        pthread_cond_wait(&chat_cleanup_changed, &chat_mutex);
    }
    previous = NULL;
    current = chat_first;
    while (current != queue)
    {
        previous = current;
        current = current->next;
    }
    if (previous != NULL)
    {
        previous->next = queue->next;
    }
    else
    {
        chat_first = queue->next;
    }
    if (chat_last == queue)
    {
        chat_last = previous;
    }
    client->chat = NULL;
    pthread_mutex_unlock(&chat_mutex);
    free(queue);
}

/**
 * @brief Returns the current client state for a given
 * client @p client.
 *
 * @param client Client structure.
 *
 * @return Returns the client state, -1 otherwise.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
static int get_client_state(ws_cli_conn_t *client)
{
	int state;

	if (!CLIENT_VALID(client))
	{
		return (-1);
	}

	pthread_mutex_lock(&client->mtx_state);
	state = client->state;
	pthread_mutex_unlock(&client->mtx_state);
    if (state == WS_STATE_OPEN && client->chat_mode)
    {
        // A reader can be waiting for a login turn when another thread aborts
        // its socket. Report that abort immediately, before recv() runs again.
        pthread_mutex_lock(&chat_mutex);
        if (client->chat == NULL || client->chat->stopped)
        {
            state = WS_STATE_CLOSING;
        }
        pthread_mutex_unlock(&chat_mutex);
    }
	return (state);
}

/**
 * @brief Set a state @p state to the client index
 * @p client.
 *
 * @param client Client structure.
 * @param state State to be set.
 *
 * @return Returns 0 if success, -1 otherwise.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
static int set_client_state(ws_cli_conn_t *client, int state)
{
	if (!CLIENT_VALID(client))
	{
		return (-1);
	}

	if (state < 0 || state > 3)
	{
		return (-1);
	}

	pthread_mutex_lock(&client->mtx_state);
	client->state = state;
	pthread_mutex_unlock(&client->mtx_state);
	return (0);
}

/**
 * @brief Send a given message @p buf on a socket @p sockfd.
 *
 * @param client Target client.
 * @param buf Message to be sent.
 * @param len Message length.
 * @param flags Send flags.
 *
 * @return If success (i.e: all message was sent), returns
 * the amount of bytes sent. Otherwise, -1.
 *
 * @note Technically this shouldn't be necessary, since send() should
 * block until all content is sent, since _we_ don't use 'O_NONBLOCK'.
 * However, it was reported (issue #22 on GitHub) that this was
 * happening, so just to be cautious, I will keep using this routine.
 */
static ssize_t send_all(ws_cli_conn_t *client, const void *buf, size_t len, int flags)
{
	const char *p;
	ssize_t ret;
	ssize_t r;

	ret = 0;

	/* Sanity check. */
	if (!CLIENT_VALID(client))
	{
		return (-1);
	}

	p = buf;
	/* clang-format off */
	pthread_mutex_lock(&client->mtx_snd);
		while (len)
		{
			r = send(client->client_sock, p, len, flags);
			if (r <= 0)
			{
                // The receiver may have only part of this frame. It must reconnect
                // before receiving more messages, including changes to the user list.
                ws_abort_client(client);
				pthread_mutex_unlock(&client->mtx_snd);
				return (-1);
			}
			p   += r;
			len -= r;
			ret += r;
		}
	pthread_mutex_unlock(&client->mtx_snd);
	/* clang-format on */
	return (ret);
}

/**
 * @brief Close client connection (no close handshake, this should
 * be done earlier), set appropriate state and destroy mutexes.
 *
 * @param client Client connection.
 * @param lock Should lock the global mutex?.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
static void* media_writer(void* argument)
{
    ws_cli_conn_t* client = argument;
    struct ws_media_buffer* media = client->media;
    unsigned char frame[WS_VIDEO_FRAME_MAX + 4];
    unsigned char keepalive[2];
    struct timespec deadline;
    size_t header_size = 0;
    uint64_t queued_at = 0;
    size_t size = 0;
    int kind = 0;
    uint64_t stale_after_ms = 0;
    uint64_t now = 0;
    uint64_t wait_ms = 0;

    for (;;)
    {
        pthread_mutex_lock(&media->mutex);
        while (media->used == 0 && !media->stopping)
        {
            wait_ms = WS_AUDIO_KEEPALIVE_INTERVAL_MS;
            if (media->is_bound)
            {
                now = audio_clock_ms();
                if (now >= media->next_keepalive_at)
                {
                    break;
                }
                wait_ms = media->next_keepalive_at - now;
            }
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += wait_ms / 1000;
            deadline.tv_nsec += (wait_ms % 1000) * 1000000;
            if (deadline.tv_nsec >= 1000000000)
            {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&media->ready, &media->mutex, &deadline);
        }
        if (media->stopping)
        {
            pthread_mutex_unlock(&media->mutex);
            break;
        }
        now = audio_clock_ms();
        if (media->is_bound && now >= media->next_keepalive_at)
        {
            // Check the deadline even when media is queued: a listener otherwise sends no reply.
            media->next_keepalive_at = now + WS_AUDIO_KEEPALIVE_INTERVAL_MS;
            pthread_mutex_unlock(&media->mutex);
            keepalive[0] = WS_FIN | WS_FR_OP_BIN;
            keepalive[1] = 0;
            if (SEND(client, keepalive, 2) < 0)
            {
                ws_abort_client(client);
                return NULL;
            }
            continue;
        }
        // the oldest record leaves the ring; the websocket frame is built around its payload
        media_ring_header(media, media->head, &queued_at, &size, &kind);
        header_size = size < 126 ? 2 : 4;
        media_ring_read(media, media->head + WS_MEDIA_RECORD_HEADER, frame + header_size, size);
        media->head = (media->head + WS_MEDIA_RECORD_HEADER + size) % media->ring_bytes;
        media->used -= WS_MEDIA_RECORD_HEADER + size;
        pthread_mutex_unlock(&media->mutex);
        stale_after_ms = kind == WS_MEDIA_KIND_VOICE ? WS_MEDIA_STALE_VOICE_MS : kind == WS_MEDIA_KIND_MUSIC ? WS_MEDIA_STALE_MUSIC_MS : 0;
        if (stale_after_ms > 0 && audio_clock_ms() - queued_at > stale_after_ms)
        {
            continue;
        }
        frame[0] = WS_FIN | WS_FR_OP_BIN;
        frame[1] = size < 126 ? (unsigned char)size : 126;
        if (header_size == 4)
        {
            frame[2] = (unsigned char)(size >> 8);
            frame[3] = (unsigned char)size;
        }
        if (SEND(client, frame, header_size + size) < 0)
        {
            // A partially sent frame cannot be skipped; terminate that connection.
            ws_abort_client(client);
            break;
        }
    }
    return NULL;
}

/** @brief gives a media socket its ring, receive buffer and writer thread, sized by its purpose */
static int media_start(ws_cli_conn_t* client)
{
    struct ws_media_buffer* media = calloc(1, sizeof(*media));

    if (media == NULL)
    {
        return -1;
    }
    media->ring_bytes = client->media_purpose == WS_MEDIA_PURPOSE_VIDEO ? WS_VIDEO_QUEUE_BYTES : WS_AUDIO_QUEUE_BYTES;
    media->frame_max = client->media_purpose == WS_MEDIA_PURPOSE_VIDEO ? WS_VIDEO_FRAME_MAX : WS_AUDIO_FRAME_MAX;
    media->ring = malloc(media->ring_bytes);
    media->received = malloc(media->frame_max + 1);
    if (media->ring == NULL || media->received == NULL)
    {
        free(media->ring);
        free(media->received);
        free(media);
        return -1;
    }
    if (pthread_mutex_init(&media->mutex, NULL) != 0)
    {
        free(media->ring);
        free(media->received);
        free(media);
        return -1;
    }
    if (pthread_cond_init(&media->ready, NULL) != 0)
    {
        pthread_mutex_destroy(&media->mutex);
        free(media->ring);
        free(media->received);
        free(media);
        return -1;
    }
    client->media = media;
    if (pthread_create(&media->writer, NULL, media_writer, client) != 0)
    {
        client->media = NULL;
        pthread_cond_destroy(&media->ready);
        pthread_mutex_destroy(&media->mutex);
        free(media->ring);
        free(media->received);
        free(media);
        return -1;
    }
    return 0;
}

static void media_stop(ws_cli_conn_t* client)
{
    struct ws_media_buffer* media = client->media;

    if (media == NULL)
    {
        return;
    }
    ws_abort_client(client);
    pthread_mutex_lock(&media->mutex);
    media->stopping = true;
    pthread_cond_signal(&media->ready);
    pthread_mutex_unlock(&media->mutex);
    pthread_join(media->writer, NULL);
    pthread_cond_destroy(&media->ready);
    pthread_mutex_destroy(&media->mutex);
    free(media->ring);
    free(media->received);
    free(media);
    client->media = NULL;
}

static void close_client(ws_cli_conn_t *client, int lock)
{
	if (!CLIENT_VALID(client))
	{
		return;
	}

	set_client_state(client, WS_STATE_CLOSED);

	close_socket(client->client_sock);

	/* Destroy client mutexes and clear fd 'slot'. */
	/* clang-format off */
	if (lock)
		pthread_mutex_lock(&mutex);
			client->client_sock = -1;
			pthread_cond_destroy(&client->cnd_state_close);
			pthread_mutex_destroy(&client->mtx_state);
			pthread_mutex_destroy(&client->mtx_snd);
			pthread_mutex_destroy(&client->mtx_ping);
	if (lock)
		pthread_mutex_unlock(&mutex);
	/* clang-format on */
}

/**
 * @brief Close time-out thread.
 *
 * For a given client, this routine sleeps until
 * TIMEOUT_MS and closes the connection or returns
 * sooner if already closed connection.
 *
 * @param p ws_connection/ws_cli_conn_t Structure Pointer.
 *
 * @return Always NULL.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
static void *close_timeout(void *p)
{
	ws_cli_conn_t *conn = p;
	struct timespec ts;
	int state;

	pthread_mutex_lock(&conn->mtx_state);

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += MS_TO_NS(TIMEOUT_MS);

	/* Normalize the time. */
	while (ts.tv_nsec >= 1000000000)
	{
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000;
	}

	while (conn->state != WS_STATE_CLOSED && pthread_cond_timedwait(&conn->cnd_state_close, &conn->mtx_state, &ts) != ETIMEDOUT)
		;

	state = conn->state;
	pthread_mutex_unlock(&conn->mtx_state);

	/* If already closed. */
	if (state == WS_STATE_CLOSED)
	{
		goto quit;
	}

	DEBUG_THELDUS_WEBSOCKET printf("%s", "[theldus-websocket] Timer expired, closing client \n");

    ws_abort_client(conn);
quit:
	return (NULL);
}

/**
 * @brief For a valid client index @p client, starts
 * the timeout thread and set the current state
 * to 'CLOSING'.
 *
 * @param client Client connection.
 *
 * @return Returns 0 if success, -1 otherwise.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
static int start_close_timeout(ws_cli_conn_t *client)
{
	if (!CLIENT_VALID(client))
	{
		return (-1);
	}

	pthread_mutex_lock(&client->mtx_state);

	if (client->state != WS_STATE_OPEN)
	{
		goto out;
	}

	client->state = WS_STATE_CLOSING;

	if (pthread_create(&client->thrd_tout, NULL, close_timeout, client))
	{
		pthread_mutex_unlock(&client->mtx_state);
		panic("Unable to create timeout thread\n");
	}
	client->close_thrd = true;
out:
	pthread_mutex_unlock(&client->mtx_state);
	return (0);
}

/**
 * @brief Sets the IP address relative to a client connection opened
 * by the server and save inside the client structure.
 *
 * @param client Client connection.
 */
static void set_client_address(ws_cli_conn_t *client)
{
	struct sockaddr_in addr;
	socklen_t addr_size;

	if (!CLIENT_VALID(client))
	{
		return;
	}

	addr_size = sizeof(struct sockaddr_in);

	if (getpeername(client->client_sock, (struct sockaddr *)&addr, &addr_size) < 0)
	{
		return;
	}



	memset(client->ip, 0, sizeof(client->ip));
	inet_ntop(AF_INET, &addr.sin_addr, client->ip, INET_ADDRSTRLEN);

	DEBUG_THELDUS_WEBSOCKET printf("%s %s %s", "[theldus-websocket] set_client_address (not forwarded)", client->ip, "\n");

}

/**
 * @brief Gets the IP address relative to a client connection opened
 * by the server.
 *
 * @param client Client connection.
 *
 * @return Pointer the ip address, or NULL if fails.
 *
 * @note The returned string is static, no need to free up memory.
 */
char *ws_getaddress(ws_cli_conn_t *client)
{
	if (!CLIENT_VALID(client))
	{
		DEBUG_THELDUS_WEBSOCKET printf("%s", "[theldus-websocket] ws_getaddress!CLIENT_VALID \n");

		return (NULL);
	}

	if (client->is_using_forwarded_ip_address)
	{
		DEBUG_THELDUS_WEBSOCKET printf("%s", "[theldus-websocket] ws_getaddress returning forwarded address \n");
		return client->forwarded_ip_address;
	}
	else
	{
		DEBUG_THELDUS_WEBSOCKET printf("%s", "[theldus-websocket] ws_getaddress returning real address \n");
		return client->ip;
	}
}

/**
 * @brief Creates and send an WebSocket frame with some payload data.
 *
 * This routine is intended to be used to create a websocket frame for
 * a given type e sending to the client. For higher level routines,
 * please check @ref ws_sendframe_txt and @ref ws_sendframe_bin.
 *
 * @param client Target to be send. If NULL, broadcast the message.
 * @param msg    Message to be send.
 * @param size   Binary message size.
 * @param type   Frame type.
 *
 * @return Returns the number of bytes written, -1 if error.
 *
 * @note If @p size is -1, it is assumed that a text frame is being sent,
 * otherwise, a binary frame. In the later case, the @p size is used.
 */
int ws_sendframe(ws_cli_conn_t *client, const char *msg, uint64_t size, int type)
{
	unsigned char *response; /* Response data.     */
	unsigned char frame[10]; /* Frame.             */
	uint8_t idx_first_rData; /* Index data.        */
	uint64_t length; /* Message length.    */
	int idx_response; /* Index response.    */
	ssize_t output; /* Bytes sent.        */
	ssize_t send_ret; /* Ret send function  */
	uint64_t i; /* Loop index.        */
	ws_cli_conn_t *cli; /* Client.            */

    if (client != NULL && client->chat_mode)
    {
        if (size > SIZE_MAX)
        {
            ws_abort_client(client);
            return -1;
        }
        return _ws_internal__chat_queue_frame(client, msg, (size_t)size, type);
    }

	frame[0] = (WS_FIN | type);
	length = (uint64_t)size;

	/* Split the size between octets. */
	if (length <= 125)
	{
		frame[1] = length & 0x7F;
		idx_first_rData = 2;
	}

	/* Size between 126 and 65535 bytes. */
	else if (length >= 126 && length <= 65535)
	{
		frame[1] = 126;
		frame[2] = (length >> 8) & 255;
		frame[3] = length & 255;
		idx_first_rData = 4;
	}

	/* More than 65535 bytes. */
	else
	{
		frame[1] = 127;
		frame[2] = (unsigned char)((length >> 56) & 255);
		frame[3] = (unsigned char)((length >> 48) & 255);
		frame[4] = (unsigned char)((length >> 40) & 255);
		frame[5] = (unsigned char)((length >> 32) & 255);
		frame[6] = (unsigned char)((length >> 24) & 255);
		frame[7] = (unsigned char)((length >> 16) & 255);
		frame[8] = (unsigned char)((length >> 8) & 255);
		frame[9] = (unsigned char)(length & 255);
		idx_first_rData = 10;
	}

	/* Add frame bytes. */
	idx_response = 0;
	response = calloc(sizeof(unsigned char) * (idx_first_rData + length + 1), 1);
	if (!response)
	{
		return (-1);
	}

	for (i = 0; i < idx_first_rData; i++)
	{
		response[i] = frame[i];
		idx_response++;
	}

	/* Add data bytes. */
	for (i = 0; i < length; i++)
	{
		response[idx_response] = msg[i];
		idx_response++;
	}

	response[idx_response] = '\0';

	/* Send to the client if there is one. */
	output = 0;
	if (client)
	{
		output = SEND(client, response, idx_response);
	}

	/* If no client specified, broadcast to everyone. */
	if (!client)
	{
		pthread_mutex_lock(&mutex);

		for (i = 0; i < client_capacity; i++)
		{
			cli = &client_socks[i];
			if ((cli->client_sock > -1) && get_client_state(cli) == WS_STATE_OPEN)
			{
				if ((send_ret = cli->chat_mode
                    ? _ws_internal__chat_queue_frame(cli, msg, (size_t)size, type)
                    : SEND(cli, response, idx_response)) != -1)
				{
					output += send_ret;
				}
				else
				{
					output = -1;
					break;
				}
			}
		}
		pthread_mutex_unlock(&mutex);
	}

	free(response);
	return ((int)output);
}

/**
 * @brief Given a PONG message, decodes the content
 * as a int32_t number that corresponds to our
 * PONG id.
 *
 * @param msg Content to be decoded.
 *
 * @return Returns the PONG id.
 */
static inline int32_t pong_msg_to_int32(uint8_t *msg)
{
	int32_t pong_id;
	/* Decodes as big-endian. */
	pong_id = (msg[3] << 0) | (msg[2] << 8) | (msg[1] << 16) | (msg[0] << 24);
	return (pong_id);
}

/**
 * @brief Given a PING id, encodes the content to be sent
 * as payload of a PING frame.
 *
 * @param ping_id PING id to be encoded.
 * @param msg Target buffer.
 */
static inline void int32_to_ping_msg(int32_t ping_id, uint8_t *msg)
{
	/* Encodes as big-endian. */
	msg[0] = (ping_id >> 24);
	msg[1] = (ping_id >> 16);
	msg[2] = (ping_id >> 8);
	msg[3] = (ping_id >> 0);
}

/**
 * @brief Send a ping message and close if the client surpasses
 * the threshold imposed.
 *
 * @param cli Client to be sent.
 * @param threshold How many pings can miss?.
 * @param lock Should lock global mutex or not?.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
static void send_ping_close(ws_cli_conn_t *cli, int threshold, int lock)
{
	uint8_t ping_msg[4];

	/* kept for API compatibility; the timeout path no longer needs the global
	 * mutex because it only shuts the socket down (see below) */
	(void)lock;

	if (!CLIENT_VALID(cli) || get_client_state(cli) != WS_STATE_OPEN)
	{
		return;
	}

	/* clang-format off */
	pthread_mutex_lock(&cli->mtx_ping);

		cli->current_ping_id++;
		int32_to_ping_msg(cli->current_ping_id, ping_msg);

		/* Send PING. */
		ws_sendframe(cli, (const char*)ping_msg, sizeof(ping_msg), WS_FR_OP_PING);

		/* Check previous PONG: if greater than threshold, abort.
		 *
		 * DO NOT call close_client() here: it destroys this client's mutexes -
		 * including mtx_ping, which this very function is still holding - and
		 * rips the state out from under the connection thread (blocked in
		 * recv) and any concurrent sender. Both are undefined behavior; in
		 * practice the first real timeout crashed the whole server.
		 *
		 * Only shut the socket down instead. The client's own connection
		 * thread wakes from recv(), leaves its read loop, fires onclose and
		 * runs the normal close_client() teardown on its own thread, exactly
		 * like a protocol-level close. Repeat shutdowns from later ping
		 * rounds (until that thread has cleaned the slot) are harmless no-ops. */
		if ((cli->current_ping_id - cli->last_pong_id) > threshold)
        {
            ws_abort_client(cli);
        }

	pthread_mutex_unlock(&cli->mtx_ping);
	/* clang-format on */
}

/**
 * @brief Sends a PING frame to the client @p cli with threshold
 * @p threshold.
 *
 * This routine sends a PING to a single client pointed to by
 * @p cli or a broadcast PING if @p cli is NULL. If the specified
 * client does not respond up to @p threshold PINGs, the connection
 * is aborted.
 *
 * ws_ping() is not automatic: the user who wants to send keep-alive
 * PINGs *must* call this routine in a timely manner, whether on
 * a different thread or inside an event.
 *
 * See examples/ping/ping.c for a minimal example usage.
 *
 * @param cli       Client to be sent, if NULL, broadcast.
 * @param threshold How many ignored PINGs should tolerate? (should be
 *                  positive and greater than 0).
 *
 * @note It should be noted that the time between each call to
 * ws_ping() is the timeout itself for receiving the PONG.
 *
 * It is also important to note that for devices with unstable
 * connections (such as a weak WiFi signal or 3/4/5G from a cell phone),
 * a threshold greater than 1 is advisable.
 */
void ws_ping(ws_cli_conn_t *cli, int threshold)
{
	int i;

	/* Sanity check. */
	if (threshold <= 0)
	{
		return;
	}

	/* PING a single client. */
	if (cli)
	{
		send_ping_close(cli, threshold, 1);
	}

	/* PING broadcast. */
	else
	{
		/* clang-format off */
		pthread_mutex_lock(&mutex);
			for (i = 0; i < client_capacity; i++)
				send_ping_close(&client_socks[i], threshold, 0
);
		pthread_mutex_unlock(&mutex);
		/* clang-format on */
	}
}

/**
 * @brief Sends a WebSocket text frame.
 *
 * @param client Target to be send. If NULL, broadcast the message.
 * @param msg    Message to be send, null terminated.
 *
 * @return Returns the number of bytes written, -1 if error.
 */
int ws_sendframe_txt(ws_cli_conn_t *client, const char *msg)
{
	//printf("%s %s", msg, "\n");
	//Sleep(5000);
	return ws_sendframe(client, msg, (uint64_t)strlen(msg), WS_FR_OP_TXT);
}

/**
 * @brief Sends a WebSocket binary frame.
 *
 * @param client Target to be send. If NULL, broadcast the message.
 * @param msg    Message to be send.
 * @param size   Binary message size.
 *
 * @return Returns the number of bytes written, -1 if error.
 */
int ws_sendframe_bin(ws_cli_conn_t *client, const char *msg, uint64_t size)
{
	return ws_sendframe(client, msg, size, WS_FR_OP_BIN);
}

/**
 * @brief For a given @p client, gets the current state for
 * the connection, or -1 if invalid.
 *
 * @param client Client connection.
 *
 * @return Returns the connection state or -1 if
 * invalid @p client.
 *
 * @see WS_STATE_CONNECTING
 * @see WS_STATE_OPEN
 * @see WS_STATE_CLOSING
 * @see WS_STATE_CLOSED
 */
int ws_get_state(ws_cli_conn_t *client)
{
	return (get_client_state(client));
}

/**
 * @brief Close the client connection for the given @p
 * client with normal close code (1000) and no reason
 * string.
 *
 * @param client Client connection.
 *
 * @return Returns 0 on success, -1 otherwise.
 *
 * @note If the client did not send a close frame in
 * TIMEOUT_MS milliseconds, the server will close the
 * connection with error code (1002).
 */
int ws_close_client(ws_cli_conn_t *client)
{
	unsigned char clse_code[2];
	int cc;

	/* Check if client is a valid and connected client. */
	if (!CLIENT_VALID(client) || client->client_sock == -1)
	{
		return (-1);
	}

	/*
	 * Instead of using do_close(), we use this to avoid using
	 * msg_ctrl buffer from wfd and avoid a race condition
	 * if this is invoked asynchronously.
	 */
	cc = WS_CLSE_NORMAL;
	clse_code[0] = (cc >> 8);
	clse_code[1] = (cc & 0xFF);
	if (ws_sendframe(client, (const char *)clse_code, sizeof(char) * 2, WS_FR_OP_CLSE) < 0)
	{
		DEBUG_THELDUS_WEBSOCKET printf("[theldus-websocket] An error has occurred while sending closing frame!\n");
		return (-1);
	}

    if (client->chat_mode)
    {
        // The writer starts the peer's close-reply timeout after the preceding
        // messages and the close frame have actually left this queue.
        set_client_state(client, WS_STATE_CLOSING);
        return 0;
    }

	/*
	 * Starts the timeout thread: if the client did not send
	 * a close frame in TIMEOUT_MS milliseconds, the server
	 * will close the connection with error code (1002).
	 */
	start_close_timeout(client);
	return (0);
}

/**
 * @brief Checks is a given opcode @p frame
 * belongs to a control frame or not.
 *
 * @param frame Frame opcode to be checked.
 *
 * @return Returns 1 if is a control frame, 0 otherwise.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
static inline int is_control_frame(int frame)
{
	return (frame == WS_FR_OP_CLSE || frame == WS_FR_OP_PING || frame == WS_FR_OP_PONG);
}

static int do_handshake(struct ws_frame_data *wfd)
{
	char *response; /* Handshake response message. */
	char *p; /* Last request line pointer.  */
	ssize_t n; /* Read/Write bytes.           */

	/* A socket is not visible to the application's auth timeout until onopen.
	 * Bound this initial read, then remove the timeout for established clients. */
	#ifndef AFL_FUZZ
	if (set_socket_timeout(wfd->client->client_sock, SO_RCVTIMEO, 10000) != 0)
	{
		return (-1);
	}
	#endif
	n = RECV(wfd->client, wfd->frm, sizeof(wfd->frm) - 1);
	if (n <= 0
#ifndef AFL_FUZZ
        || set_socket_timeout(wfd->client->client_sock, SO_RCVTIMEO, 0) != 0
#endif
       )
	{
		return (-1);
	}

	/* Advance our pointers before the first next_byte(). */
	p = strstr((const char *)wfd->frm, "\r\n\r\n");
	if (p == NULL)
	{
		DEBUG_THELDUS_WEBSOCKET printf("[theldus-websocket] An empty line with \\r\\n was expected!\n");
		return (-1);
	}
	wfd->amt_read = n;
	wfd->cur_pos = (size_t)((ptrdiff_t)(p - (char *)wfd->frm)) + 4;

	char *forwarded_ip_address = 0;
	/* Only trust the X-Stunnel-Client-IP header when the connection comes from
	 * our local stunnel front-end (127.0.0.1). A direct (non-stunnel) client
	 * must not be able to spoof its address, so for those we ignore the header
	 * and fall back to the real socket address below. */
	if (strcmp(wfd->client->ip, "127.0.0.1") == 0)
	{
		//get forwarded ip address that stunnel sent
		forwarded_ip_address = get_forwarded_ip_address((char *)wfd->frm);
	}

	if (forwarded_ip_address != 0)
	{
		wfd->client->is_using_forwarded_ip_address = true;
		memset((void *)&wfd->client->forwarded_ip_address[0], 0, INET6_ADDRSTRLEN);
		memcpy((void *)&wfd->client->forwarded_ip_address[0], forwarded_ip_address, strlen(forwarded_ip_address));

		DEBUG_THELDUS_WEBSOCKET printf("%s %s %s", "[theldus-websocket] forwarded_ip_address is", wfd->client->forwarded_ip_address, "\n");
		free(forwarded_ip_address);
	}
	else
	{
		wfd->client->is_using_forwarded_ip_address = false;
		DEBUG_THELDUS_WEBSOCKET printf("[theldus-websocket] failed to get forwarded ip address \n");
	}

	/* Get response. */
    wfd->client->media_purpose = WS_MEDIA_PURPOSE_NONE;
    if (strncmp((char*)wfd->frm, "GET /audio HTTP/1.1\r\n", 21) == 0)
    {
        wfd->client->media_purpose = WS_MEDIA_PURPOSE_AUDIO;
    }
    else if (strncmp((char*)wfd->frm, "GET /video HTTP/1.1\r\n", 21) == 0)
    {
        wfd->client->media_purpose = WS_MEDIA_PURPOSE_VIDEO;
    }

	if (get_handshake_response((char *)wfd->frm, &response) < 0)
	{
		DEBUG_THELDUS_WEBSOCKET printf("[theldus-websocket] Cannot get handshake response, request was: %s\n", wfd->frm);
		return (-1);
	}

	/* Valid request. */
	DEBUG_THELDUS_WEBSOCKET printf("[theldus-websocket] Handshaked, response: \n"
				       "------------------------------------\n"
				       "%s"
				       "------------------------------------\n",
				       response);

	/* Send handshake. */
	if (SEND(wfd->client, response, strlen(response)) < 0)
	{
		free(response);
		DEBUG_THELDUS_WEBSOCKET printf("[theldus-websocket] As error has occurred while handshaking!\n");
		return (-1);
	}

	/* Trigger events and clean up buffers. */
    if ((wfd->client->media_purpose != WS_MEDIA_PURPOSE_NONE))
    {
        int enabled = 1;
        setsockopt(wfd->client->client_sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&enabled, sizeof(enabled));
        if (set_socket_timeout(wfd->client->client_sock, SO_RCVTIMEO, WS_AUDIO_HELLO_TIMEOUT_MS) != 0
            || set_socket_timeout(wfd->client->client_sock, SO_SNDTIMEO, 100) != 0
            || media_start(wfd->client) != 0)
        {
            free(response);
            return -1;
        }
    }
    else
    {
#ifndef AFL_FUZZ
        if (_ws_internal__chat_start(wfd->client) != 0)
        {
            free(response);
            return -1;
        }
#endif
    }
    set_client_state(wfd->client, WS_STATE_OPEN);
	cli_events.onopen(wfd->client);
	free(response);
	return (0);
}

/**
 * @brief Sends a close frame, accordingly with the @p close_code
 * or the message inside @p wfd.
 *
 * @param wfd Websocket Frame Data.
 * @param close_code Websocket close code.
 *
 * @return Returns 0 if success, a negative number otherwise.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
static int do_close(struct ws_frame_data *wfd, int close_code)
{
	int cc; /* Close code.           */

	/* If custom close-code. */
	if (close_code != -1)
	{
		cc = close_code;
		goto custom_close;
	}

	/* If empty or have a close reason, just re-send. */
	if (wfd->frame_size == 0 || wfd->frame_size > 2)
	{
		goto send;
	}

	/* Parse close code and check if valid, if not, we issue an protocol error.
	 */
	if (wfd->frame_size == 1)
	{
		cc = wfd->msg_ctrl[0];
	}
	else
	{
		cc = ((int)wfd->msg_ctrl[0]) << 8 | wfd->msg_ctrl[1];
	}

	/* Check if it's not valid, if so, we send a protocol error (1002). */
	if ((cc < 1000 || cc > 1003) && (cc < 1007 || cc > 1011) && (cc < 3000 || cc > 4999))
	{
		cc = WS_CLSE_PROTERR;

custom_close:
		wfd->msg_ctrl[0] = (cc >> 8);
		wfd->msg_ctrl[1] = (cc & 0xFF);

		if (ws_sendframe(wfd->client, (const char *)wfd->msg_ctrl, sizeof(char) * 2, WS_FR_OP_CLSE) < 0)
		{
			DEBUG_THELDUS_WEBSOCKET printf("[theldus-websocket] An error has occurred while sending closing frame!\n");
			return (-1);
		}
		return (0);
	}

	/* Send the data inside wfd->msg_ctrl. */
send:
	if (ws_sendframe(wfd->client, (const char *)wfd->msg_ctrl, wfd->frame_size, WS_FR_OP_CLSE) < 0)
	{
		DEBUG_THELDUS_WEBSOCKET printf("[theldus-websocket] An error has occurred while sending closing frame!\n");
		return (-1);
	}
	return (0);
}

/**
 * @brief Send a pong frame in response to a ping frame.
 *
 * Accordingly to the RFC, a pong frame must have the same
 * data payload as the ping frame, so we just send a
 * ordinary frame with PONG opcode.
 *
 * @param wfd Websocket frame data.
 *
 * @param frame_size Pong frame size.
 *
 * @return Returns 0 if success and a negative number
 * otherwise.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
static int do_pong(struct ws_frame_data *wfd, uint64_t frame_size)
{
	if (ws_sendframe(wfd->client, (const char *)wfd->msg_ctrl, frame_size, WS_FR_OP_PONG) < 0)
	{
		wfd->error = 1;
		DEBUG_THELDUS_WEBSOCKET printf("[theldus-websocket] An error has occurred while ponging!\n");
		return (-1);
	}
	return (0);
}

/**
 * @brief Read a chunk of bytes and return the next byte
 * belonging to the frame.
 *
 * @param wfd Websocket Frame Data.
 *
 * @return Returns the byte read, or -1 if error.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
static inline int next_byte(struct ws_frame_data *wfd)
{
	ssize_t n;

	/* If empty or full. */
	if (wfd->cur_pos == 0 || wfd->cur_pos == wfd->amt_read)
	{
		if ((n = RECV(wfd->client, wfd->frm, sizeof(wfd->frm))) <= 0)
		{
			wfd->error = 1;
			DEBUG_THELDUS_WEBSOCKET printf("[theldus-websocket] An error has occurred while trying to read next byte\n");
			return (-1);
		}
		wfd->amt_read = (size_t)n;
		wfd->cur_pos = 0;
	}
	return (wfd->frm[wfd->cur_pos++]);
}

/**
 * @brief Skips @p frame_size bytes of the current frame.
 *
 * @param wfd Websocket Frame Data.
 * @param frame_size Amount of bytes to be skipped.
 *
 * @return Returns 0 if success, a negative number
 * otherwise.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
static int skip_frame(struct ws_frame_data *wfd, uint64_t frame_size)
{
	uint64_t i;
	for (i = 0; i < frame_size; i++)
	{
		if (next_byte(wfd) == -1)
		{
			wfd->error = 1;
			return (-1);
		}
	}
	return (0);
}

/**
 * @brief Reads the current frame isolating data from control frames.
 * The parameters are changed in order to reflect the current state.
 *
 * @param wfd Websocket Frame Data.
 * @param opcode Frame opcode.
 * @param buf Buffer to be written.
 * @param frame_length Length of the current frame.
 * @param frame_size Total size of the frame (considering CONT frames)
 *                   read until the moment.
 * @param msg_idx Message index, reflects the current buffer pointer state.
 * @param masks Masks vector.
 * @param is_fin Is FIN frame indicator.
 *
 * @return Returns 0 if success, a negative number otherwise.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
static int read_frame(struct ws_frame_data *wfd, int opcode, unsigned char **buf, uint64_t *frame_length, uint64_t *frame_size, uint64_t *msg_idx, uint8_t *masks, int is_fin)
{
	unsigned char *tmp; /* Tmp message.     */
	unsigned char *msg; /* Current message. */
	int cur_byte; /* Curr byte read.  */
	uint64_t i; /* Loop index.      */
    uint64_t limit = wfd->client->media_purpose != WS_MEDIA_PURPOSE_NONE ? wfd->client->media->frame_max : MAX_FRAME_LENGTH;

	msg = *buf;
    if ((wfd->client->media_purpose != WS_MEDIA_PURPOSE_NONE) && !is_control_frame(opcode))
    {
        msg = wfd->client->media->received;
        *buf = msg;
    }

	/* Decode masks and length for 16-bit messages. */
	if (*frame_length == 126)
	{
		*frame_length = (((uint64_t)next_byte(wfd)) << 8) | next_byte(wfd);
	}

	/* 64-bit messages. */
	else if (*frame_length == 127)
	{
		*frame_length = (((uint64_t)next_byte(wfd)) << 56) | /* frame[2]. */
				(((uint64_t)next_byte(wfd)) << 48) | /* frame[3]. */
				(((uint64_t)next_byte(wfd)) << 40) | (((uint64_t)next_byte(wfd)) << 32) | (((uint64_t)next_byte(wfd)) << 24) | (((uint64_t)next_byte(wfd)) << 16) | (((uint64_t)next_byte(wfd)) << 8) | (((uint64_t)next_byte(wfd))); /* frame[9]. */
	}

	/*
	 * Check frame size
	 *
	 * We need to limit the amount supported here, since if
	 * we follow strictly to the RFC, we have to allow 2^64
	 * bytes. Also keep in mind that this is still true
	 * for continuation frames.
	 */
	/* Check BEFORE addition: a malicious 64-bit continuation length can wrap
	 * both the cumulative size and the allocation below to a tiny value. */
	if (*frame_size > limit || *frame_length > limit - *frame_size)
	{
		DEBUG_THELDUS_WEBSOCKET printf("[theldus-websocket] Current frame from client %d, exceeds the maximum\n"
					       "amount of bytes allowed (%" PRId64 "/%d)!",
					       wfd->client->client_sock, *frame_size + *frame_length, MAX_FRAME_LENGTH);

		wfd->error = 1;
		return (-1);
	}
	*frame_size += *frame_length;

	/* Read masks. */
	masks[0] = next_byte(wfd);
	masks[1] = next_byte(wfd);
	masks[2] = next_byte(wfd);
	masks[3] = next_byte(wfd);

	/*
	 * Abort if error.
	 *
	 * This is tricky: we may have multiples error codes from the
	 * previous next_bytes() calls, but, since we're only setting
	 * variables and flags, there is no major issue in setting
	 * them wrong _if_ we do not use their values, thing that
	 * we do here.
	 */
	if (wfd->error)
	{
		return (-1);
	}

	/*
	 * Allocate memory.
	 *
	 * The statement below will allocate a new chunk of memory
	 * if msg is NULL with size total_length. Otherwise, it will
	 * resize the total memory accordingly with the message index
	 * and if the current frame is a FIN frame or not, if so,
	 * increment the size by 1 to accommodate the line ending \0.
	 */
	if (*frame_length > 0)
	{
		if (!is_control_frame(opcode) && !(wfd->client->media_purpose != WS_MEDIA_PURPOSE_NONE))
		{
			tmp = realloc(msg, sizeof(unsigned char) * (*msg_idx + *frame_length + is_fin));
			if (!tmp)
			{
				DEBUG_THELDUS_WEBSOCKET printf("[theldus-websocket] Cannot allocate memory, requested: % " PRId64 "\n", (*msg_idx + *frame_length + is_fin));

				wfd->error = 1;
				return (-1);
			}
			msg = tmp;
			*buf = msg;
		}

		/* Copy to the proper location. */
		for (i = 0; i < *frame_length; i++, (*msg_idx)++)
		{
			/* We were able to read? .*/
			cur_byte = next_byte(wfd);
			if (cur_byte == -1)
			{
				return (-1);
			}

			msg[*msg_idx] = cur_byte ^ masks[i % 4];
		}
	}

	/* If we're inside a FIN frame, lets... */
	if (is_fin && *frame_size > 0)
	{
		/* Increase memory if our FIN frame is of length 0. */
		if (!*frame_length && !is_control_frame(opcode) && !(wfd->client->media_purpose != WS_MEDIA_PURPOSE_NONE))
		{
			tmp = realloc(msg, sizeof(unsigned char) * (*msg_idx + 1));
			if (!tmp)
			{
				DEBUG_THELDUS_WEBSOCKET printf("[theldus-websocket] Cannot allocate memory, requested: %" PRId64 "\n", (*msg_idx + 1));

				wfd->error = 1;
				return (-1);
			}
			msg = tmp;
			*buf = msg;
		}
		msg[*msg_idx] = '\0';
	}

	return (0);
}

/**
 * @brief Reads the next frame, whether if a TXT/BIN/CLOSE
 * of arbitrary size.
 *
 * @param wfd Websocket Frame Data.
 *
 * @return Returns 0 if success, a negative number otherwise.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
static int next_frame(struct ws_frame_data *wfd)
{
	unsigned char *msg_data; /* Data frame.                */
	unsigned char *msg_ctrl; /* Control frame.             */
	uint8_t masks_data[4]; /* Masks data frame array.    */
	uint8_t masks_ctrl[4]; /* Masks control frame array. */
	uint64_t msg_idx_data; /* Current msg index.         */
	uint64_t msg_idx_ctrl; /* Current msg index.         */
	uint64_t frame_length; /* Frame length.              */
	uint64_t frame_size; /* Current frame size.        */
#ifdef VALIDATE_UTF8
	uint32_t utf8_state; /* Current UTF-8 state.       */
#endif
	int32_t pong_id; /* Current PONG id.           */
	uint8_t opcode; /* Frame opcode.              */
	uint8_t is_fin; /* Is FIN frame flag.         */
	uint8_t mask; /* Mask.                      */
	int cur_byte; /* Current frame byte.        */

	msg_data = NULL;
	msg_ctrl = wfd->msg_ctrl;
	is_fin = 0;
	frame_length = 0;
	frame_size = 0;
	msg_idx_data = 0;
	msg_idx_ctrl = 0;
	wfd->frame_size = 0;
	wfd->frame_type = -1;
	wfd->msg = NULL;
#ifdef VALIDATE_UTF8
	utf8_state = UTF8_ACCEPT;
#endif

	/* Read until find a FIN or a unsupported frame. */
	do
	{
		/*
		 * Obs: next_byte() can return error if not possible to read the
		 * next frame byte, in this case, we return an error.
		 *
		 * However, please note that this check is only made here and in
		 * the subsequent next_bytes() calls this also may occur too.
		 * wsServer is assuming that the client only create right
		 * frames and we will do not have disconnections while reading
		 * the frame but just when waiting for a frame.
		 */
		cur_byte = next_byte(wfd);
		if (cur_byte == -1)
		{
			return (-1);
		}

		is_fin = (cur_byte & 0xFF) >> WS_FIN_SHIFT;
		opcode = (cur_byte & 0xF);

		/*
		 * Check for RSV field.
		 *
		 * Since wsServer do not negotiate extensions if we receive
		 * a RSV field, we must drop the connection.
		 */
		if (cur_byte & 0x70)
		{
			DEBUG_THELDUS_WEBSOCKET printf("[theldus-websocket] RSV is set while wsServer do not negotiate extensions!\n");
			wfd->error = 1;
			break;
		}

		/*
		 * Check if the current opcode makes sense:
		 * a) If we're inside a cont frame but no previous data frame
		 *
		 * b) If we're handling a data-frame and receive another data
		 *    frame. (it's expected to receive only CONT or control
		 *    frames).
		 *
		 * It is worth to note that in a), we do not need to check
		 * if the previous frame was FIN or not: if was FIN, an
		 * on_message event was triggered and this function returned;
		 * so the only possibility here is a previous non-FIN data
		 * frame, ;-).
		 */
		if ((wfd->frame_type == -1 && opcode == WS_FR_OP_CONT) || (wfd->frame_type != -1 && !is_control_frame(opcode) && opcode != WS_FR_OP_CONT))
		{
			DEBUG_THELDUS_WEBSOCKET printf("[theldus-websocket] Unexpected frame was received!, opcode: %d, previous: %d\n", opcode, wfd->frame_type);
			wfd->error = 1;
			break;
		}

		/* Check if one of the valid opcodes. */
		if (opcode == WS_FR_OP_TXT || opcode == WS_FR_OP_BIN || opcode == WS_FR_OP_CONT || opcode == WS_FR_OP_PING || opcode == WS_FR_OP_PONG || opcode == WS_FR_OP_CLSE)
		{
			/*
			 * Check our current state: if CLOSING, we only accept close
			 * frames.
			 *
			 * Since the server may, at any time, asynchronously, asks
			 * to close the client connection, we should terminate
			 * immediately.
			 */
			if (get_client_state(wfd->client) == WS_STATE_CLOSING && opcode != WS_FR_OP_CLSE)
			{
				DEBUG_THELDUS_WEBSOCKET printf("[theldus-websocket] Unexpected frame received, expected CLOSE (%d), "
							       "received: (%d)",
							       WS_FR_OP_CLSE, opcode);
				wfd->error = 1;
				break;
			}

			/* Only change frame type if not a CONT frame. */
			if (opcode != WS_FR_OP_CONT && !is_control_frame(opcode))
			{
				wfd->frame_type = opcode;
			}

			mask = next_byte(wfd);
			frame_length = mask & 0x7F;
			frame_size = 0;
			msg_idx_ctrl = 0;

			/*
			 * We should deny non-FIN control frames or that have
			 * more than 125 octets.
			 */
			if (is_control_frame(opcode) && (!is_fin || frame_length > 125))
			{
				DEBUG_THELDUS_WEBSOCKET printf("%s", "[theldus-websocket] Control frame bigger than 125 octets or not a FIN frame!\n");
				wfd->error = 1;
				break;
			}

			/* Normal data frames. */
			if (opcode == WS_FR_OP_TXT || opcode == WS_FR_OP_BIN || opcode == WS_FR_OP_CONT)
			{
				if (read_frame(wfd, opcode, &msg_data, &frame_length, &wfd->frame_size, &msg_idx_data, masks_data, is_fin) < 0)
				{
					break;
				}

#ifdef VALIDATE_UTF8
				/* UTF-8 Validate partial (or not) frame. */
				if (wfd->frame_type == WS_FR_OP_TXT)
				{
					if (is_fin)
					{
						if (is_utf8_len_state(msg_data + (msg_idx_data - frame_length), frame_length, utf8_state) != UTF8_ACCEPT)
						{
							DEBUG_THELDUS_WEBSOCKET printf("%s", "[theldus-websocket] Dropping invalid complete message!\n");
							wfd->error = 1;
							do_close(wfd, WS_CLSE_INVUTF8);
						}
					}

					/* Check current state for a CONT or initial TXT frame. */
					else
					{
						utf8_state = is_utf8_len_state(msg_data + (msg_idx_data - frame_length), frame_length, utf8_state);

						/* We can be in any state, except reject. */
						if (utf8_state == UTF8_REJECT)
						{
							DEBUG_THELDUS_WEBSOCKET printf("%s", "[theldus-websocket] Dropping invalid cont/initial frame!\n");
							wfd->error = 1;
							do_close(wfd, WS_CLSE_INVUTF8);
						}
					}
				}
#endif
			}

			/*
			 * We _may_ send a PING frame if the ws_ping() routine was invoked.
			 *
			 * If the content is invalid and/or differs the size, ignore it.
			 * (maybe unsolicited PONG).
			 */
			else if (opcode == WS_FR_OP_PONG)
			{
				if (read_frame(wfd, opcode, &msg_ctrl, &frame_length, &frame_size, &msg_idx_ctrl, masks_ctrl, is_fin) < 0)
				{
					break;
				}

				is_fin = 0;

				/* If there is no content and/or differs the size, ignore it. */
				if (frame_size != sizeof(wfd->client->last_pong_id))
				{
					continue;
				}

				/*
				 * Our PONG id should be positive and smaller than our
				 * current PING id. If not, ignore.
				 */
				/* clang-format off */
				pthread_mutex_lock(&wfd->client->mtx_ping);

					pong_id = pong_msg_to_int32(msg_ctrl);
					if (pong_id < 0 || pong_id > wfd->client->current_ping_id)
					{
						pthread_mutex_unlock(&wfd->client->mtx_ping);
						continue;
					}
					wfd->client->last_pong_id = pong_id;

				pthread_mutex_unlock(&wfd->client->mtx_ping);
				/* clang-format on */
				continue;
			}

			/* We should answer to a PING frame as soon as possible. */
			else if (opcode == WS_FR_OP_PING)
			{
				if (read_frame(wfd, opcode, &msg_ctrl, &frame_length, &frame_size, &msg_idx_ctrl, masks_ctrl, is_fin) < 0)
				{
					break;
				}

				if (do_pong(wfd, frame_size) < 0)
				{
					break;
				}

				/* Quick hack to keep our loop. */
				is_fin = 0;
			}

			/* We interrupt the loop as soon as we find a CLOSE frame. */
			else
			{
				if (read_frame(wfd, opcode, &msg_ctrl, &frame_length, &frame_size, &msg_idx_ctrl, masks_ctrl, is_fin) < 0)
				{
					break;
				}

#ifdef VALIDATE_UTF8
				/* If there is a close reason, check if it is UTF-8 valid. */
				if (frame_size > 2 && !is_utf8_len(msg_ctrl + 2, frame_size - 2))
				{
					DEBUG_THELDUS_WEBSOCKET printf("%s", "[theldus-websocket] Invalid close frame payload reason! (not UTF-8)\n");
					wfd->error = 1;
					break;
				}
#endif

				/* Since we're aborting, we can scratch the 'data'-related
				 * vars here. */
				wfd->frame_size = frame_size;
				wfd->frame_type = WS_FR_OP_CLSE;
				if (!(wfd->client->media_purpose != WS_MEDIA_PURPOSE_NONE))
                {
                    free(msg_data);
                }
				return (0);
			}
		}

		/* Anything else (unsupported frames). */
		else
		{
			DEBUG_THELDUS_WEBSOCKET printf("[theldus-websocket] Unsupported frame opcode: %d\n", opcode);
			/* We should consider as error receive an unknown frame. */
			wfd->frame_type = opcode;
			wfd->error = 1;
		}

	} while (!is_fin && !wfd->error);

	/* Check for error. */
	if (wfd->error)
	{
		if (!(wfd->client->media_purpose != WS_MEDIA_PURPOSE_NONE))
                {
                    free(msg_data);
                }
		wfd->msg = NULL;
		return (-1);
	}

	wfd->msg = msg_data;
	return (0);
}

/**
 * @brief Establishes to connection with the client and trigger
 * events when occurs one.
 *
 * @param vclient Client connection.
 *
 * @return Returns @p vclient.
 *
 * @note This will be run on a different thread.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
static void *ws_establishconnection(void *vclient)
{
	struct ws_frame_data wfd; /* WebSocket frame data.   */
	ws_cli_conn_t *client; /* Client structure.       */
	int clse_thrd; /* Time-out close thread.  */

	client = vclient;

	/* Prepare frame data. */
	memset(&wfd, 0, sizeof(wfd));
	wfd.client = client;

	/* Do handshake. */
	if (do_handshake(&wfd) < 0)
	{
		goto closed;
	}

	/* Read next frame until client disconnects or an error occur. */
	while (next_frame(&wfd) >= 0)
	{
		/* Text/binary event. */
		if ((wfd.frame_type == WS_FR_OP_TXT || wfd.frame_type == WS_FR_OP_BIN) && !wfd.error)
		{
			cli_events.onmessage(client, wfd.msg, wfd.frame_size, wfd.frame_type);
		}

		/* Close event. */
		else if (wfd.frame_type == WS_FR_OP_CLSE && !wfd.error)
		{
			/*
			 * We only send a CLOSE frame once, if we're already
			 * in CLOSING state, there is no need to send.
			 */
			if (get_client_state(client) != WS_STATE_CLOSING)
			{
				set_client_state(client, WS_STATE_CLOSING);

				/* We only send a close frameSend close frame */
				do_close(&wfd, -1);
			}

			if (client->media_purpose == WS_MEDIA_PURPOSE_NONE)
            {
                free(wfd.msg);
            }
			break;
		}

		if (client->media_purpose == WS_MEDIA_PURPOSE_NONE)
            {
                free(wfd.msg);
            }
	}

	/*
	 * on_close events always occur, whether for client closure
	 * or server closure, as the server is expected to
	 * always know when the client disconnects.
	 */
	cli_events.onclose(client);

closed:
    _ws_internal__chat_stop(client);
    media_stop(client);
	clse_thrd = client->close_thrd;

	/* Wait for timeout thread if necessary. */
	if (clse_thrd)
	{
		pthread_cond_signal(&client->cnd_state_close);
		pthread_join(client->thrd_tout, NULL);
	}

	/* Close connectin properly. */
	if (get_client_state(client) != WS_STATE_CLOSED)
	{
		close_client(client, 1);
	}

	return (vclient);
}

/**
 * @brief Main loop that keeps accepting new connections.
 *
 * @param data Server socket.
 *
 * @return Returns @p data.
 *
 * @note This may be run on a different thread.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
static void *ws_accept(void *data)
{
	struct sockaddr_in client; /* Client.                */
	pthread_t client_thread; /* Client thread.         */
	int new_sock; /* New opened connection. */
	int sock; /* Server sock.           */
	int len; /* Length of sockaddr.    */
	int i; /* Loop index.            */
#ifdef SO_NOSIGPIPE
    int no_sigpipe;
#endif

	sock = *(int *)data;
	len = sizeof(struct sockaddr_in);

	while (1)
	{
		DEBUG_THELDUS_WEBSOCKET printf("%s", "[theldus-websocket] ws_accept \n");
		/* Accept. */
		new_sock = accept(sock, (struct sockaddr *)&client, (socklen_t *)&len);
		if (new_sock < 0)
		{
			panic("[theldus-websocket] Error on accepting connections..");
		}

#ifdef SO_NOSIGPIPE
        // macOS uses this socket option instead of MSG_NOSIGNAL. A peer that
        // disconnects while a frame is being sent must not stop the server.
        no_sigpipe = 1;
        if (setsockopt(new_sock, SOL_SOCKET, SO_NOSIGPIPE,
            (const char*)&no_sigpipe, sizeof(no_sigpipe)) != 0)
        {
            close_socket(new_sock);
            continue;
        }
#endif

		if (timeout)
		{
			if (set_socket_timeout(new_sock, SO_SNDTIMEO, timeout) != 0)
			{
				close_socket(new_sock);
				continue;
			}
		}

		/* Adds client socket to socks list. */
		pthread_mutex_lock(&mutex);
		for (i = 0; i < client_capacity; i++)
		{
			if (client_socks[i].client_sock == -1)
			{
				client_socks[i].client_sock = new_sock;
				client_socks[i].state = WS_STATE_CONNECTING;
                client_socks[i].chat_mode = false;
                client_socks[i].media_purpose = WS_MEDIA_PURPOSE_NONE;
                client_socks[i].media = NULL;
                client_socks[i].media_client_id = UINT64_MAX;
				client_socks[i].close_thrd = false;
				client_socks[i].last_pong_id = -1;
				client_socks[i].current_ping_id = -1;
				set_client_address(&client_socks[i]);

				if (pthread_mutex_init(&client_socks[i].mtx_state, NULL))
				{
					panic("Error on allocating close mutex");
				}
				if (pthread_cond_init(&client_socks[i].cnd_state_close, NULL))
				{
					panic("Error on allocating condition var\n");
				}
				if (pthread_mutex_init(&client_socks[i].mtx_snd, NULL))
				{
					panic("Error on allocating send mutex");
				}
				if (pthread_mutex_init(&client_socks[i].mtx_ping, NULL))
				{
					panic("Error on allocating ping/pong mutex");
				}
				break;
			}
		}
		pthread_mutex_unlock(&mutex);

		/* Client socket added to socks list ? */
		if (i != client_capacity)
		{
			if (pthread_create(&client_thread, NULL, ws_establishconnection, &client_socks[i]))
			{
				panic("Could not create the client thread!");
			}

			pthread_detach(client_thread);
		}
		else
		{
			close_socket(new_sock);
		}
	}
	free(data);
	return (data);
}

/**
 * @brief Main loop for the server.
 *
 * @param evs  Events structure.
 * @param port Server port.
 * @param thread_loop If any value other than zero, runs
 *                    the accept loop in another thread
 *                    and immediately returns. If 0, runs
 *                    in the same thread and blocks execution.
 *
 * @param timeout_ms  Max timeout if the client is not responding
 *                    (in milliseconds).
 *
 * @return If @p thread_loop != 0, returns 0. Otherwise, never
 * returns.
 */
int ws_socket(struct ws_events *evs, uint16_t port, int thread_loop, uint32_t timeout_ms)
{
	struct sockaddr_in server; /* Server.                */
	pthread_t accept_thread; /* Accept thread.         */
	int reuse; /* Socket option.         */
	int *sock; /* Client sock.           */

	if (client_socks == NULL && ws_init_client_slots(MAX_CLIENTS) != 0)
	{
		panic("Unable to allocate websocket slots");
	}

	timeout = timeout_ms;

	/* Ignore 'unused functions' warnings. */
	((void)skip_frame);

	/* Checks if the event list is a valid pointer. */
	if (evs == NULL)
	{
		panic("Invalid event list!");
	}

	/* Allocates our sock data. */
	sock = calloc(sizeof(*sock), 1);
	if (!sock)
	{
		panic("Unable to allocate sock, out of memory!\n");
	}

	/* Copy events. */
	memcpy(&cli_events, evs, sizeof(struct ws_events));

#ifdef _WIN32
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
	{
		panic("WSAStartup failed!");
	}

	/**
	 * Sets stdout to be non-buffered.
	 *
	 * According to the docs from MSDN (setvbuf page), Windows do not
	 * really supports line buffering but full-buffering instead.
	 *
	 * Quote from the docs:
	 * "... _IOLBF For some systems, this provides line buffering.
	 *  However, for Win32, the behavior is the same as _IOFBF"
	 */
	setvbuf(stdout, NULL, _IONBF, 0);
#endif

	/* Create socket. */
	*sock = socket(AF_INET, SOCK_STREAM, 0);
	if (*sock < 0)
	{
		panic("[theldus-websocket] Could not create socket");
	}

	/* Reuse previous address. */
	reuse = 1;
	if (setsockopt(*sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse)) < 0)
	{
		panic("[theldus-websocket] setsockopt(SO_REUSEADDR) failed");
	}

	/* Prepare the sockaddr_in structure. */
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = INADDR_ANY;
	server.sin_port = htons(port);

	/* Bind. */
	if (bind(*sock, (struct sockaddr *)&server, sizeof(server)) < 0)
	{
		panic("[theldus-websocket] Bind failed");
	}

	/* Listen. */
	if (listen(*sock, SOMAXCONN) < 0)
	{
		panic("[theldus-websocket] Listen failed");
	}

	/* Wait for incoming connections. */
	DEBUG_THELDUS_WEBSOCKET printf("[theldus-websocket] Waiting for incoming connections..!.\n");


	/* Accept connections. */
	if (!thread_loop)
	{
		ws_accept((void *)sock);
	}
	else
	{
		if (pthread_create(&accept_thread, NULL, ws_accept, (void *)sock))
		{
			panic("[theldus-websocket] Could not create the client thread!");
		}
		pthread_detach(accept_thread);
	}

	DEBUG_THELDUS_WEBSOCKET printf("[theldus-websocket] websocket started \n");

	return (0);
}

#ifdef AFL_FUZZ
/**
 * @brief WebSocket fuzzy test routine
 *
 * @param evs  Events structure.
 *
 * @param file File to be read.
 *
 * @return Returns 0, or crash.
 *
 * @note This is a 'fuzzy version' of the function @ref ws_socket.
 * This routine do not listen to any port nor accept multiples
 * connections. It is intended to read a stream of frames through a
 * file and process it as if they are coming from a socket.
 *
 * This behavior enables us to test wsServer against fuzzers, like
 * AFL, and see if it crashes, hangs or behaves normally, even under
 * weird conditions.
 */
int ws_file(struct ws_events *evs, const char *file)
{
	int sock;
	sock = open(file, O_RDONLY);
	if (sock < 0)
	{
		panic("Invalid file\n");
	}

	/* Copy events. */
	memcpy(&cli_events, evs, sizeof(struct ws_events));

	/* Clear client socks list. */
	if (client_socks == NULL && ws_init_client_slots(1) != 0) { return -1; }

	/* Set client settings. */
	client_socks[0].client_sock = sock;
	client_socks[0].state = WS_STATE_CONNECTING;
	client_socks[0].close_thrd = false;

	/* Initialize mutexes. */
	if (pthread_mutex_init(&client_socks[0].mtx_state, NULL))
	{
		panic("Error on allocating close mutex");
	}
	if (pthread_cond_init(&client_socks[0].cnd_state_close, NULL))
	{
		panic("Error on allocating condition var\n");
	}
	if (pthread_mutex_init(&client_socks[0].mtx_snd, NULL))
	{
		panic("Error on allocating send mutex");
	}
	if (pthread_mutex_init(&client_socks[0].mtx_ping, NULL))
	{
		panic("Error on allocating ping/pong mutex");
	}

	ws_establishconnection(&client_socks[0]);
	return (0);
}
#endif
