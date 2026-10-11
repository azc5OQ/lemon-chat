// This file provides the Linux and macOS connections used for URL previews
// when a client posts a website link in chat. The feature must be enabled in
// the server settings. It lets the server download the page and preview
// picture that will be shown below the message.
//
// It uses the operating system's sockets for HTTP and the bundled mbedTLS
// library for HTTPS. The website name and certificate are checked before the
// HTTPS request is sent. Each connection keeps its own encryption state and
// memory for the downloaded bytes, so several pages can be fetched at once.
#include "httpclient_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

struct https_connection_t
{
    int socket;
    int is_https;
    uint64 deadline;
    httpclient_response_t* response;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt roots;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
};

static uint64 _httpclient_posix__now(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64)now.tv_sec * 1000 + (uint64)now.tv_nsec / 1000000;
}

// The same deadline covers connecting, TLS negotiation and all body reads.
// A website that keeps sending small pieces does not restart the time limit.
static httpclient_error_t _httpclient_posix__wait(httpclient_connection_t* connection, int writing)
{
    struct pollfd descriptor;
    descriptor.fd = connection->socket;
    descriptor.events = writing ? POLLOUT : POLLIN;
    for (;;)
    {
        uint64 now = _httpclient_posix__now();
        if (now >= connection->deadline)
        {
            return HTTPCLIENT_TIMEOUT;
        }
        uint64 remaining = connection->deadline - now;
        int status = poll(&descriptor, 1, remaining > INT_MAX ? INT_MAX : (int)remaining);
        if (status > 0)
        {
            if (descriptor.revents & POLLNVAL)
            {
                return HTTPCLIENT_SOCKET;
            }
            return HTTPCLIENT_OK;
        }
        if (status == 0)
        {
            return HTTPCLIENT_TIMEOUT;
        }
        if (errno != EINTR)
        {
            connection->response->native_error = (uint)errno;
            return HTTPCLIENT_SOCKET;
        }
    }
}

