/* winsock2.h must be included before anything can pull in windows.h (via pthread/winpthreads), or it
   clashes with the legacy winsock.h that windows.h includes by default. */
#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "../definitions.h"

#include "http_server.h"
#include "../clib/clib_memory.h"
#include "../clib/clib_string.h"

#include "../../third-party/rxi-log/log.h"

#include <string.h>
#include <stdio.h>

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
typedef int http_socket_t;
#define HTTP_INVALID_SOCKET (-1)
#define http_close_socket close
#define HTTP_SEND_FLAGS MSG_NOSIGNAL
#endif

#define HTTP_SERVER_REQUEST_BUFFER_SIZE 8192
#define HTTP_SERVER_PATH_SIZE 1024
#define HTTP_SERVER_SEND_CHUNK_SIZE 16384
#define HTTP_SERVER_HEADER_SIZE 512
#define HTTP_CLIENT_CONFIG_SIZE 2048
#define HTTP_SERVER_RECV_TIMEOUT_SECONDS 10
#define HTTP_SERVER_LISTEN_BACKLOG 16

/* one extension -> Content-Type mapping for the static file server */
typedef struct http_mime_entry_t
{
    const char* extension;   // 0x0
    const char* mime_type;   // 0x8
} http_mime_entry_t;

/* the single http server's configuration, set by http_server__start before the thread is spawned */
static int64 g_http_server_port = 0;
static char g_http_server_webroot[512];

/* window.__SERVER_CONFIG__ script (websocket port + connect keys) injected into the served client.html so it can autoconnect; empty until http_server__set_client_config runs */
static char g_http_client_config_script[HTTP_CLIENT_CONFIG_SIZE];
static boole g_http_client_config_set = FALSE;

static const http_mime_entry_t g_http_mime_table[] = {
    { ".html", "text/html; charset=utf-8" },
    { ".js", "text/javascript; charset=utf-8" },
    { ".mjs", "text/javascript; charset=utf-8" },
    { ".css", "text/css; charset=utf-8" },
    { ".json", "application/json" },
    { ".wasm", "application/wasm" },
    { ".png", "image/png" },
    { ".jpg", "image/jpeg" },
    { ".jpeg", "image/jpeg" },
    { ".gif", "image/gif" },
    { ".svg", "image/svg+xml" },
    { ".ico", "image/x-icon" },
    { ".mp3", "audio/mpeg" },
    { ".wav", "audio/wav" },
    { ".woff", "font/woff" },
    { ".woff2", "font/woff2" },
    { ".ttf", "font/ttf" },
    { ".txt", "text/plain; charset=utf-8" }
};

/* static functions are defined first */

/* declarations */
static void* _http_server_internal__server_thread(void* arg_unused);
static void _http_server_internal__handle_connection(http_socket_t client_socket);
static void _http_server_internal__serve_file(http_socket_t client_socket, char* request_path);
static void _http_server_internal__send_simple_response(http_socket_t client_socket, const char* status_line, const char* body);
static void _http_server_internal__send_all(http_socket_t client_socket, void* data, uint64 length);
static boole _http_server_internal__is_request_path_safe(const char* request_path);
static const char* _http_server_internal__content_type_for_path(const char* file_path);

/**
 * @brief sends the whole buffer, looping until every byte is written or the socket errors
 *
 * @param http_socket_t client_socket -> the connection to write to
 * @param void* data -> bytes to send
 * @param uint64 length -> number of bytes to send
 *
 * @return void
 */
