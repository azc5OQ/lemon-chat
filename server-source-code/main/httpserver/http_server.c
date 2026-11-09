/* winsock2.h must be included before anything can pull in windows.h (via pthread/winpthreads), or it
   clashes with the legacy winsock.h that windows.h includes by default. */
#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "../definitions.h"

#include "http_server.h"
#include "http_gzip.h"
#include "../clib/clib.h"

#include "../../third-party/rxi-log/log.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* platform socket shims: a Windows SOCKET handle is unsigned (so "< 0" never detects an error) and is
   closed with closesocket(); Windows also has no MSG_NOSIGNAL (it never raises SIGPIPE). */
#ifdef WIN32
typedef SOCKET http_socket_t;
#define HTTP_INVALID_SOCKET INVALID_SOCKET
#define http_close_socket closesocket
#define HTTP_SEND_FLAGS 0
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
typedef int http_socket_t; // POSIX socket descriptor.
#define HTTP_INVALID_SOCKET (-1)
#define http_close_socket close
/* MSG_NOSIGNAL keeps send() to a closed peer from raising SIGPIPE (which would end the server process).
   Linux has the flag; macOS/BSD do not - there the SO_NOSIGPIPE socket option is set per connection
   in _http_server_internal__handle_connection instead, so the send flag is just 0 */
#ifdef MSG_NOSIGNAL
#define HTTP_SEND_FLAGS MSG_NOSIGNAL
#else
#define HTTP_SEND_FLAGS 0
#endif
#endif

#define HTTP_SERVER_REQUEST_BUFFER_SIZE 8192
#define HTTP_SERVER_PATH_SIZE 1024
#define HTTP_SERVER_WEBROOT_SIZE 512
#define HTTP_SERVER_SEND_CHUNK_SIZE 16384
#define HTTP_SERVER_HEADER_SIZE 512
#define HTTP_SERVER_RECV_TIMEOUT_SECONDS 10
#define HTTP_SERVER_SEND_TIMEOUT_SECONDS 10
// Keep room for a burst of new connections while their worker threads start.
// The separate worker limit below still bounds how many requests run at once.
#define HTTP_SERVER_LISTEN_BACKLOG 128
#define HTTP_SERVER_MAX_CONNECTIONS 32

// Each visitor gets a separate thread, so a browser that opens a connection
// and waits before sending its request does not stop other visitors. Keep a
// limit on those threads so waiting connections cannot use unlimited memory.
static pthread_mutex_t g_http_connections_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64 g_http_active_connections = 0;

/* Listener port and webroot are initialized once, including when HTTP starts disabled. */
static int64 g_http_server_port = 0;
static char *g_http_server_webroot = NULL_POINTER;

/* when TRUE, direct http visitors (not stunnel-forwarded https backend requests) are 301'd to https */
static boole g_http_https_redirect_enabled = FALSE;
static int64 g_http_https_redirect_port = 443;

/* window.__SERVER_CONFIG__ script (websocket port + connect keys) injected into the served client.html so it can autoconnect; empty until http_server__set_client_config runs */
static char *g_http_client_config_script = NULL_POINTER;
static pthread_mutex_t g_http_config_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_http_control_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_http_listener_thread;
static _Atomic(boole) g_http_listener_running = FALSE;
static boole g_http_page_enabled = FALSE;
static boole g_http_blank_when_disabled = FALSE;

typedef struct http_gzip_page
{
    ubyte *data;
    uint64 size;
    uint64 references; // Cache and active downloads; protected by g_http_config_mutex.
} http_gzip_page_t;

#ifndef DEBUG_ACTIVE
/* The original client.html is cached once. Per-request configuration is sent
   separately; the shared page is immutable. Debug builds read the file per request. */
static char *g_http_client_page_cache = NULL_POINTER;
static uint64 g_http_client_page_cache_size = 0;
static http_gzip_page_t *g_http_gzip_page_cache = NULL_POINTER;
#endif

// One deadline covers every header/body send in this worker response.
static _Thread_local uint64 g_http_response_deadline;

// declarations
static void *_http_server_internal__server_thread(void *arg_unused);
static void *_http_server_internal__connection_thread(void *arg_socket);
static void _http_server_internal__handle_connection(http_socket_t client_socket);
static void _http_server_internal__serve_file(http_socket_t client_socket, char *request_path, http_encoding_t encoding, boole identity_allowed);
static void _http_server_internal__send_simple_response(http_socket_t client_socket, const char *status_line, const char *body);
static boole _http_server_internal__send_all(http_socket_t client_socket, void *data, uint64 length);
static boole _http_server_internal__is_request_path_safe(const char *request_path);
static uint64 _http_server_internal__monotonic_ms(void);
static int64 _http_server_internal__file_size(FILE *file);
static uint64 _http_server_internal__read_request(http_socket_t client_socket, char *buffer, uint64 capacity);
static char *_http_server_internal__find_bytes(char *haystack, uint64 haystack_length, const char *needle, uint64 needle_length);
static void _http_server_internal__release_gzip_page(http_gzip_page_t *page);
static http_gzip_page_t *_http_server_internal__compress_page(char *page, uint64 size, const char *config);
static boole _http_server_internal__redirect_host(const char *request, char *host, uint64 capacity);

#ifndef DEBUG_ACTIVE
static void _http_server_internal__cache_client_page(void);
static void _http_server_internal__refresh_gzip_page(void);
#endif

/**
 * @brief Reads an elapsed-time clock so changing the computer clock cannot extend an HTTP request
 *        timeout.
 *
 * @return uint64 -> the monotonic clock value in milliseconds, or zero if the POSIX clock cannot be read
 */
static uint64 _http_server_internal__monotonic_ms(void)
{
#ifdef WIN32
    return GetTickCount64();
#else
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return 0;
    }
    return (uint64)now.tv_sec * 1000 + (uint64)now.tv_nsec / 1000000;
#endif
}

