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
    int socket; // POSIX socket descriptors use int.
    boole is_https;
    uint64 deadline;
    httpclient_response_t *response;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt roots;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
};

// declarations
static uint64 _httpclient_posix_internal__now(void);
static httpclient_error_t _httpclient_posix_internal__wait(httpclient_connection_t *connection, boole writing);

// These signatures must match the callbacks passed to mbedtls_ssl_set_bio().
static int _httpclient_posix_internal__tls_send(void *context, const ubyte *bytes, size_t size);
static int _httpclient_posix_internal__tls_receive(void *context, ubyte *bytes, size_t size);
static boole _httpclient_posix_internal__load_roots(httpclient_connection_t *connection, const char *ca_file);

/**
 * @brief Reads the clock used to measure how long a URL preview connection has been waiting.
 *
 * This clock moves forward independently of changes to the computer's date and time.
 *
 * @return uint64 -> the monotonic clock value in milliseconds
 */
static uint64 _httpclient_posix_internal__now(void)
{
    struct timespec now = { 0 };

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64)now.tv_sec * 1000 + (uint64)now.tv_nsec / 1000000;
}

/**
 * @brief Waits for a URL preview socket to become ready or reach its existing time limit.
 *
 * The same deadline covers connecting, TLS negotiation and all body reads.
 * A website that keeps sending small pieces does not restart the time limit.
 *
 * @param httpclient_connection_t* connection -> the connection whose socket and deadline are checked
 * @param boole writing -> TRUE to wait for writing, or FALSE to wait for reading
 *
 * @return httpclient_error_t -> HTTPCLIENT_OK when the socket reports an event, or a timeout or socket error
 */
static httpclient_error_t _httpclient_posix_internal__wait(httpclient_connection_t *connection, boole writing)
{
    uint64 now = 0;
    uint64 remaining = 0;
    int64 status = 0;
    struct pollfd descriptor = { 0 };

    descriptor.fd = connection->socket;
    descriptor.events = writing ? POLLOUT : POLLIN;
    for (;;)
    {
        // Check shared deadline
        now = _httpclient_posix_internal__now();
        if (now >= connection->deadline)
        {
            return HTTPCLIENT_TIMEOUT;
        }
        remaining = connection->deadline - now;

        // Wait for socket
        status = poll(&descriptor, 1, remaining > INT_MAX ? INT_MAX : (int)remaining);
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
            continue;
        }
        if (errno != EINTR)
        {
            connection->response->native_error = (uint)errno;
            return HTTPCLIENT_SOCKET;
        }
    }
}

/**
 * @brief Sends bytes on a URL preview socket and reports the result in the form expected by
 *        mbedTLS.
 *
 * @param void* context -> the httpclient_connection_t that owns the socket
 * @param const ubyte* bytes -> the bytes to send
 * @param size_t size -> the number of bytes available to send
 *
 * @return int -> the number of bytes sent, a retry code when writing must wait, or a send error
 */