static void _http_server_internal__send_all(http_socket_t client_socket, void* data, uint64 length)
{
    uint64 total_sent = 0;
    int sent_now = 0;

    while (total_sent < length)
    {
        sent_now = send(client_socket, (char*)data + total_sent, (int)(length - total_sent), HTTP_SEND_FLAGS);

        if (sent_now <= 0)
        {
            break;
        }

        total_sent = total_sent + (uint64)sent_now;
    }
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
static void _http_server_internal__send_simple_response(http_socket_t client_socket, const char* status_line, const char* body)
{
    char response[HTTP_SERVER_HEADER_SIZE];

    clib__null_memory(response, sizeof(response));

    snprintf(response, sizeof(response), "HTTP/1.1 %s\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %llu\r\nConnection: close\r\n\r\n%s", status_line, clib__utf8_string_length(body), body);

    _http_server_internal__send_all(client_socket, response, clib__utf8_string_length(response));
}

/**
 * @brief rejects request paths that could escape the webroot (path traversal)
 *
 * @param const char* request_path -> the url path taken from the request line
 *
 * @return boole TRUE if the path is safe to resolve under the webroot, FALSE otherwise
 */
static boole _http_server_internal__is_request_path_safe(const char* request_path)
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
 * @brief picks a Content-Type for a file from its extension
 *
 * @param const char* file_path -> the path whose extension is examined
 *
 * @return const char* a MIME type string, or application/octet-stream when unknown
 */
static const char* _http_server_internal__content_type_for_path(const char* file_path)
{
    const char* extension = NULL_POINTER;
    uint64 i = 0;
    uint64 count = 0;

    extension = strrchr(file_path, '.');

    if (extension == NULL_POINTER)
    {
        return "application/octet-stream";
    }

    count = sizeof(g_http_mime_table) / sizeof(g_http_mime_table[0]);

    for (i = 0; i < count; i++)
    {
        if (clib__is_string_equal(extension, g_http_mime_table[i].extension) == TRUE)
        {
            return g_http_mime_table[i].mime_type;
        }
    }

    return "application/octet-stream";
}

/**
 * @brief stores the window.__SERVER_CONFIG__ script injected into served client.html; call before http_server__start
 *
 * @param char* config_script -> the script text the served page should run, e.g. window.__SERVER_CONFIG__={...};
 *
 * @return void
 */
void http_server__set_client_config(char* config_script)
{
    clib__null_memory(g_http_client_config_script, sizeof(g_http_client_config_script));
    clib__copy_memory(config_script, g_http_client_config_script, clib__utf8_string_length(config_script), sizeof(g_http_client_config_script) - 1);
    g_http_client_config_set = TRUE;
}

/**
 * @brief finds the first occurrence of needle inside the first haystack_length bytes of haystack
 *
 * @return char* -> pointer to the match within haystack, or NULL_POINTER when absent
 */
static char* _http_server_internal__find_bytes(char* haystack, uint64 haystack_length, const char* needle, uint64 needle_length)
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

/**
 * @brief overwrites the LEMONCFG placeholder comment in the first chunk of client.html with the connection
 *        config, then pads it back to the original byte length so the already-sent Content-Length stays valid
 *
 * @param char* chunk -> first read chunk of client.html, modified in place
 * @param uint64 chunk_length -> count of valid bytes in chunk
 *
 * @return void
 */
static void _http_server_internal__inject_client_config(char* chunk, uint64 chunk_length)
{
    char* marker = NULL_POINTER;
    char* comment_end = NULL_POINTER;
    char* pad = NULL_POINTER;
    uint64 region_length = 0;
    uint64 config_length = 0;
    uint64 pad_length = 0;
    uint64 pad_index = 0;

    marker = _http_server_internal__find_bytes(chunk, chunk_length, "/*LEMONCFG", 10);
    if (marker == NULL_POINTER)
    {
        return;
    }

    comment_end = _http_server_internal__find_bytes(marker, chunk_length - (uint64)(marker - chunk), "*/", 2);
    if (comment_end == NULL_POINTER)
    {
        return;
    }
    comment_end += 2; /* advance past the placeholder comment terminator */

    region_length = (uint64)(comment_end - marker);
    config_length = clib__utf8_string_length(g_http_client_config_script);

    /* leave room for a trailing padding comment, which is at least 4 bytes */
    if ((config_length + 4) > region_length)
    {
        printf("%s", "http: client config too large for the client.html placeholder; serving without autoconnect\n");
        return;
    }

    clib__copy_memory(g_http_client_config_script, marker, config_length, region_length);

    pad = marker + config_length;
    pad_length = region_length - config_length;
    pad[0] = '/';
    pad[1] = '*';
    for (pad_index = 2; pad_index < (pad_length - 2); pad_index++)
    {
        pad[pad_index] = ' ';
    }
    pad[pad_length - 2] = '*';
    pad[pad_length - 1] = '/';
}

/**
 * @brief resolves request_path under the webroot and streams the file, or sends 403/404
 *
 * @param http_socket_t client_socket -> the connection to write the response to
 * @param char* request_path -> the url path from the request line (e.g. "/", "/scripts/app/main.js")
 *
 * @return void
 */
static void _http_server_internal__serve_file(http_socket_t client_socket, char* request_path)
{
    char file_path[HTTP_SERVER_PATH_SIZE];
    char header[HTTP_SERVER_HEADER_SIZE];
    char send_chunk[HTTP_SERVER_SEND_CHUNK_SIZE];
    const char* serve_target = NULL_POINTER;
    const char* content_type = NULL_POINTER;
    FILE* file = NULL_POINTER;
    int64 file_size = 0;
    uint64 read_count = 0;
    boole is_client_page = FALSE;
    boole is_first_chunk = TRUE;

    if (_http_server_internal__is_request_path_safe(request_path) == FALSE)
    {
        _http_server_internal__send_simple_response(client_socket, "403 Forbidden", "403 Forbidden");
        return;
    }

    /* "/" serves the client page */
    serve_target = request_path;
    if (request_path[1] == 0)
    {
        serve_target = "/client.html";
    }

    is_client_page = clib__is_string_equal((char* )serve_target, "/client.html");

    clib__null_memory(file_path, sizeof(file_path));
    snprintf(file_path, sizeof(file_path), "%s%s", g_http_server_webroot, serve_target);

    file = fopen(file_path, "rb");
    if (file == NULL_POINTER)
    {
        _http_server_internal__send_simple_response(client_socket, "404 Not Found", "404 Not Found");
        return;
    }

    fseek(file, 0, SEEK_END);
    file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size < 0)
    {
        fclose(file);
        _http_server_internal__send_simple_response(client_socket, "404 Not Found", "404 Not Found");
        return;
    }

    content_type = _http_server_internal__content_type_for_path(serve_target);

    clib__null_memory(header, sizeof(header));
    snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\nConnection: close\r\nCache-Control: no-cache\r\n\r\n", content_type, (long long)file_size);

    _http_server_internal__send_all(client_socket, header, clib__utf8_string_length(header));

    for (;;)
    {
        read_count = fread(send_chunk, 1, sizeof(send_chunk), file);

        if (read_count == 0)
        {
            break;
        }

        if ((is_first_chunk == TRUE) && (is_client_page == TRUE) && (g_http_client_config_set == TRUE))
        {
            _http_server_internal__inject_client_config(send_chunk, read_count);
        }
        is_first_chunk = FALSE;

        _http_server_internal__send_all(client_socket, send_chunk, read_count);
    }

    fclose(file);
}