/**
 * @brief Reads a file's byte length and returns its position to the beginning.
 * @param FILE* file -> open file whose contents will be read from the start
 * @return int64 -> byte length, or -1 when seeking or reading the position fails
 */
static int64 _http_server_internal__file_size(FILE *file)
{
    int64 size = -1;

    // Read file length
#ifdef WIN32
    if (_fseeki64(file, 0, SEEK_END) != 0)
    {
        return -1;
    }
    size = _ftelli64(file);
#else
    if (fseeko(file, 0, SEEK_END) != 0)
    {
        return -1;
    }
    size = ftello(file);
#endif

    // Rewind for reading
#ifdef WIN32
    if (_fseeki64(file, 0, SEEK_SET) != 0)
#else
    if (fseeko(file, 0, SEEK_SET) != 0)
#endif
    {
        return -1;
    }
    return size;
}

/**
 * @brief Waits for the complete HTTP request headers even when they arrive in several socket reads.
 *
 * All pieces share one timeout. Oversized headers receive HTTP 431; embedded zero bytes receive HTTP 400.
 *
 * @param http_socket_t client_socket -> the accepted connection to read
 * @param char* buffer -> receives the request bytes and an ending zero byte
 * @param uint64 capacity -> the size of buffer in bytes, which must be greater than zero
 *
 * @return uint64 -> the number of received bytes when all headers are present, or zero for a timeout, closed socket or invalid request
 */
static uint64 _http_server_internal__read_request(http_socket_t client_socket, char *buffer, uint64 capacity)
{
#ifndef WIN32
    struct timeval timeout = { 0 };
#endif

    uint64 now = 0;
    uint64 remaining_ms = 0;
    int64 received_now = 0;
    uint64 amount = 0;
#ifdef WIN32
    DWORD timeout_ms = 0;
#endif

    uint64 received = 0;
    uint64 deadline = _http_server_internal__monotonic_ms() + HTTP_SERVER_RECV_TIMEOUT_SECONDS * 1000;

    if (buffer == NULL_POINTER || capacity < 2 || capacity > SIZE_MAX)
    {
        return 0;
    }
    buffer[0] = 0;
    for (;;)
    {
        now = _http_server_internal__monotonic_ms();
        remaining_ms = 0;
        received_now = 0;

        if (now >= deadline)
        {
            return 0;
        }
        if (received == capacity - 1)
        {
            _http_server_internal__send_simple_response(client_socket, "431 Request Header Fields Too Large", "431 Request Header Fields Too Large");
            return 0;
        }
        remaining_ms = deadline - now;
#ifdef WIN32
        timeout_ms = (DWORD)remaining_ms;
        if (setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, (int)sizeof(timeout_ms)) != 0)
        {
            return 0;
        }
#else
        timeout.tv_sec = (long)(remaining_ms / 1000);
        timeout.tv_usec = (long)((remaining_ms % 1000) * 1000);
        if (setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)) != 0)
        {
            return 0;
        }
#endif
        // Limit socket read
        amount = capacity - 1 - received;
        if (amount > INT_MAX)
        {
            amount = INT_MAX;
        }
        received_now = recv(client_socket, buffer + received, (int)amount, 0);
        if (received_now <= 0)
        {
            return 0;
        }
        if (clib__memchr(buffer + received, 0, (uint64)received_now) != NULL_POINTER)
        {
            _http_server_internal__send_simple_response(client_socket, "400 Bad Request", "400 Bad Request");
            return 0;
        }
        received += (uint64)received_now;
        buffer[received] = 0;
        if (clib__strstr(buffer, "\r\n\r\n") != NULL_POINTER)
        {
            return received;
        }
    }
}

/**
 * @brief Sends the whole buffer, repeating socket writes until every byte is sent or writing fails.
 *
 * @param http_socket_t client_socket -> the connection to write to
 * @param void* data -> the bytes to send; the caller keeps ownership
 * @param uint64 length -> the number of bytes to send
 *
 * @return boole -> TRUE when every byte was sent, otherwise FALSE
 */
static boole _http_server_internal__send_all(http_socket_t client_socket, void *data, uint64 length)
{
#ifndef _WIN32
    struct timeval timeout = { 0 };
#endif

    uint64 now = 0;
    uint64 remaining_ms = 0;
#ifdef _WIN32
    DWORD timeout = 0;
#endif

    uint64 total_sent = 0;
    int64 sent_now = 0;
    uint64 remaining = 0;
    uint64 chunk_length = 0;

    if ((data == NULL_POINTER && length != 0) || length > SIZE_MAX)
    {
        return FALSE;
    }
    while (total_sent < length)
    {
        now = _http_server_internal__monotonic_ms();
        if (g_http_response_deadline == 0)
        {
            g_http_response_deadline = now + 30000;
        }
        if (now >= g_http_response_deadline)
        {
            return FALSE;
        }
        remaining_ms = g_http_response_deadline - now;
#ifdef _WIN32
        timeout = (DWORD)remaining_ms;
#else
        timeout = (struct timeval){ (long)(remaining_ms / 1000), (long)((remaining_ms % 1000) * 1000) };
#endif
        if (setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout)) != 0)
        {
            return FALSE;
        }
        remaining = length - total_sent;
        chunk_length = remaining > 65536 ? 65536 : remaining;
        sent_now = send(client_socket, (char *)data + total_sent, (int)chunk_length, HTTP_SEND_FLAGS);

        if (sent_now <= 0)
        {
            return FALSE;
        }

        total_sent = total_sent + (uint64)sent_now;
    }
    return TRUE;
}

/**
 * @brief sends a tiny plain-text response for an error status (404, 403, 405, ...)
 *
 * @param http_socket_t client_socket -> the connection to write to
 * @param const char* status_line -> the http status line text, e.g. "404 Not Found"
 * @param const char* body -> the response body text
 *
 * @return void
 */