static int _httpclient_posix_internal__tls_send(void *context, const ubyte *bytes, size_t size)
{
    httpclient_connection_t *connection = context;

    // The callback reports an int byte count, even when size_t is wider.
    uint64 amount = size > INT_MAX ? INT_MAX : size;
    int64 sent = send(connection->socket, bytes, amount, MSG_NOSIGNAL);

    // Handle socket errors
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

/**
 * @brief Reads bytes from a URL preview socket and reports the result in the form expected by
 *        mbedTLS.
 *
 * @param void* context -> the httpclient_connection_t that owns the socket
 * @param ubyte* bytes -> the buffer that receives the bytes
 * @param size_t size -> the size of that buffer in bytes
 *
 * @return int -> the number of bytes read, zero for a closed socket, a retry code, or a receive error
 */
static int _httpclient_posix_internal__tls_receive(void *context, ubyte *bytes, size_t size)
{
    httpclient_connection_t *connection = context;

    // The callback reports an int byte count, even when size_t is wider.
    uint64 amount = size > INT_MAX ? INT_MAX : size;
    int64 received = recv(connection->socket, bytes, amount, 0);

    // Handle socket errors
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

/**
 * @brief Loads trusted certificates so an HTTPS preview can check the website's certificate.
 *
 * A supplied certificate file is used on its own. Otherwise the function reads
 * the certificates trusted by macOS or looks in the usual Linux and Android locations.
 *
 * @param httpclient_connection_t* connection -> the connection whose trusted certificate list is filled
 * @param const char* ca_file -> a certificate file path, or NULL to use the operating system's certificates
 *
 * @return boole -> TRUE when at least one certificate was loaded, otherwise FALSE
 */
static boole _httpclient_posix_internal__load_roots(httpclient_connection_t *connection, const char *ca_file)
{
#ifdef __APPLE__
    CFArrayRef certificates = NULL;
    SecCertificateRef certificate = NULL;
    CFDataRef bytes = NULL;
    int64 index = 0;
#else
    static const char *files[] = { "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem" };
    uint64 index = 0;
#endif

    // Use specified certificates
    if (ca_file != NULL)
    {
        return mbedtls_x509_crt_parse_file(&connection->roots, ca_file) >= 0 && connection->roots.raw.p != NULL;
    }
#ifdef __APPLE__
    // Ask macOS for its trusted roots instead of requiring a Homebrew install
    // or a downloaded certificate bundle next to the chat server.
    if (SecTrustCopyAnchorCertificates(&certificates) == errSecSuccess && certificates != NULL)
    {
        for (index = 0; index < CFArrayGetCount(certificates); index++)
        {
            certificate = (SecCertificateRef)CFArrayGetValueAtIndex(certificates, index);
            bytes = SecCertificateCopyData(certificate);
            if (bytes != NULL)
            {
                mbedtls_x509_crt_parse_der(&connection->roots, CFDataGetBytePtr(bytes), (size_t)CFDataGetLength(bytes));
                CFRelease(bytes);
            }
        }
        CFRelease(certificates);
    }
#else
    // Try system certificates
    for (index = 0; index < sizeof(files) / sizeof(files[0]); index++)
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

/**
 * @brief Opens a connection to a website and prepares TLS when a URL preview uses HTTPS.
 *
 * Always close *output, even on failure, to release any partly initialized resources.
 *
 * @param const httpclient_request_t* request -> the timeout, allowed-address policy and other connection options
 * @param const httpclient_url_t* url -> the parsed hostname, port and HTTPS flag
 * @param httpclient_response_t* response -> receives native connection errors and must stay alive until close
 * @param httpclient_connection_t** output -> receives the allocated connection, or NULL when allocation fails
 *
 * @return httpclient_error_t -> HTTPCLIENT_OK when ready to send, or the allocation, DNS, socket, timeout or TLS error
 */
httpclient_error_t httpclient_transport__open(const httpclient_request_t *request, const httpclient_url_t *url, httpclient_response_t *response, httpclient_connection_t **output)
{
#ifdef SO_NOSIGPIPE
    int no_sigpipe = 1; // setsockopt reads an int.
#endif
    int64 status = 0;
    uint64 now = 0;
    uint64 timeout = 0;
    struct addrinfo *address = NULL_POINTER;
    int flags = 0; // fcntl expects an int mask.
    int socket_error = 0; // getsockopt writes an int.
    socklen_t length = 0;
    static const ubyte purpose[] = "lemon-chat-url-resolver";
    struct addrinfo hints = { 0 };
    struct addrinfo *addresses = NULL;
    httpclient_error_t error = HTTPCLIENT_SOCKET;
    httpclient_connection_t *connection = calloc(1, sizeof(*connection));

    // Keep cleanup handle
    *output = connection;
    if (connection == NULL)
    {
        return HTTPCLIENT_MEMORY;
    }
    connection->socket = -1;
    connection->is_https = url->is_https;
    connection->response = response;

    // Initialize TLS state
    mbedtls_ssl_init(&connection->ssl);
    mbedtls_ssl_config_init(&connection->config);
    mbedtls_x509_crt_init(&connection->roots);
    mbedtls_entropy_init(&connection->entropy);
    mbedtls_ctr_drbg_init(&connection->random);

    // Resolve website address
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    status = getaddrinfo(url->host, url->port, &hints, &addresses);
    if (status != 0)
    {
        response->native_error = (uint)status;
        return HTTPCLIENT_DNS;
    }

    // Set shared deadline
    now = _httpclient_posix_internal__now();
    timeout = request->timeout_ms == 0 ? 15000 : request->timeout_ms;
    if (timeout > UINT64_MAX - now)
    {
        freeaddrinfo(addresses);
        return HTTPCLIENT_ARGUMENT;
    }
    connection->deadline = now + timeout;

    // Try allowed addresses
    for (address = addresses; address != NULL; address = address->ai_next)
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
        setsockopt(connection->socket, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
        // Enable nonblocking socket
        flags = fcntl(connection->socket, F_GETFL, 0);
        status = flags < 0 ? -1 : fcntl(connection->socket, F_SETFL, flags | O_NONBLOCK);
        if (status == 0)
        {
            status = connect(connection->socket, address->ai_addr, address->ai_addrlen);
            error = status == 0 ? HTTPCLIENT_OK : HTTPCLIENT_SOCKET;
            if (status < 0 && errno == EINPROGRESS)
            {
                error = _httpclient_posix_internal__wait(connection, 1);

                // Check completed connection
                socket_error = 0;
                length = sizeof(socket_error);
                if (error == HTTPCLIENT_OK && (getsockopt(connection->socket, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0 || socket_error != 0))
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

    // Load trusted certificates
    if (!_httpclient_posix_internal__load_roots(connection, request->ca_file))
    {
        return HTTPCLIENT_TLS;
    }

    // Seed TLS randomness
    status = mbedtls_ctr_drbg_seed(&connection->random, mbedtls_entropy_func, &connection->entropy, purpose, sizeof(purpose) - 1);
    if (status == 0)
    {
        status = mbedtls_ssl_config_defaults(&connection->config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    }
    if (status == 0)
    {
        // Require certificate verification
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
        // Set expected hostname
        status = mbedtls_ssl_set_hostname(&connection->ssl, url->host);
    }
    if (status != 0)
    {
        response->native_error = (uint)status;
        return HTTPCLIENT_TLS;
    }

    // Connect TLS callbacks
    mbedtls_ssl_set_bio(&connection->ssl, connection, _httpclient_posix_internal__tls_send, _httpclient_posix_internal__tls_receive, NULL);
    for (;;)
    {
        // Continue TLS handshake
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
        error = _httpclient_posix_internal__wait(connection, status == MBEDTLS_ERR_SSL_WANT_WRITE);
        if (error != HTTPCLIENT_OK)
        {
            return error;
        }
    }
}

/**
 * @brief Sends all request bytes for a URL preview, encrypting them when the connection uses HTTPS.
 *
 * @param httpclient_connection_t* connection -> the open connection and the time limit shared by the request
 * @param const void* data -> the request bytes, kept alive by the caller until this function returns
 * @param uint64 size -> the number of bytes to send
 *
 * @return httpclient_error_t -> HTTPCLIENT_OK when all bytes were sent, or a TLS, socket or timeout error
 */
httpclient_error_t httpclient_transport__write(httpclient_connection_t *connection, const void *data, uint64 size)
{
    uint64 amount = 0;
    int64 status = 0;
    httpclient_error_t error = 0;
    uint64 sent = 0;

    // Check native limit
    if (size > SIZE_MAX)
    {
        return HTTPCLIENT_LIMIT;
    }
    while (sent < size)
    {
        if (_httpclient_posix_internal__now() >= connection->deadline)
        {
            return HTTPCLIENT_TIMEOUT;
        }

        // Limit socket chunk
        amount = size - sent > INT_MAX ? INT_MAX : size - sent;

        // Send request bytes
        status = connection->is_https ? mbedtls_ssl_write(&connection->ssl, (const ubyte *)data + sent, amount) : _httpclient_posix_internal__tls_send(connection, (const ubyte *)data + sent, amount);
        if (status > 0)
        {
            sent += (uint64)status;
            continue;
        }
        if (status != MBEDTLS_ERR_SSL_WANT_READ && status != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            connection->response->native_error = (uint)status;
            return connection->is_https ? HTTPCLIENT_TLS : HTTPCLIENT_SOCKET;
        }

        // Wait before retry
        error = _httpclient_posix_internal__wait(connection, status == MBEDTLS_ERR_SSL_WANT_WRITE);
        if (error != HTTPCLIENT_OK)
        {
            return error;
        }
    }
    return HTTPCLIENT_OK;
}

/**
 * @brief Reads URL preview response data, decrypts HTTPS data if needed and continues parsing the
 *        response.
 *
 * HTTPS socket closure without a TLS close notification is reported as truncated.
 *
 * @param httpclient_connection_t* connection -> the open connection and its response object
 * @param http_parser_t* parser -> the reading state and buffers for the HTTP response
 * @param boole* closed -> set to 1 when the connection ends cleanly, or 0 otherwise
 *
 * @return httpclient_error_t -> HTTPCLIENT_OK after processing data or a clean closure, or the connection, TLS or HTTP error
 */
httpclient_error_t httpclient_transport__read(httpclient_connection_t *connection, http_parser_t *parser, boole *closed)
{
    int64 status = 0;
    httpclient_error_t parse_error = 0;
    httpclient_error_t error = 0;
    ubyte bytes[16384] = { 0 };

    *closed = 0;
    for (;;)
    {
        if (_httpclient_posix_internal__now() >= connection->deadline)
        {
            return HTTPCLIENT_TIMEOUT;
        }

        // Read response bytes
        status = connection->is_https ? mbedtls_ssl_read(&connection->ssl, bytes, sizeof(bytes)) : _httpclient_posix_internal__tls_receive(connection, bytes, sizeof(bytes));
        if (status > 0)
        {
            // Parse received bytes
            parse_error = _httpclient_internal__append(&parser->pending, bytes, (uint64)status, HTTP_HEADER_LIMIT + TLS_BUFFER_LIMIT);
            return parse_error == HTTPCLIENT_OK ? _httpclient_internal__parse(parser, connection->response) : parse_error;
        }

        // Check clean closure
        if (status == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || (status == 0 && !connection->is_https))
        {
            *closed = 1;
            return HTTPCLIENT_OK;
        }

        // Reject truncated TLS
        if (status == 0)
        {
            return HTTPCLIENT_TRUNCATED;
        }
        if (status != MBEDTLS_ERR_SSL_WANT_READ && status != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            connection->response->native_error = (uint)status;
            return connection->is_https ? HTTPCLIENT_TLS : HTTPCLIENT_SOCKET;
        }

        // Wait before retry
        error = _httpclient_posix_internal__wait(connection, status == MBEDTLS_ERR_SSL_WANT_WRITE);
        if (error != HTTPCLIENT_OK)
        {
            return error;
        }
    }
}

/**
 * @brief Closes a Linux or macOS URL preview connection and frees its socket and TLS resources.
 *
 * @param httpclient_connection_t* connection -> the connection to release, including one that only partly opened, or NULL
 *
 * @return void
 */
void httpclient_transport__close(httpclient_connection_t *connection)
{
    if (connection == NULL)
    {
        return;
    }
    if (connection->socket >= 0)
    {
        close(connection->socket);
    }

    // Release TLS state
    mbedtls_ssl_free(&connection->ssl);
    mbedtls_ssl_config_free(&connection->config);
    mbedtls_x509_crt_free(&connection->roots);
    mbedtls_ctr_drbg_free(&connection->random);
    mbedtls_entropy_free(&connection->entropy);
    free(connection);
}