/**
 * @brief reads one http request from the socket and dispatches it (GET only)
 *
 * @param http_socket_t client_socket -> the accepted connection socket
 *
 * @return void
 */
static void _http_server_internal__handle_connection(http_socket_t client_socket)
{
    char request_buffer[HTTP_SERVER_REQUEST_BUFFER_SIZE];
    char request_path[HTTP_SERVER_PATH_SIZE];
    int64 received = 0;
    uint64 i = 0;
    uint64 path_length = 0;

    /* don't let a stalled client hold the single-threaded accept loop forever. SO_RCVTIMEO takes a DWORD of
       milliseconds on Windows but a struct timeval on POSIX, so it has to be set per platform. */
#ifdef WIN32
    DWORD recv_timeout_ms = HTTP_SERVER_RECV_TIMEOUT_SECONDS * 1000;
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recv_timeout_ms, (int)sizeof(recv_timeout_ms));
#else
    struct timeval recv_timeout;
    clib__null_memory(&recv_timeout, sizeof(recv_timeout));
    recv_timeout.tv_sec = HTTP_SERVER_RECV_TIMEOUT_SECONDS;
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recv_timeout, sizeof(recv_timeout));
#endif

    clib__null_memory(request_buffer, sizeof(request_buffer));
    clib__null_memory(request_path, sizeof(request_path));

    received = recv(client_socket, request_buffer, (int)(sizeof(request_buffer) - 1), 0);
    if (received <= 0)
    {
        return;
    }

    /* only GET is served */
    if (request_buffer[0] != 'G' || request_buffer[1] != 'E' || request_buffer[2] != 'T' || request_buffer[3] != ' ')
    {
        _http_server_internal__send_simple_response(client_socket, "405 Method Not Allowed", "405 Method Not Allowed");
        return;
    }

    /* copy the request target (between "GET " and the next space), dropping any query string */
    i = 4;
    while (i < (uint64)received && request_buffer[i] != ' ' && request_buffer[i] != '\r' && request_buffer[i] != '\n' && request_buffer[i] != '?')
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

    _http_server_internal__serve_file(client_socket, request_path);
}