static void _http_server_internal__send_simple_response(http_socket_t client_socket, const char *status_line, const char *body)
{
    char response[HTTP_SERVER_HEADER_SIZE] = { 0 };

    clib__null_memory(response, sizeof(response));

    clib__string_format(response, sizeof(response), "HTTP/1.1 %s\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %llu\r\nConnection: close\r\n\r\n%s", status_line, clib__utf8_string_length(body), body);

    _http_server_internal__send_all(client_socket, response, clib__utf8_string_length(response));
}

/**
 * @brief rejects request paths that could escape the webroot (path traversal)
 *
 * @param const char* request_path -> the url path taken from the request line
 *
 * @return boole TRUE if the path is safe to resolve under the webroot, FALSE otherwise
 */
static boole _http_server_internal__is_request_path_safe(const char *request_path)
{
    uint64 i = 0;
    uint64 length = 0;

    length = clib__utf8_string_length(request_path);

    /* must be a site-absolute path */
    if (length == 0 || request_path[0] != '/')
    {
        return FALSE;
    }

    for (i = 0; i < length; i++)
    {
        /* a backslash or any ".." segment could climb out of the webroot */
        if (request_path[i] == '\\')
        {
            return FALSE;
        }

        if (request_path[i] == '.' && request_path[i + 1] == '.')
        {
            return FALSE;
        }
    }

    return TRUE;
}

/**
 * @brief stores the window.__SERVER_CONFIG__ script injected into served client.html; safe to
 *        replace while the HTTP server is running
 *
 * @param char* config_script -> the script text the served page should run, e.g. window.__SERVER_CONFIG__={...};
 *
 * @return void
 */
void http_server__set_client_config(char *config_script)
{
    uint64 size = config_script == NULL_POINTER ? 0 : strlen(config_script) + 1;
    char *copy = size != 0 ? malloc(size) : NULL_POINTER;

    if (copy != NULL_POINTER)
    {
        memcpy(copy, config_script, size);
    }
    pthread_mutex_lock(&g_http_config_mutex);
    if (g_http_client_config_script != NULL_POINTER)
    {
        memset(g_http_client_config_script, 0, strlen(g_http_client_config_script));
        free(g_http_client_config_script);
    }
    g_http_client_config_script = copy;
#ifndef DEBUG_ACTIVE
    _http_server_internal__refresh_gzip_page();
#endif
    pthread_mutex_unlock(&g_http_config_mutex);
}

/**
 * @brief Finds the first occurrence of a byte sequence inside a buffer.
 *
 * @param char* haystack -> the buffer to search; no ending zero byte is required
 * @param uint64 haystack_length -> the number of bytes available in haystack
 * @param const char* needle -> the byte sequence to find
 * @param uint64 needle_length -> the number of bytes in needle
 *
 * @return char* -> a pointer to the match within haystack, or NULL_POINTER when absent or needle_length is zero
 */
static char *_http_server_internal__find_bytes(char *haystack, uint64 haystack_length, const char *needle, uint64 needle_length)
{
    uint64 i = 0;
    uint64 j = 0;

    if ((needle_length == 0) || (haystack_length < needle_length))
    {
        return NULL_POINTER;
    }

    for (i = 0; i <= haystack_length - needle_length; i++)
    {
        for (j = 0; j < needle_length; j++)
        {
            if (haystack[i + j] != needle[j])
            {
                break;
            }
        }

        if (j == needle_length)
        {
            return haystack + i;
        }
    }

    return NULL_POINTER;
}

// Caller holds g_http_config_mutex. Old downloads keep their own reference
// when a settings change replaces the cached response.
static void _http_server_internal__release_gzip_page(http_gzip_page_t *page)
{
    if (page != NULL_POINTER && --page->references == 0)
    {
        memset(page->data, 0, page->size);
        free(page->data);
        free(page);
    }
}

static http_gzip_page_t *_http_server_internal__compress_page(char *page, uint64 size, const char *config)
{
    uint64 prefix = 0;
    uint64 suffix = 0;
    uint64 config_size = config == NULL_POINTER ? 0 : strlen(config);
    char *marker = NULL_POINTER;
    char *end = NULL_POINTER;
    char *body = page;
    uint64 body_size;
    http_gzip_page_t *compressed;

    if (size > SIZE_MAX - config_size)
    {
        return NULL_POINTER;
    }
    body_size = (uint64)size;
    if (config_size != 0)
    {
        marker = _http_server_internal__find_bytes(page, size, "/*LEMONCFG", 10);
        if (marker != NULL_POINTER)
        {
            end = _http_server_internal__find_bytes(marker, size - (uint64)(marker - page), "*/", 2);
        }
    }
    if (end != NULL_POINTER)
    {
        prefix = (uint64)(marker - page);
        suffix = (uint64)size - (uint64)(end + 2 - page);
        body_size = prefix + config_size + suffix;
        body = malloc(body_size);
        if (body == NULL_POINTER)
        {
            return NULL_POINTER;
        }
        memcpy(body, page, prefix);
        memcpy(body + prefix, config, config_size);
        memcpy(body + prefix + config_size, end + 2, suffix);
    }
    compressed = calloc(1, sizeof(*compressed));
    if (compressed != NULL_POINTER)
    {
        compressed->data = http_gzip__compress(body, body_size, &compressed->size);
        compressed->references = 1;
        if (compressed->data == NULL_POINTER)
        {
            free(compressed);
            compressed = NULL_POINTER;
        }
    }
    if (body != page)
    {
        memset(body, 0, body_size);
        free(body);
    }
    return compressed;
}

#ifndef DEBUG_ACTIVE
// Called under g_http_config_mutex, only on startup or configuration changes.
// Compression and downloads use RAM; no generated file contains the login keys.
static void _http_server_internal__refresh_gzip_page(void)
{
    http_gzip_page_t *replacement = NULL_POINTER;

    if (g_http_client_page_cache != NULL_POINTER)
    {
        replacement = _http_server_internal__compress_page(g_http_client_page_cache, g_http_client_page_cache_size, g_http_client_config_script);
        if (replacement == NULL_POINTER)
        {
            log_warn("%s", "http server: gzip cache unavailable, serving uncompressed HTML when accepted");
        }
    }
    _http_server_internal__release_gzip_page(g_http_gzip_page_cache);
    g_http_gzip_page_cache = replacement;
}

