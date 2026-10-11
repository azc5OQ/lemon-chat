#ifndef HTTPCLIENT_INTERNAL_H
#define HTTPCLIENT_INTERNAL_H



#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "httpclient.h"

typedef signed char boole;
typedef unsigned char ubyte;
typedef unsigned int uint;
typedef unsigned long long uint64;
#define HTTP_HEADER_LIMIT 65536u
#define TLS_BUFFER_LIMIT 262144u
#define BODY_LIMIT_MAX 1073741823u

typedef struct http_buffer_t
{
    ubyte* data;
    uint size;
    uint capacity;
} http_buffer_t;

typedef struct http_parser_t
{
    http_buffer_t pending;
    http_buffer_t body;
    uint limit;
    uint remaining;
    uint metadata_bytes;
    uint informational;


    int state;
    boole head;
} http_parser_t;

typedef struct https_connection_t httpclient_connection_t;
httpclient_error_t _httpclient_internal__append(http_buffer_t* buffer, const void* data, uint size, uint limit);
httpclient_error_t _httpclient_internal__parse(http_parser_t* parser, httpclient_response_t* response);
int httpclient_internal__address_allowed(const struct sockaddr* address);
httpclient_error_t httpclient_transport__open(const httpclient_request_t* request, const httpclient_url_t* url,
    httpclient_response_t* response, httpclient_connection_t** output);
httpclient_error_t httpclient_transport__write(httpclient_connection_t* connection, const void* data, uint size);
httpclient_error_t httpclient_transport__read(httpclient_connection_t* connection, http_parser_t* parser, boole* closed);
void httpclient_transport__close(httpclient_connection_t* connection);
#endif