/**
 * @brief background thread: binds the listen socket and serves each incoming http connection in turn
 *
 * @param void* arg_unused -> unused
 *
 * @return void* always 0
 */
static void* _http_server_internal__server_thread(void* arg_unused)
{
    http_socket_t listen_socket = HTTP_INVALID_SOCKET;
    http_socket_t client_socket = HTTP_INVALID_SOCKET;
    int reuse = 1;
    struct sockaddr_in server_address;

    (void)arg_unused;

#ifdef WIN32
    /* Winsock must be initialized before any socket call. WSAStartup is process-wide and reference
       counted, so calling it here is safe even if the websocket server already started it. */
    {
        WSADATA wsa_data;
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
        {
            log_info("%s", "http server: WSAStartup() failed, not serving the client \n");
            return NULL_POINTER;
        }
    }
#endif

    listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket == HTTP_INVALID_SOCKET)
    {
        log_info("%s", "http server: socket() failed, not serving the client \n");
        return NULL_POINTER;
    }

    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, (int)sizeof(reuse));

    clib__null_memory(&server_address, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons((unsigned short)g_http_server_port);

    if (bind(listen_socket, (struct sockaddr*)&server_address, sizeof(server_address)) < 0)
    {
        log_info("%s %lld %s", "http server: bind() failed on port ", g_http_server_port, " (port 80 may need admin/root); not serving the client \n");
        http_close_socket(listen_socket);
        return NULL_POINTER;
    }

    if (listen(listen_socket, HTTP_SERVER_LISTEN_BACKLOG) < 0)
    {
        log_info("%s", "http server: listen() failed, not serving the client \n");
        http_close_socket(listen_socket);
        return NULL_POINTER;
    }

    log_info("%s %lld %s %s %s", "http server: serving client on port ", g_http_server_port, " from ", g_http_server_webroot, "\n");

    for (;;)
    {
        client_socket = accept(listen_socket, NULL_POINTER, NULL_POINTER);

        if (client_socket == HTTP_INVALID_SOCKET)
        {
            continue;
        }

        _http_server_internal__handle_connection(client_socket);
        http_close_socket(client_socket);
    }
}

/**
 * @brief starts the static-file http server on its own background thread and returns immediately
 *
 * @param int64 port -> tcp port to listen on (80 is the standard http port)
 * @param char* webroot -> directory whose files are served; a request for "/" maps to client.html in it
 *
 * @attention the server thread is detached and runs until the process exits; binding port 80 needs admin/root
 *
 * @return void
 */
void http_server__start(int64 port, char* webroot)
{
    pthread_t server_thread = 0;

    g_http_server_port = port;

    clib__null_memory(g_http_server_webroot, sizeof(g_http_server_webroot));
    if (webroot != NULL_POINTER)
    {
        clib__copy_memory(webroot, g_http_server_webroot, clib__utf8_string_length(webroot), sizeof(g_http_server_webroot) - 1);
    }

    if (pthread_create(&server_thread, 0, _http_server_internal__server_thread, NULL_POINTER) == 0)
    {
        pthread_detach(server_thread);
    }
    else
    {
        log_info("%s", "http_server__start: failed to create the server thread \n");
    }
}