/**
 * @brief reads the original client.html into an immutable cache before the listener starts
 *
 * @return void
 *
 * @attention on any failure the cache stays empty and _http_server_internal__serve_file falls back to streaming the file from disk per request, same as a DEBUG_ACTIVE build
 */
static void _http_server_internal__cache_client_page(void)
{
    char file_path[HTTP_SERVER_PATH_SIZE] = { 0 };
    FILE *file = NULL_POINTER;
    int64 file_size = 0;
    uint64 read_count = 0;

    clib__null_memory(file_path, sizeof(file_path));
    clib__string_format(file_path, sizeof(file_path), "%s%s", g_http_server_webroot, "/client.html");

    file = fopen(file_path, "rb");
    if (file == NULL_POINTER)
    {
        log_info("%s", "http server: client.html not found, ram cache stays empty \n");
        return;
    }

    file_size = _http_server_internal__file_size(file);

    if (file_size <= 0 || (uint64)file_size > SIZE_MAX)
    {
        fclose(file);
        return;
    }

    g_http_client_page_cache = (char *)malloc((uint64)file_size);
    if (g_http_client_page_cache == NULL_POINTER)
    {
        fclose(file);
        log_info("%s", "http server: client.html ram cache allocation failed, serving from disk \n");
        return;
    }

    read_count = fread(g_http_client_page_cache, 1, (uint64)file_size, file);
    fclose(file);

    if (read_count != (uint64)file_size)
    {
        free(g_http_client_page_cache);
        g_http_client_page_cache = NULL_POINTER;
        log_info("%s", "http server: client.html ram cache read failed, serving from disk \n");
        return;
    }

    g_http_client_page_cache_size = (uint64)file_size;
}
#endif

/**
 * @brief resolves request_path under the webroot and streams the file, or sends 403/404
 *
 * @param http_socket_t client_socket -> the connection to write the response to
 * @param char* request_path -> the url path from the request line (e.g. "/", "/scripts/app/main.js")
 *
 * @return void
 */
// The cached page remains unmodified. Each request gets a short configuration
// snapshot, so a slow download never blocks a settings save or another visitor.
static void _http_server_internal__serve_file(http_socket_t client_socket, char *request_path, http_encoding_t encoding, boole identity_allowed)
{
    boole blank = FALSE;
    char response[] = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: 0\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    int64 length = 0;
    char file_path[HTTP_SERVER_PATH_SIZE] = { 0 };
    char header[HTTP_SERVER_HEADER_SIZE] = { 0 };
    char *page = NULL_POINTER;
    char *config = NULL_POINTER;
    char *marker = NULL_POINTER;
    char *end = NULL_POINTER;
    FILE *file = NULL_POINTER;
    uint64 size = 0;
    uint64 prefix = 0;
    uint64 suffix = 0;
    uint64 config_size = 0;
    boole owned = FALSE;
    http_gzip_page_t *compressed = NULL_POINTER;

    pthread_mutex_lock(&g_http_config_mutex);
    if (!g_http_page_enabled)
    {
        blank = g_http_blank_when_disabled;
        pthread_mutex_unlock(&g_http_config_mutex);
        if (blank)
        {
            _http_server_internal__send_all(client_socket, response, sizeof(response) - 1);
        }
        return;
    }
#ifndef DEBUG_ACTIVE
    if (encoding == HTTP_ENCODING_GZIP && g_http_gzip_page_cache != NULL_POINTER)
    {
        compressed = g_http_gzip_page_cache;
        compressed->references++;
    }
#endif
    if (compressed == NULL_POINTER && g_http_client_config_script != NULL_POINTER)
    {
        config_size = strlen(g_http_client_config_script);
        config = malloc(config_size + 1);
        if (config != NULL_POINTER)
        {
            memcpy(config, g_http_client_config_script, config_size + 1);
        }
    }
    pthread_mutex_unlock(&g_http_config_mutex);
    if (config_size != 0 && config == NULL_POINTER)
    {
        _http_server_internal__send_simple_response(client_socket, "503 Service Unavailable", "503 Service Unavailable");
        return;
    }
    if (!_http_server_internal__is_request_path_safe(request_path))
    {
        _http_server_internal__send_simple_response(client_socket, "403 Forbidden", "403 Forbidden");
        goto done;
    }
    if (strcmp(request_path, "/") != 0 && strcmp(request_path, "/client.html") != 0)
    {
        _http_server_internal__send_simple_response(client_socket, "404 Not Found", "404 Not Found");
        goto done;
    }
    if (encoding == HTTP_ENCODING_UNACCEPTABLE)
    {
        _http_server_internal__send_simple_response(client_socket, "406 Not Acceptable", "No acceptable content encoding");
        goto done;
    }
    if (compressed != NULL_POINTER)
    {
        goto send_gzip;
    }
#ifndef DEBUG_ACTIVE
    page = g_http_client_page_cache;
    size = g_http_client_page_cache_size;
#endif
    if (page == NULL_POINTER)
    {
        snprintf(file_path, sizeof(file_path), "%s/client.html", g_http_server_webroot);
        file = fopen(file_path, "rb");
        if (file != NULL_POINTER)
        {
            length = _http_server_internal__file_size(file);
            if (length > 0 && (uint64)length <= SIZE_MAX)
            {
                page = malloc((uint64)length);
                owned = TRUE;
                if (page != NULL_POINTER && fread(page, 1, (uint64)length, file) == (uint64)length)
                {
                    size = (uint64)length;
                }
            }
        }
        if (file != NULL_POINTER)
        {
            fclose(file);
        }
        if (size == 0)
        {
            _http_server_internal__send_simple_response(client_socket, "404 Not Found", "404 Not Found");
            goto done;
        }
    }

    // Debug builds and the uncached disk fallback read the current file on each request.
    if (encoding == HTTP_ENCODING_GZIP && owned)
    {
        compressed = _http_server_internal__compress_page(page, size, config);
    }
send_gzip:
    if (compressed != NULL_POINTER)
    {
        snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Encoding: gzip\r\nVary: Accept-Encoding\r\nContent-Length: %llu\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n", (uint64)compressed->size);
        if (_http_server_internal__send_all(client_socket, header, strlen(header)))
        {
            _http_server_internal__send_all(client_socket, compressed->data, compressed->size);
        }
        goto done;
    }
    if (!identity_allowed)
    {
        _http_server_internal__send_simple_response(client_socket, "503 Service Unavailable", "Compressed response unavailable");
        goto done;
    }
    if (config_size != 0)
    {
        marker = _http_server_internal__find_bytes(page, size, "/*LEMONCFG", 10);
        if (marker != NULL_POINTER)
        {
            end = _http_server_internal__find_bytes(marker, size - (uint64)(marker - page), "*/", 2);
        }
    }
    prefix = size;
    if (end != NULL_POINTER)
    {
        prefix = (uint64)(marker - page);
        suffix = size - (uint64)(end + 2 - page);
    }
    else
    {
        config_size = 0;
    }
    snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nVary: Accept-Encoding\r\nContent-Length: %llu\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n", (uint64)(prefix + config_size + suffix));
    if (_http_server_internal__send_all(client_socket, header, strlen(header)) && _http_server_internal__send_all(client_socket, page, prefix) && _http_server_internal__send_all(client_socket, config, config_size) && suffix != 0)
    {
        _http_server_internal__send_all(client_socket, end + 2, suffix);
    }
done:
    pthread_mutex_lock(&g_http_config_mutex);
    _http_server_internal__release_gzip_page(compressed);
    pthread_mutex_unlock(&g_http_config_mutex);
    if (config != NULL_POINTER)
    {
        memset(config, 0, strlen(config));
        free(config);
    }
    if (owned)
    {
        free(page);
    }
}

