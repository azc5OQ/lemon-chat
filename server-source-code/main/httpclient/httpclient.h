#ifndef HTTPCLIENT_H
#define HTTPCLIENT_H

#include "../definitions.h"

typedef enum httpclient_error_t
{
    HTTPCLIENT_OK = 0,
    HTTPCLIENT_ARGUMENT,
    HTTPCLIENT_MEMORY,
    HTTPCLIENT_DNS,
    HTTPCLIENT_SOCKET,
    HTTPCLIENT_TIMEOUT,
    HTTPCLIENT_TLS,
    HTTPCLIENT_HTTP,
    HTTPCLIENT_LIMIT,
    HTTPCLIENT_UNSUPPORTED,
    HTTPCLIENT_TRUNCATED
} httpclient_error_t;

typedef struct httpclient_header_t
{
    const char *name;
    const char *value;
} httpclient_header_t;

typedef struct httpclient_request_t
{
    const char *url;
    const char *method;
    const httpclient_header_t *headers;
    uint64 header_count;
    const void *body;
    uint64 body_size;
    boole public_addresses_only;
    const char *ca_file;
    uint64 timeout_ms;
    uint64 max_response_bytes;
} httpclient_request_t;

typedef struct httpclient_response_t
{
    uint64 status_code;
    char *headers;
    ubyte *body;
    uint64 body_size;
    uint64 native_error;
    uint64 tls_protocol;
} httpclient_response_t;

httpclient_error_t httpclient__request(const httpclient_request_t *request, httpclient_response_t *response);
void httpclient__response_free(httpclient_response_t *response);
const char *httpclient__error_text(httpclient_error_t error);

typedef struct httpclient_url_t
{
    char host[256];
    char port[6];
    char target[8193];
    boole is_https;
    boole is_ipv6;
} httpclient_url_t;

httpclient_error_t httpclient__parse_url(const char *url, httpclient_url_t *parsed);

#endif
