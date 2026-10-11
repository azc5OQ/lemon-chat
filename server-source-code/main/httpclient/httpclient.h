#ifndef HTTPCLIENT_H
#define HTTPCLIENT_H


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
    const char* name;
    const char* value;
} httpclient_header_t;

typedef struct httpclient_request_t
{
    const char* url;
    const char* method;
    const httpclient_header_t* headers;
    unsigned int header_count;
    const void* body;
    unsigned int body_size;
    int public_addresses_only;
    const char* ca_file;
    unsigned int timeout_ms;
    unsigned int max_response_bytes;
} httpclient_request_t;

typedef struct httpclient_response_t
{
    unsigned int status_code;
    char* headers;
    unsigned char* body;
    unsigned int body_size;
    unsigned int native_error;
    unsigned int tls_protocol;
} httpclient_response_t;


httpclient_error_t httpclient__request(const httpclient_request_t* request, httpclient_response_t* response);
void httpclient__response_free(httpclient_response_t* response);
const char* httpclient__error_text(httpclient_error_t error);


typedef struct httpclient_url_t
{
    char host[256];
    char port[6];
    char target[8193];
    int is_https;
    int is_ipv6;
} httpclient_url_t;

httpclient_error_t httpclient__parse_url(const char* url, httpclient_url_t* parsed);

#endif