// Only the local TLS proxy can supply the marker that suppresses HTTPS redirects.
// Match a complete header, never text in a URL, another header value, or a body.
static boole _http_server_internal__is_https_proxy_request(http_socket_t client_socket, const char *request)
{
    const char *name = NULL_POINTER;
    uint64 name_size = 0;
    const char *line = NULL_POINTER;
    boole found = FALSE;
    const char *end = NULL_POINTER;
    uint64 matched = 0;
    ubyte letter = 0;
    const char *value = NULL_POINTER;
    const char *value_end = NULL_POINTER;
    char address[INET6_ADDRSTRLEN] = { 0 };
    struct in6_addr binary = { 0 };
    uint64 value_size = 0;
    struct sockaddr_in peer;
#ifdef WIN32
    int peer_size = sizeof(peer); // getpeername writes an int.
#else
    socklen_t peer_size = sizeof(peer);
#endif

    memset(&peer, 0, sizeof(peer));
    if (getpeername(client_socket, (struct sockaddr *)&peer, &peer_size) != 0 || peer.sin_family != AF_INET || ntohl(peer.sin_addr.s_addr) != 0x7f000001U)
    {
        return FALSE;
    }

    name = "x-stunnel-client-ip";
    name_size = strlen(name);
    line = strstr(request, "\r\n");
    if (line == NULL_POINTER)
    {
        return FALSE;
    }
    line += 2;
    for (;;)
    {
        end = strstr(line, "\r\n");
        if (end == NULL_POINTER)
        {
            return FALSE;
        }
        if (end == line)
        {
            return found;
        }
        if (*line == ' ' || *line == '\t')
        {
            return FALSE;
        }
        matched = 0;
        if ((uint64)(end - line) > name_size && line[name_size] == ':')
        {
            while (matched < name_size)
            {
                letter = (ubyte)line[matched];
                if (letter >= 'A' && letter <= 'Z')
                {
                    letter += 'a' - 'A';
                }
                if (letter != (ubyte)name[matched])
                {
                    break;
                }
                matched++;
            }
        }
        if (matched == name_size)
        {
            value = line + name_size + 1;
            value_end = end;
            clib__memset(address, 0, sizeof(address));
            while (value < value_end && (*value == ' ' || *value == '\t'))
            {
                value++;
            }
            while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t'))
            {
                value_end--;
            }
            value_size = (uint64)(value_end - value);
            if (found || value_size == 0 || value_size >= sizeof(address))
            {
                return FALSE;
            }
            memcpy(address, value, value_size);
            address[value_size] = '\0';
            if (inet_pton(AF_INET, address, &binary) != 1 && inet_pton(AF_INET6, address, &binary) != 1)
            {
                return FALSE;
            }
            found = TRUE;
        }
        line = end + 2;
    }
}