static int _httpclient_posix__tls_send(void* context, const unsigned char* bytes, size_t size)
{
    httpclient_connection_t* connection = context;
    ssize_t sent = send(connection->socket, bytes, size, MSG_NOSIGNAL);
    if (sent < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        connection->response->native_error = (uint)errno;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return (int)sent;
}

static int _httpclient_posix__tls_receive(void* context, unsigned char* bytes, size_t size)
{
    httpclient_connection_t* connection = context;
    ssize_t received = recv(connection->socket, bytes, size, 0);
    if (received < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        connection->response->native_error = (uint)errno;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return (int)received;
}

static int _httpclient_posix__load_roots(httpclient_connection_t* connection, const char* ca_file)
{
    if (ca_file != NULL)
    {
        return mbedtls_x509_crt_parse_file(&connection->roots, ca_file) >= 0 && connection->roots.raw.p != NULL;
    }
#ifdef __APPLE__
    // Ask macOS for its trusted roots instead of requiring a Homebrew install
    // or a downloaded certificate bundle next to the chat server.
    CFArrayRef certificates = NULL;
    if (SecTrustCopyAnchorCertificates(&certificates) == errSecSuccess && certificates != NULL)
    {
        for (CFIndex index = 0; index < CFArrayGetCount(certificates); index++)
        {
            SecCertificateRef certificate = (SecCertificateRef)CFArrayGetValueAtIndex(certificates, index);
            CFDataRef bytes = SecCertificateCopyData(certificate);
            if (bytes != NULL)
            {
                mbedtls_x509_crt_parse_der(&connection->roots, CFDataGetBytePtr(bytes), (size_t)CFDataGetLength(bytes));
                CFRelease(bytes);
            }
        }
        CFRelease(certificates);
    }
#else
    static const char* files[] =
    {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/ca-bundle.pem",
        "/etc/ssl/cert.pem"
    };
    for (size_t index = 0; index < sizeof(files) / sizeof(files[0]); index++)
    {
        if (mbedtls_x509_crt_parse_file(&connection->roots, files[index]) >= 0 && connection->roots.raw.p != NULL)
        {
            break;
        }
    }
#ifdef __ANDROID__
    if (connection->roots.raw.p == NULL)
    {
        mbedtls_x509_crt_parse_path(&connection->roots, "/apex/com.android.conscrypt/cacerts");
        mbedtls_x509_crt_parse_path(&connection->roots, "/system/etc/security/cacerts");
    }
#endif
#endif
    return connection->roots.raw.p != NULL;
}

httpclient_error_t httpclient_transport__open(const httpclient_request_t* request, const httpclient_url_t* url,
    httpclient_response_t* response, httpclient_connection_t** output)
{
    struct addrinfo hints = {0};
    struct addrinfo* addresses = NULL;
    httpclient_error_t error = HTTPCLIENT_SOCKET;
    httpclient_connection_t* connection = calloc(1, sizeof(*connection));
    *output = connection;
    if (connection == NULL)
    {
        return HTTPCLIENT_MEMORY;
    }
    connection->socket = -1;
    connection->is_https = url->is_https;
    connection->response = response;
    mbedtls_ssl_init(&connection->ssl);
    mbedtls_ssl_config_init(&connection->config);
    mbedtls_x509_crt_init(&connection->roots);
    mbedtls_entropy_init(&connection->entropy);
    mbedtls_ctr_drbg_init(&connection->random);

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int status = getaddrinfo(url->host, url->port, &hints, &addresses);
    if (status != 0)
    {
        response->native_error = (uint)status;
        return HTTPCLIENT_DNS;
    }
    connection->deadline = _httpclient_posix__now() + (request->timeout_ms == 0 ? 15000u : request->timeout_ms);
    for (struct addrinfo* address = addresses; address != NULL; address = address->ai_next)
    {
        if (request->public_addresses_only && !httpclient_internal__address_allowed(address->ai_addr))
        {
            continue;
        }
        connection->socket = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (connection->socket < 0)
        {
            continue;
        }
#ifdef SO_NOSIGPIPE
        int no_sigpipe = 1;
        setsockopt(connection->socket, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
        int flags = fcntl(connection->socket, F_GETFL, 0);
        status = flags < 0 ? -1 : fcntl(connection->socket, F_SETFL, flags | O_NONBLOCK);
        if (status == 0)
        {
            status = connect(connection->socket, address->ai_addr, address->ai_addrlen);
            error = status == 0 ? HTTPCLIENT_OK : HTTPCLIENT_SOCKET;
            if (status < 0 && errno == EINPROGRESS)
            {
                error = _httpclient_posix__wait(connection, 1);
                int socket_error = 0;
                socklen_t length = sizeof(socket_error);
                if (error == HTTPCLIENT_OK && (getsockopt(connection->socket, SOL_SOCKET, SO_ERROR,
                    &socket_error, &length) != 0 || socket_error != 0))
                {
                    response->native_error = (uint)(socket_error != 0 ? socket_error : errno);
                    error = HTTPCLIENT_SOCKET;
                }
            }
        }
        if (status == 0 || error == HTTPCLIENT_OK)
        {
            error = HTTPCLIENT_OK;
            break;
        }
        close(connection->socket);
        connection->socket = -1;
        if (error == HTTPCLIENT_TIMEOUT)
        {
            break;
        }
    }
    freeaddrinfo(addresses);
    if (error != HTTPCLIENT_OK || !connection->is_https)
    {
        return error;
    }
    if (!_httpclient_posix__load_roots(connection, request->ca_file))
    {
        return HTTPCLIENT_TLS;
    }
    static const unsigned char purpose[] = "lemon-chat-url-resolver";
    status = mbedtls_ctr_drbg_seed(&connection->random, mbedtls_entropy_func, &connection->entropy,
        purpose, sizeof(purpose) - 1);
    if (status == 0)
    {
        status = mbedtls_ssl_config_defaults(&connection->config, MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    }
    if (status == 0)
    {
        mbedtls_ssl_conf_authmode(&connection->config, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_min_tls_version(&connection->config, MBEDTLS_SSL_VERSION_TLS1_2);
        // The bundled library has no locks for the shared key storage used by
        // TLS 1.3. Use TLS 1.2 here so simultaneous downloads keep all their
        // encryption state inside their own connection objects.
        mbedtls_ssl_conf_max_tls_version(&connection->config, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_ca_chain(&connection->config, &connection->roots, NULL);
        mbedtls_ssl_conf_rng(&connection->config, mbedtls_ctr_drbg_random, &connection->random);
        status = mbedtls_ssl_setup(&connection->ssl, &connection->config);
    }
    if (status == 0)
    {
        status = mbedtls_ssl_set_hostname(&connection->ssl, url->host);
    }
    if (status != 0)
    {
        response->native_error = (uint)status;
        return HTTPCLIENT_TLS;
    }
    mbedtls_ssl_set_bio(&connection->ssl, connection, _httpclient_posix__tls_send, _httpclient_posix__tls_receive, NULL);
    for (;;)
    {
        status = mbedtls_ssl_handshake(&connection->ssl);
        if (status == 0)
        {
            return HTTPCLIENT_OK;
        }
        if (status != MBEDTLS_ERR_SSL_WANT_READ && status != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            response->native_error = (uint)status;
            return HTTPCLIENT_TLS;
        }
        error = _httpclient_posix__wait(connection, status == MBEDTLS_ERR_SSL_WANT_WRITE);
        if (error != HTTPCLIENT_OK)
        {
            return error;
        }
    }
}

httpclient_error_t httpclient_transport__write(httpclient_connection_t* connection, const void* data, uint size)
{
    uint sent = 0;
    while (sent < size)
    {
        if (_httpclient_posix__now() >= connection->deadline)
        {
            return HTTPCLIENT_TIMEOUT;
        }
        int status = connection->is_https
            ? mbedtls_ssl_write(&connection->ssl, (const unsigned char*)data + sent, size - sent)
            : _httpclient_posix__tls_send(connection, (const unsigned char*)data + sent, size - sent);
        if (status > 0)
        {
            sent += (uint)status;
            continue;
        }
        if (status != MBEDTLS_ERR_SSL_WANT_READ && status != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            connection->response->native_error = (uint)status;
            return connection->is_https ? HTTPCLIENT_TLS : HTTPCLIENT_SOCKET;
        }
        httpclient_error_t error = _httpclient_posix__wait(connection, status == MBEDTLS_ERR_SSL_WANT_WRITE);
        if (error != HTTPCLIENT_OK)
        {
            return error;
        }
    }
    return HTTPCLIENT_OK;
}

httpclient_error_t httpclient_transport__read(httpclient_connection_t* connection, http_parser_t* parser, boole* closed)
{
    unsigned char bytes[16384];
    *closed = 0;
    for (;;)
    {
        if (_httpclient_posix__now() >= connection->deadline)
        {
            return HTTPCLIENT_TIMEOUT;
        }
        int status = connection->is_https ? mbedtls_ssl_read(&connection->ssl, bytes, sizeof(bytes))
            : _httpclient_posix__tls_receive(connection, bytes, sizeof(bytes));
        if (status > 0)
        {
            httpclient_error_t error = _httpclient_internal__append(&parser->pending, bytes, (uint)status,
                HTTP_HEADER_LIMIT + TLS_BUFFER_LIMIT);
            return error == HTTPCLIENT_OK ? _httpclient_internal__parse(parser, connection->response) : error;
        }
        if (status == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || (status == 0 && !connection->is_https))
        {
            *closed = 1;
            return HTTPCLIENT_OK;
        }
        if (status == 0)
        {
            return HTTPCLIENT_TRUNCATED;
        }
        if (status != MBEDTLS_ERR_SSL_WANT_READ && status != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            connection->response->native_error = (uint)status;
            return connection->is_https ? HTTPCLIENT_TLS : HTTPCLIENT_SOCKET;
        }
        httpclient_error_t error = _httpclient_posix__wait(connection, status == MBEDTLS_ERR_SSL_WANT_WRITE);
        if (error != HTTPCLIENT_OK)
        {
            return error;
        }
    }
}

void httpclient_transport__close(httpclient_connection_t* connection)
{
    if (connection == NULL)
    {
        return;
    }
    if (connection->socket >= 0)
    {
        close(connection->socket);
    }
    mbedtls_ssl_free(&connection->ssl);
    mbedtls_ssl_config_free(&connection->config);
    mbedtls_x509_crt_free(&connection->roots);
    mbedtls_ctr_drbg_free(&connection->random);
    mbedtls_entropy_free(&connection->entropy);
    free(connection);
}