// Read exactly one Host field, then keep only its validated host component.
// The redirect uses the configured HTTPS port, not the port supplied by a visitor.
static boole _http_server_internal__redirect_host(const char *request, char *host, uint64 capacity)
{
    const char *end = NULL_POINTER;
    const char *colon = NULL_POINTER;
    const char *name_cursor = NULL_POINTER;
    ubyte name_letter = 0;
    const char *current = NULL_POINTER;
    ubyte value_letter = 0;
    const char *host_end = NULL_POINTER;
    char address[INET6_ADDRSTRLEN] = { 0 };
    struct in6_addr binary = { 0 };
    const char *closing = NULL_POINTER;
    const char *label = NULL_POINTER;
    ubyte letter = 0;
    uint64 name_size = 0;
    const char *port = NULL_POINTER;
    uint64 number = 0;
    uint64 host_size = 0;
    const char *line = strstr(request, "\r\n");
    const char *value = NULL_POINTER;
    const char *value_end = NULL_POINTER;

    if (capacity == 0 || line == NULL_POINTER)
    {
        return FALSE;
    }
    host[0] = 0;
    line += 2;
    for (;;)
    {
        end = strstr(line, "\r\n");
        if (end == NULL_POINTER)
        {
            return FALSE;
        }
        if (end == line)
        {
            break;
        }
        colon = memchr(line, ':', (uint64)(end - line));
        if (colon == NULL_POINTER || colon == line)
        {
            return FALSE;
        }
        for (name_cursor = line; name_cursor < colon; name_cursor++)
        {
            name_letter = (ubyte)*name_cursor;
            if (!clib__isalnum(name_letter) && strchr("!#$%&'*+-.^_`|~", name_letter) == NULL_POINTER)
            {
                return FALSE;
            }
        }
        for (current = colon + 1; current < end; current++)
        {
            value_letter = (ubyte)*current;
            if ((value_letter < 32 && value_letter != '\t') || value_letter == 127)
            {
                return FALSE;
            }
        }
        if (colon - line == 4 && clib__tolower((ubyte)line[0]) == 'h' && clib__tolower((ubyte)line[1]) == 'o' && clib__tolower((ubyte)line[2]) == 's' && clib__tolower((ubyte)line[3]) == 't')
        {
            if (value != NULL_POINTER)
            {
                return FALSE;
            }
            value = colon + 1;
            value_end = end;
            while (value < value_end && (*value == ' ' || *value == '\t'))
            {
                value++;
            }
            while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t'))
            {
                value_end--;
            }
        }
        line = end + 2;
    }
    if (value == NULL_POINTER || value == value_end)
    {
        return FALSE;
    }
    host_end = value;
    if (*value == '[')
    {
        clib__memset(address, 0, sizeof(address));
        closing = memchr(value + 1, ']', (uint64)(value_end - value - 1));
        if (closing == NULL_POINTER || closing == value + 1 || (uint64)(closing - value - 1) >= sizeof(address))
        {
            return FALSE;
        }
        memcpy(address, value + 1, (uint64)(closing - value - 1));
        address[closing - value - 1] = 0;
        if (inet_pton(AF_INET6, address, &binary) != 1)
        {
            return FALSE;
        }
        host_end = closing + 1;
    }
    else
    {
        label = value;
        while (host_end < value_end && *host_end != ':')
        {
            letter = (ubyte)*host_end;
            if (!clib__isalnum(letter) && letter != '-' && letter != '_' && letter != '.')
            {
                return FALSE;
            }
            if (letter == '.')
            {
                if (host_end == label || host_end - label > 63 || *label == '-' || host_end[-1] == '-')
                {
                    return FALSE;
                }
                label = host_end + 1;
            }
            host_end++;
        }
        name_size = (uint64)(host_end - value);
        if (name_size == 0 || name_size > 254 || (name_size == 254 && host_end[-1] != '.') || host_end - label > 63 || (host_end > label && (*label == '-' || host_end[-1] == '-')))
        {
            return FALSE;
        }
    }
    if (host_end < value_end)
    {
        port = host_end;
        number = 0;
        if (*port++ != ':' || port == value_end || value_end - port > 5)
        {
            return FALSE;
        }
        while (port < value_end)
        {
            if (*port < '0' || *port > '9')
            {
                return FALSE;
            }
            number = number * 10 + (uint64)(*port++ - '0');
        }
        if (number == 0 || number > 65535)
        {
            return FALSE;
        }
    }
    host_size = (uint64)(host_end - value);
    if (host_size >= capacity)
    {
        return FALSE;
    }
    memcpy(host, value, host_size);
    host[host_size] = 0;
    return TRUE;
}

/**
 * @brief reads one http request from the socket and dispatches it (GET only)
 * @param http_socket_t client_socket -> the accepted connection socket
 * @return void
 */
static void _http_server_internal__handle_connection(http_socket_t client_socket)
{
#if !defined(WIN32) && defined(SO_NOSIGPIPE)
    int nosigpipe_on = 1; // setsockopt reads an int.
#endif
#ifndef WIN32
    struct timeval send_timeout = { 0 };
#endif

#ifdef WIN32
    DWORD send_timeout_ms = 0;
#endif
    boole page_enabled = FALSE;
    int64 formatted = 0;
    char request_buffer[HTTP_SERVER_REQUEST_BUFFER_SIZE] = { 0 };
    char request_path[HTTP_SERVER_PATH_SIZE] = { 0 };
    uint64 received = 0;
    uint64 i = 0;
    uint64 path_length = 0;
    char redirect_host[300] = { 0 };
    char redirect_location[HTTP_SERVER_PATH_SIZE + sizeof(redirect_host) + 32] = { 0 };
    char redirect_response[sizeof(redirect_location) + 128] = { 0 };
    boole identity_allowed = TRUE;
    http_encoding_t encoding;

    g_http_response_deadline = _http_server_internal__monotonic_ms() + 30000;

    // Stop a visitor that no longer reads the response from keeping its thread
    // forever. Windows expects this timeout in milliseconds; the other systems
    // expect seconds and microseconds. The request reader sets its own timeout
    // using the time left to receive the complete headers.
#ifdef WIN32
    send_timeout_ms = HTTP_SERVER_SEND_TIMEOUT_SECONDS * 1000;
    if (setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, (const char *)&send_timeout_ms, (int)sizeof(send_timeout_ms)) != 0)
    {
        return;
    }
#else
    clib__null_memory(&send_timeout, sizeof(send_timeout));
    send_timeout.tv_sec = HTTP_SERVER_SEND_TIMEOUT_SECONDS;
    if (setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, (const char *)&send_timeout, sizeof(send_timeout)) != 0)
    {
        return;
    }
#ifdef SO_NOSIGPIPE
    /* macOS/BSD have no MSG_NOSIGNAL; suppress SIGPIPE on this socket so a client vanishing
       mid-send returns EPIPE instead of killing the server. no-op define on Linux */
    setsockopt(client_socket, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&nosigpipe_on, sizeof(nosigpipe_on));
#endif
#endif

    clib__null_memory(request_buffer, sizeof(request_buffer));
    clib__null_memory(request_path, sizeof(request_path));

    received = _http_server_internal__read_request(client_socket, request_buffer, sizeof(request_buffer));
    if (received == 0)
    {
        return;
    }

    /* only GET is served */
    if (request_buffer[0] != 'G' || request_buffer[1] != 'E' || request_buffer[2] != 'T' || request_buffer[3] != ' ')
    {
        DBG_HTTP_SERVER log_info("%s", "http server: request refused, only GET is served");
        _http_server_internal__send_simple_response(client_socket, "405 Method Not Allowed", "405 Method Not Allowed");
        return;
    }

    /* copy the request target (between "GET " and the next space), dropping any query string */
    i = 4;
    while (i < received && request_buffer[i] != ' ' && request_buffer[i] != '\r' && request_buffer[i] != '\n' && request_buffer[i] != '?')
    {
        if (path_length >= sizeof(request_path) - 1)
        {
            _http_server_internal__send_simple_response(client_socket, "414 URI Too Long", "414 URI Too Long");
            return;
        }

        request_path[path_length] = request_buffer[i];
        path_length++;
        i++;
    }

    request_path[path_length] = 0;
    encoding = http_gzip__choose_encoding(request_buffer, &identity_allowed);

    pthread_mutex_lock(&g_http_config_mutex);
    page_enabled = g_http_page_enabled;
    pthread_mutex_unlock(&g_http_config_mutex);
    if (!page_enabled)
    {
        _http_server_internal__serve_file(client_socket, request_path, encoding, identity_allowed);
        return;
    }

    /* Local stunnel requests already arrived through HTTPS. Direct visitors
       must still redirect even when their request contains a forged marker. */
    if (g_http_https_redirect_enabled == TRUE && !_http_server_internal__is_https_proxy_request(client_socket, request_buffer))
    {
        clib__null_memory(redirect_host, sizeof(redirect_host));
        clib__null_memory(redirect_location, sizeof(redirect_location));
        clib__null_memory(redirect_response, sizeof(redirect_response));

        if (!_http_server_internal__redirect_host(request_buffer, redirect_host, sizeof(redirect_host)))
        {
            _http_server_internal__send_simple_response(client_socket, "400 Bad Request", "400 Bad Request");
            return;
        }

        formatted = 0;
        if (g_http_https_redirect_port == 443)
        {
            formatted = clib__string_format(redirect_location, sizeof(redirect_location), "https://%s%s", redirect_host, request_path);
        }
        else
        {
            formatted = clib__string_format(redirect_location, sizeof(redirect_location), "https://%s:%lld%s", redirect_host, g_http_https_redirect_port, request_path);
        }
        if (formatted < 0 || (uint64)formatted >= sizeof(redirect_location))
        {
            _http_server_internal__send_simple_response(client_socket, "414 URI Too Long", "414 URI Too Long");
            return;
        }

        formatted = clib__string_format(redirect_response, sizeof(redirect_response), "HTTP/1.1 301 Moved Permanently\r\nLocation: %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", redirect_location);
        if (formatted >= 0 && (uint64)formatted < sizeof(redirect_response))
        {
            _http_server_internal__send_all(client_socket, redirect_response, (uint64)formatted);
        }
        return;
    }

    _http_server_internal__serve_file(client_socket, request_path, encoding, identity_allowed);
}

static boole _http_server_internal__listener_ready(http_socket_t listener)
{
#ifdef WIN32
    fd_set ready;
    struct timeval timeout = { 0, 100000 };

    FD_ZERO(&ready);
    FD_SET(listener, &ready);
    return select(0, &ready, NULL_POINTER, NULL_POINTER, &timeout) > 0;
#else
    struct pollfd ready = { listener, POLLIN, 0 };

    return poll(&ready, 1, 100) > 0 && (ready.revents & POLLIN) != 0;
#endif
}

/**
 * @brief accepts HTTP connections and gives each one to a separate thread, so waiting for one
 *        visitor does not delay the others
 *
 * @param void* arg_unused -> unused
 *
 * @return void* always 0
 */
static void *_http_server_internal__server_thread(void *argument)
{
#ifndef WIN32
    int flags = 0; // fcntl expects an int mask.
#endif

    http_socket_t client_socket = 0;
#ifdef WIN32
    u_long blocking = 0;
#endif
    http_socket_t *connection_socket = NULL_POINTER;
    pthread_t thread = 0;
    http_socket_t listener = *(http_socket_t *)argument;

    free(argument);
    while (atomic_load(&g_http_listener_running))
    {
        if (!_http_server_internal__listener_ready(listener))
        {
            continue;
        }
        client_socket = accept(listener, NULL_POINTER, NULL_POINTER);
        if (client_socket == HTTP_INVALID_SOCKET)
        {
            continue;
        }

        // Accepted sockets must block with the request/send timeouts, even on
        // platforms that inherit the listener's nonblocking flag.
#ifdef WIN32
        blocking = 0;
        ioctlsocket(client_socket, FIONBIO, &blocking);
#else
        flags = fcntl(client_socket, F_GETFL, 0);
        fcntl(client_socket, F_SETFL, flags & ~O_NONBLOCK);
#endif
        pthread_mutex_lock(&g_http_connections_mutex);
        if (g_http_active_connections >= HTTP_SERVER_MAX_CONNECTIONS)
        {
            pthread_mutex_unlock(&g_http_connections_mutex);
            http_close_socket(client_socket);
            continue;
        }
        g_http_active_connections++;
        pthread_mutex_unlock(&g_http_connections_mutex);
        connection_socket = malloc(sizeof(*connection_socket));
        if (connection_socket != NULL_POINTER)
        {
            *connection_socket = client_socket;
            if (pthread_create(&thread, NULL_POINTER, _http_server_internal__connection_thread, connection_socket) == 0)
            {
                pthread_detach(thread);
                continue;
            }
            free(connection_socket);
        }
        http_close_socket(client_socket);
        pthread_mutex_lock(&g_http_connections_mutex);
        g_http_active_connections--;
        pthread_mutex_unlock(&g_http_connections_mutex);
    }
    http_close_socket(listener);
    return NULL_POINTER;
}

/**
 * @brief Serves one HTTP visitor and releases its connection slot when the request ends.
 *
 * @param void* arg_socket -> an allocated http_socket_t passed by the listener; this thread frees it and closes the socket
 *
 * @return void* -> NULL_POINTER after the connection is closed
 */
static void *_http_server_internal__connection_thread(void *arg_socket)
{
    http_socket_t client_socket = *(http_socket_t *)arg_socket;

    free(arg_socket);

    _http_server_internal__handle_connection(client_socket);
    http_close_socket(client_socket);
    pthread_mutex_lock(&g_http_connections_mutex);
    g_http_active_connections--;
    pthread_mutex_unlock(&g_http_connections_mutex);
    return NULL_POINTER;
}

/**
 * @brief Chooses whether HTTP visitors are redirected to the HTTPS front end.
 *
 * Call this before starting the HTTP server so connections use the chosen redirect settings.
 *
 * @param boole enabled -> TRUE to enable redirects or FALSE to serve the client page over HTTP
 * @param int64 https_port -> the HTTPS port included in redirect URLs
 *
 * @return void
 */
void http_server__set_https_redirect(boole enabled, int64 https_port)
{
    g_http_https_redirect_enabled = enabled;
    g_http_https_redirect_port = https_port;
}

// Initialize once even when HTTP starts disabled, so it can be enabled live.
void http_server__initialize(int64 port, char *webroot)
{
    if (g_http_server_webroot != NULL_POINTER)
    {
        return;
    }
    g_http_server_port = port;
    g_http_server_webroot = calloc(HTTP_SERVER_WEBROOT_SIZE, 1);
    if (g_http_server_webroot == NULL_POINTER)
    {
        return;
    }
    if (!clib__string_copy(g_http_server_webroot, HTTP_SERVER_WEBROOT_SIZE, webroot))
    {
        free(g_http_server_webroot);
        g_http_server_webroot = NULL_POINTER;
        return;
    }
#ifndef DEBUG_ACTIVE
    _http_server_internal__cache_client_page();
    pthread_mutex_lock(&g_http_config_mutex);
    _http_server_internal__refresh_gzip_page();
    pthread_mutex_unlock(&g_http_config_mutex);
#endif
}

// Bind synchronously so the settings panel can report a busy/invalid port.
// Only the listener is joined on shutdown; existing bounded HTTP downloads finish.
boole http_server__set_mode(boole enabled, boole blank_when_disabled)
{
#ifndef WIN32
    int flags = 0; // fcntl expects an int mask.
#endif

    http_socket_t *listener = NULL_POINTER;
    struct sockaddr_in address = { 0 };
    int reuse = 0; // setsockopt reads an int.
#ifdef WIN32
    WSADATA data = { 0 };
    u_long nonblocking = 0;
#endif

    boole success = FALSE;

    pthread_mutex_lock(&g_http_control_mutex);
    if ((enabled || blank_when_disabled) && !atomic_load(&g_http_listener_running))
    {
        listener = NULL_POINTER;
        reuse = 1;
        if (g_http_server_webroot == NULL_POINTER || g_http_server_port < 1 || g_http_server_port > 65535)
        {
            goto done;
        }
#ifdef WIN32
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        {
            goto done;
        }
#endif
        listener = malloc(sizeof(*listener));
        if (listener == NULL_POINTER)
        {
            goto startup_failed;
        }
        *listener = socket(AF_INET, SOCK_STREAM, 0);
        if (*listener == HTTP_INVALID_SOCKET)
        {
            goto startup_failed;
        }
        setsockopt(*listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons((ushort)g_http_server_port);
        if (bind(*listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(*listener, HTTP_SERVER_LISTEN_BACKLOG) != 0)
        {
            goto startup_failed;
        }
#ifdef WIN32
        nonblocking = 1;
        if (ioctlsocket(*listener, FIONBIO, &nonblocking) != 0)
#else
        flags = fcntl(*listener, F_GETFL, 0);
        if (flags < 0 || fcntl(*listener, F_SETFL, flags | O_NONBLOCK) != 0)
#endif
        {
            goto startup_failed;
        }
        atomic_store(&g_http_listener_running, TRUE);
        if (pthread_create(&g_http_listener_thread, NULL_POINTER, _http_server_internal__server_thread, listener) != 0)
        {
            atomic_store(&g_http_listener_running, FALSE);
            goto startup_failed;
        }
        goto apply;
startup_failed:
        if (listener != NULL_POINTER)
        {
            if (*listener != HTTP_INVALID_SOCKET)
            {
                http_close_socket(*listener);
            }
            free(listener);
        }
#ifdef WIN32
        WSACleanup();
#endif
        log_error("HTTP listener could not start on port %lld", (int64)g_http_server_port);
        goto done;
    }
apply:
    pthread_mutex_lock(&g_http_config_mutex);
    g_http_page_enabled = enabled;
    g_http_blank_when_disabled = blank_when_disabled;
    pthread_mutex_unlock(&g_http_config_mutex);
    if (!enabled && !blank_when_disabled && atomic_load(&g_http_listener_running))
    {
        atomic_store(&g_http_listener_running, FALSE);
        pthread_join(g_http_listener_thread, NULL_POINTER);

        // Release the listener reference; the chat listener keeps Winsock active
        // while accepted HTTP requests finish.
#ifdef WIN32
        WSACleanup();
#endif
    }
    success = TRUE;
done:
    pthread_mutex_unlock(&g_http_control_mutex);
    return success;
}

void http_server__start(int64 port, char *webroot)
{
    http_server__initialize(port, webroot);
    http_server__set_mode(TRUE, FALSE);
}
