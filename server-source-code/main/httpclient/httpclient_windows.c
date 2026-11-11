#include "../clib/clib.h"
// This file provides the Windows connections used for URL previews when a
// client posts a website link in chat. The feature must be enabled in the
// server settings. It lets the server download the page and preview picture
// that will be shown below the message.
//
// The connection code comes from the supplied winhttpc client. Winsock opens
// the connection to the website. Schannel, the Windows TLS library, checks the
// website's certificate and encrypts HTTPS traffic using Windows' trusted
// certificates.
#define SECURITY_WIN32
#define SCHANNEL_USE_BLACKLISTS
#include "httpclient_internal.h"
#include <windows.h>
#include <security.h>
#include <schannel.h>

typedef struct https_connection_t
{
    SOCKET socket;
    CredHandle credential;
    CtxtHandle context;
    SecPkgContext_StreamSizes sizes;
    boole has_credential;
    boole has_context;
    boole winsock_started;
    boole is_https;
    boole public_addresses_only;
    uint64 deadline;
    uint64 used;
    ubyte encrypted[TLS_BUFFER_LIMIT];
    WCHAR hostname[256];
    httpclient_response_t *response;
} https_connection_t;

// declarations
static httpclient_error_t _httpclient_internal__wait(https_connection_t *connection, boole writing);
static httpclient_error_t _httpclient_internal__send(https_connection_t *connection, const void *data, uint64 size);
static httpclient_error_t _httpclient_internal__receive(https_connection_t *connection, boole *eof);
static httpclient_error_t _httpclient_internal__handshake(https_connection_t *connection, boole initial);
static httpclient_error_t _httpclient_internal__encrypt(https_connection_t *connection, const void *data, uint64 size);
static httpclient_error_t _httpclient_internal__read(https_connection_t *connection, http_parser_t *parser, boole *closed);
static httpclient_error_t _httpclient_internal__connect(https_connection_t *connection, const char *host, const char *port, uint64 timeout);

/**
 * @brief Waits until a URL preview socket is ready for reading or writing, within its existing time
 *        limit.
 *
 * @param https_connection_t* connection -> the socket, deadline and response error field to use
 * @param boole writing -> nonzero to wait for writing, or zero to wait for reading
 *
 * @return httpclient_error_t -> HTTPCLIENT_OK when ready, HTTPCLIENT_TIMEOUT when time runs out, or HTTPCLIENT_SOCKET on failure
 */
static httpclient_error_t _httpclient_internal__wait(https_connection_t *connection, boole writing)
{
    fd_set ready;
    fd_set failed;
    struct timeval timeout;
    uint64 now = 0;
    uint64 remaining = 0;
    uint64 wait_ms = 0;
    int64 result = 0;
    int socket_error = 0; // getsockopt writes an int
    int size = sizeof(socket_error); // getsockopt requires int*

    // repeat until an event
    for (;;)
    {
        // check shared deadline
        now = GetTickCount64();
        if (now >= connection->deadline)
        {
            return HTTPCLIENT_TIMEOUT;
        }

        // set the wait time
        remaining = connection->deadline - now;
        wait_ms = remaining > INT_MAX ? INT_MAX : remaining;
        timeout.tv_sec = (long)(wait_ms / 1000);
        timeout.tv_usec = (long)((wait_ms % 1000) * 1000);

        // prepare socket wait
        FD_ZERO(&ready);
        FD_ZERO(&failed);
        FD_SET(connection->socket, &ready);
        FD_SET(connection->socket, &failed);

        // wait for the socket
        result = select(0, writing ? NULL : &ready, writing ? &ready : NULL, &failed, &timeout);
        if (result == 0)
        {
            continue;
        }

        // report socket error
        if (result == SOCKET_ERROR || FD_ISSET(connection->socket, &failed))
        {
            socket_error = WSAGetLastError();
            if (result != SOCKET_ERROR && getsockopt(connection->socket, SOL_SOCKET, SO_ERROR, (char *)&socket_error, &size) != 0)
            {
                socket_error = WSAGetLastError();
            }
            connection->response->native_error = (uint)socket_error;
            return HTTPCLIENT_SOCKET;
        }
        return HTTPCLIENT_OK;
    }
}

/**
 * @brief Sends all supplied bytes through the Windows socket before the URL preview deadline.
 *
 * @param https_connection_t* connection -> the open socket and time limit to use
 * @param const void* data -> the bytes to send; this function does not encrypt them
 * @param uint64 size -> the number of bytes to send
 *
 * @return httpclient_error_t -> HTTPCLIENT_OK when every byte was sent, or a timeout or socket error
 */
static httpclient_error_t _httpclient_internal__send(https_connection_t *connection, const void *data, uint64 size)
{
    uint64 amount = 0;
    uint64 position = 0;
    int64 result = 0;
    int64 native = 0;
    httpclient_error_t error = HTTPCLIENT_OK;

    // send all bytes
    while (position < size)
    {
        // wait for the socket
        error = _httpclient_internal__wait(connection, 1);
        if (error != HTTPCLIENT_OK)
        {
            return error;
        }

        // limit socket chunk
        amount = size - position > INT_MAX ? INT_MAX : size - position;

        // send the bytes
        result = send(connection->socket, (const char *)data + position, (int)amount, 0);

        // handle socket errors
        if (result <= 0)
        {
            native = result < 0 ? WSAGetLastError() : WSAECONNRESET;
            if (native == WSAEWOULDBLOCK)
            {
                continue;
            }
            connection->response->native_error = (uint)native;
            return HTTPCLIENT_SOCKET;
        }

        // count sent bytes
        position += (uint64)result;
    }
    return HTTPCLIENT_OK;
}

/**
 * @brief Reads more socket bytes into the connection buffer without discarding bytes already
 *        waiting there.
 *
 * @param https_connection_t* connection -> the socket, input buffer and time limit to use
 * @param boole* eof -> set to 1 when the socket closes normally, or 0 while it remains open
 *
 * @return httpclient_error_t -> HTTPCLIENT_OK for a successful read or closure, or a size, timeout or socket error
 */
static httpclient_error_t _httpclient_internal__receive(https_connection_t *connection, boole *eof)
{
    int64 received = 0;
    int64 native = 0;
    httpclient_error_t error = HTTPCLIENT_OK;

    // clear the eof flag
    *eof = 0;

    // check receive capacity
    if (connection->used == TLS_BUFFER_LIMIT)
    {
        return HTTPCLIENT_LIMIT;
    }

    // repeat until data arrives
    for (;;)
    {
        // wait for the socket
        error = _httpclient_internal__wait(connection, 0);
        if (error != HTTPCLIENT_OK)
        {
            return error;
        }

        // append received bytes
        received = recv(connection->socket, (char *)connection->encrypted + connection->used, (int)(TLS_BUFFER_LIMIT - connection->used), 0);
        if (received >= 0)
        {
            *eof = received == 0;
            connection->used += (uint64)received;
            return HTTPCLIENT_OK;
        }

        // report socket error
        native = WSAGetLastError();
        if (native != WSAEWOULDBLOCK)
        {
            connection->response->native_error = (uint)native;
            return HTTPCLIENT_SOCKET;
        }
    }
}

/**
 * @brief Establishes or continues the encrypted HTTPS connection used to download a URL preview.
 *
 * Unread bytes stay in the connection buffer; the response records the TLS version.
 *
 * @param https_connection_t* connection -> the socket, Schannel state, website name and time limit
 * @param boole initial -> nonzero for the first TLS handshake, or zero to continue an existing TLS connection
 *
 * @return httpclient_error_t -> HTTPCLIENT_OK when TLS is ready, or the TLS, socket, timeout or response error
 */
static httpclient_error_t _httpclient_internal__handshake(https_connection_t *connection, boole initial)
{
    SecBuffer input[2];
    SecBuffer output[2];
    SecBufferDesc incoming;
    SecBufferDesc outgoing;
    SecPkgContext_ConnectionInfo information;
    TimeStamp expiry;
    int64 status = SEC_E_OK;
    int64 completed = SEC_E_OK;
    ULONG attributes = 0; // Schannel writes a ULONG
    uint64 flags = ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_CONFIDENTIALITY | ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT | ISC_REQ_STREAM | ISC_REQ_EXTENDED_ERROR | ISC_REQ_USE_SUPPLIED_CREDS;
    uint64 extra = 0;
    uint64 index = 0;
    boole first = initial;
    boole eof = 0;
    boole need_more = 0;
    httpclient_error_t error = HTTPCLIENT_OK;

    // repeat until handshake ends
    for (;;)
    {
        // check the deadline
        if (GetTickCount64() >= connection->deadline)
        {
            return HTTPCLIENT_TIMEOUT;
        }

        // read handshake bytes
        if (need_more)
        {
            error = _httpclient_internal__receive(connection, &eof);
            if (error != HTTPCLIENT_OK || eof)
            {
                return eof ? HTTPCLIENT_TRUNCATED : error;
            }
        }

        // prepare TLS buffers
        clib__memset(input, 0, sizeof(input));
        clib__memset(output, 0, sizeof(output));
        input[0].BufferType = SECBUFFER_TOKEN;
        input[0].pvBuffer = connection->encrypted;
        input[0].cbBuffer = (ULONG)connection->used;
        input[1].BufferType = SECBUFFER_EMPTY;
        output[0].BufferType = SECBUFFER_TOKEN;
        output[1].BufferType = SECBUFFER_ALERT;
        incoming.ulVersion = SECBUFFER_VERSION;
        incoming.cBuffers = 2;
        incoming.pBuffers = input;
        outgoing.ulVersion = SECBUFFER_VERSION;
        outgoing.cBuffers = 2;
        outgoing.pBuffers = output;

        // continue TLS handshake
        status = InitializeSecurityContextW(&connection->credential, first ? NULL : &connection->context, connection->hostname, (ULONG)flags, 0, 0, first ? NULL : &incoming, 0, &connection->context, &outgoing, &attributes, &expiry);
        connection->has_context = (boole)SecIsValidHandle(&connection->context);
        first = 0;

        // complete the token
        if (status == SEC_I_COMPLETE_NEEDED || status == SEC_I_COMPLETE_AND_CONTINUE)
        {
            completed = CompleteAuthToken(&connection->context, &outgoing);
            if (completed != SEC_E_OK)
            {
                status = completed;
            }
            else
            {
                status = status == SEC_I_COMPLETE_NEEDED ? SEC_E_OK : SEC_I_CONTINUE_NEEDED;
            }
        }

        // a successful final handshake can still have a token that must be sent
        if (output[0].cbBuffer != 0 && output[0].pvBuffer != NULL)
        {
            error = _httpclient_internal__send(connection, output[0].pvBuffer, output[0].cbBuffer);
        }

        // release TLS output
        for (index = 0; index < 2; index++)
        {
            if (output[index].pvBuffer != NULL)
            {
                FreeContextBuffer(output[index].pvBuffer);
            }
        }

        // report send failure
        if (error != HTTPCLIENT_OK)
        {
            return error;
        }

        // more bytes are needed
        if (status == SEC_E_INCOMPLETE_MESSAGE)
        {
            need_more = 1;
            continue;
        }

        // report the TLS error
        if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED)
        {
            connection->response->native_error = (uint)status;
            return status == SEC_I_INCOMPLETE_CREDENTIALS ? HTTPCLIENT_UNSUPPORTED : HTTPCLIENT_TLS;
        }

        // keep unread bytes
        extra = input[1].BufferType == SECBUFFER_EXTRA ? input[1].cbBuffer : 0;
        if (extra > connection->used)
        {
            return HTTPCLIENT_TLS;
        }
        if (extra != 0)
        {
            clib__memmove(connection->encrypted, connection->encrypted + connection->used - extra, extra);
        }
        connection->used = extra;

        // handshake is complete
        if (status == SEC_E_OK)
        {
            // check connection attributes
            if ((attributes & (ISC_RET_CONFIDENTIALITY | ISC_RET_STREAM)) != (ISC_RET_CONFIDENTIALITY | ISC_RET_STREAM))
            {
                return HTTPCLIENT_TLS;
            }

            // read connection settings
            status = QueryContextAttributesW(&connection->context, SECPKG_ATTR_STREAM_SIZES, &connection->sizes);
            if (status == SEC_E_OK)
            {
                status = QueryContextAttributesW(&connection->context, SECPKG_ATTR_CONNECTION_INFO, &information);
            }
            if (status != SEC_E_OK)
            {
                connection->response->native_error = (uint)status;
                return HTTPCLIENT_TLS;
            }

            // store TLS version
            connection->response->tls_protocol = information.dwProtocol;

            // check negotiated limits
            if ((information.dwProtocol & (SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_3_CLIENT)) == 0 || connection->sizes.cbMaximumMessage == 0 || connection->sizes.cbMaximumMessage > TLS_BUFFER_LIMIT || connection->sizes.cbHeader > TLS_BUFFER_LIMIT || connection->sizes.cbTrailer > TLS_BUFFER_LIMIT)
            {
                return HTTPCLIENT_TLS;
            }
            return HTTPCLIENT_OK;
        }

        // read more when empty
        need_more = extra == 0;
    }
}

/**
 * @brief Encrypts request bytes with Schannel and sends all resulting HTTPS data.
 *
 * @param https_connection_t* connection -> the established TLS connection and its time limit
 * @param const void* data -> the unencrypted request bytes to send
 * @param uint64 size -> the number of request bytes
 *
 * @return httpclient_error_t -> HTTPCLIENT_OK when all bytes were sent, or an allocation, TLS, socket or timeout error
 */
static httpclient_error_t _httpclient_internal__encrypt(https_connection_t *connection, const void *data, uint64 size)
{
    ubyte *record = NULL_POINTER;
    SecBuffer buffers[4];
    SecBufferDesc message;
    int64 status = SEC_E_OK;
    uint64 amount = 0;
    uint64 position = 0;
    uint64 index = 0;
    uint64 capacity = 0;
    httpclient_error_t error = HTTPCLIENT_OK;

    // allocate TLS record
    capacity = (uint64)connection->sizes.cbHeader + connection->sizes.cbMaximumMessage + connection->sizes.cbTrailer;
    if (capacity > SIZE_MAX)
    {
        return HTTPCLIENT_LIMIT;
    }
    record = (ubyte *)malloc(capacity);
    if (record == NULL)
    {
        return HTTPCLIENT_MEMORY;
    }

    // repeat for each record
    while (position < size && error == HTTPCLIENT_OK)
    {
        // split request bytes
        amount = size - position;
        if (amount > connection->sizes.cbMaximumMessage)
        {
            amount = connection->sizes.cbMaximumMessage;
        }

        // prepare TLS record
        clib__memset(buffers, 0, sizeof(buffers));
        buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
        buffers[0].pvBuffer = record;
        buffers[0].cbBuffer = connection->sizes.cbHeader;
        buffers[1].BufferType = SECBUFFER_DATA;
        buffers[1].pvBuffer = record + connection->sizes.cbHeader;
        buffers[1].cbBuffer = (ULONG)amount;
        buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
        buffers[2].pvBuffer = record + connection->sizes.cbHeader + amount;
        buffers[2].cbBuffer = connection->sizes.cbTrailer;
        buffers[3].BufferType = SECBUFFER_EMPTY;

        // copy request bytes
        clib__memcpy(buffers[1].pvBuffer, (const ubyte *)data + position, amount);

        // fill buffer description
        message.ulVersion = SECBUFFER_VERSION;
        message.cBuffers = 4;
        message.pBuffers = buffers;

        // encrypt request bytes
        status = EncryptMessage(&connection->context, 0, &message, 0);
        if (status != SEC_E_OK)
        {
            connection->response->native_error = (uint)status;
            error = HTTPCLIENT_TLS;
            break;
        }

        // send encrypted record
        for (index = 0; index < 3 && error == HTTPCLIENT_OK; index++)
        {
            error = _httpclient_internal__send(connection, buffers[index].pvBuffer, buffers[index].cbBuffer);
        }

        // count sent bytes
        position += amount;
    }

    // free the record
    free(record);
    return error;
}

/**
 * @brief Decrypts HTTPS response bytes and passes the resulting page data to the HTTP parser.
 *
 * TLS control messages can be handled without adding page bytes.
 *
 * @param https_connection_t* connection -> the established TLS connection and any received bytes still waiting
 * @param http_parser_t* parser -> receives the decrypted headers and body bytes
 * @param boole* closed -> set to 1 for a TLS close notification, or 0 otherwise
 *
 * @return httpclient_error_t -> HTTPCLIENT_OK after processing available TLS data, or the connection, TLS or HTTP error
 */
static httpclient_error_t _httpclient_internal__read(https_connection_t *connection, http_parser_t *parser, boole *closed)
{
    SecBuffer buffers[4];
    SecBufferDesc message;
    int64 status = SEC_E_OK;
    uint64 index = 0;
    uint64 extra = 0;
    boole eof = 0;
    boole need_more = 0;
    httpclient_error_t error = HTTPCLIENT_OK;

    // set starting values
    *closed = 0;
    need_more = connection->used == 0;

    // repeat for incomplete records
    for (;;)
    {
        // check the deadline
        if (GetTickCount64() >= connection->deadline)
        {
            return HTTPCLIENT_TIMEOUT;
        }

        // read encrypted bytes
        if (need_more)
        {
            error = _httpclient_internal__receive(connection, &eof);
            if (error != HTTPCLIENT_OK || eof)
            {
                return eof ? HTTPCLIENT_TRUNCATED : error;
            }
        }

        // prepare TLS buffers
        clib__memset(buffers, 0, sizeof(buffers));
        buffers[0].BufferType = SECBUFFER_DATA;
        buffers[0].pvBuffer = connection->encrypted;
        buffers[0].cbBuffer = (ULONG)connection->used;
        message.ulVersion = SECBUFFER_VERSION;
        message.cBuffers = 4;
        message.pBuffers = buffers;

        // decrypt response bytes
        status = DecryptMessage(&connection->context, &message, 0, NULL);

        // await complete record
        if (status == SEC_E_INCOMPLETE_MESSAGE)
        {
            need_more = 1;
            continue;
        }

        // report the TLS error
        if (status != SEC_E_OK && status != SEC_I_CONTEXT_EXPIRED && status != SEC_I_RENEGOTIATE)
        {
            connection->response->native_error = (uint)status;
            return HTTPCLIENT_TLS;
        }

        // a shutdown/control status does not promise application plaintext in the input DATA slot
        if (status == SEC_I_CONTEXT_EXPIRED)
        {
            *closed = 1;
            return HTTPCLIENT_OK;
        }

        // parse decrypted bytes
        extra = 0;
        for (index = 0; index < 4; index++)
        {
            if (status == SEC_E_OK && buffers[index].BufferType == SECBUFFER_DATA && buffers[index].cbBuffer != 0)
            {
                error = _httpclient_internal__append(&parser->pending, buffers[index].pvBuffer, buffers[index].cbBuffer, HTTP_HEADER_LIMIT + TLS_BUFFER_LIMIT);
                if (error == HTTPCLIENT_OK)
                {
                    error = _httpclient_internal__parse(parser, connection->response);
                }
                if (error != HTTPCLIENT_OK)
                {
                    return error;
                }
            }
            else if (buffers[index].BufferType == SECBUFFER_EXTRA)
            {
                extra = buffers[index].cbBuffer;
            }
        }

        // keep unread bytes
        if (extra > connection->used)
        {
            return HTTPCLIENT_TLS;
        }
        if (extra != 0)
        {
            clib__memmove(connection->encrypted, connection->encrypted + connection->used - extra, extra);
        }
        connection->used = extra;

        // continue the handshake
        if (status == SEC_I_RENEGOTIATE)
        {
            // TLS 1.3 post-handshake messages also use this Schannel status
            return _httpclient_internal__handshake(connection, 0);
        }
        return HTTPCLIENT_OK;
    }
}

/**
 * @brief Resolves a website name and opens a Windows socket for its URL preview download.
 *
 * @param https_connection_t* connection -> receives the open socket and shared connection deadline
 * @param const char* host -> the hostname or numeric IP address to resolve
 * @param const char* port -> the destination port written as decimal text
 * @param uint64 timeout -> the time allowed in milliseconds after DNS lookup, shared by later reads and writes
 *
 * @return httpclient_error_t -> HTTPCLIENT_OK when connected, or a DNS, socket or timeout error
 */
static httpclient_error_t _httpclient_internal__connect(https_connection_t *connection, const char *host, const char *port, uint64 timeout)
{
    uint64 now = 0;
    ADDRINFOA hints;
    ADDRINFOA *addresses = NULL_POINTER;
    ADDRINFOA *current = NULL_POINTER;
    ULONG nonblocking = 1; // ioctlsocket requires ULONG*
    int native = 0; // getsockopt writes an int
    int64 result = 0;
    int size = 0; // getsockopt requires int*
    httpclient_error_t error = HTTPCLIENT_SOCKET;

    // resolve website address
    clib__memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    native = getaddrinfo(host, port, &hints, &addresses);
    if (native != 0)
    {
        connection->response->native_error = (uint)native;
        return HTTPCLIENT_DNS;
    }

    // set shared deadline
    now = GetTickCount64();
    if (timeout > UINT64_MAX - now)
    {
        freeaddrinfo(addresses);
        return HTTPCLIENT_ARGUMENT;
    }
    connection->deadline = now + timeout;

    // try allowed addresses
    for (current = addresses; current != NULL; current = current->ai_next)
    {
        // skip addresses not allowed
        if (connection->public_addresses_only && !httpclient_internal__address_allowed(current->ai_addr))
        {
            continue;
        }

        // create the socket
        connection->socket = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (connection->socket == INVALID_SOCKET)
        {
            connection->response->native_error = (uint)WSAGetLastError();
            continue;
        }

        // enable nonblocking socket
        result = ioctlsocket(connection->socket, FIONBIO, &nonblocking);

        // connect the socket
        if (result == 0)
        {
            result = connect(connection->socket, current->ai_addr, (int)current->ai_addrlen);
        }
        native = result == 0 ? 0 : WSAGetLastError();
        error = result == 0 ? HTTPCLIENT_OK : HTTPCLIENT_SOCKET;

        // wait for the connection
        if (native == WSAEWOULDBLOCK)
        {
            error = _httpclient_internal__wait(connection, 1);
            if (error == HTTPCLIENT_OK)
            {
                // check completed connection
                size = sizeof(native);
                result = getsockopt(connection->socket, SOL_SOCKET, SO_ERROR, (char *)&native, &size);
                if (result != 0 || native != 0)
                {
                    native = result != 0 ? WSAGetLastError() : native;
                    error = HTTPCLIENT_SOCKET;
                }
            }
        }

        // stop when connected
        if (error == HTTPCLIENT_OK)
        {
            connection->response->native_error = 0;
            break;
        }

        // store the socket error
        if (native != 0 && native != WSAEWOULDBLOCK)
        {
            connection->response->native_error = (uint)native;
        }

        // discard failed socket
        closesocket(connection->socket);
        connection->socket = INVALID_SOCKET;

        // stop after timeout
        if (error == HTTPCLIENT_TIMEOUT)
        {
            break;
        }
    }

    // free the addresses
    freeaddrinfo(addresses);
    return error;
}

/**
 * @brief Closes a Windows URL preview connection and frees its socket and TLS resources.
 *
 * @param https_connection_t* connection -> the connection to release, including one that only partly opened, or NULL
 *
 * @return void
 */
void httpclient_transport__close(https_connection_t *connection)
{
    // nothing to close
    if (connection == NULL)
    {
        return;
    }

    // release TLS handles
    if (connection->has_context)
    {
        DeleteSecurityContext(&connection->context);
    }
    if (connection->has_credential)
    {
        FreeCredentialsHandle(&connection->credential);
    }

    // close Windows socket
    if (connection->socket != INVALID_SOCKET)
    {
        closesocket(connection->socket);
    }

    // stop Windows sockets
    if (connection->winsock_started)
    {
        WSACleanup();
    }

    // free the connection
    free(connection);
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
    uint64 index = 0;
    WSADATA winsock;
    SCH_CREDENTIALS credentials;
    TLS_PARAMETERS tls;
    TimeStamp expiry;
    int64 security = SEC_E_OK;
    httpclient_error_t error;
    int64 result;
    https_connection_t *connection = calloc(1, sizeof(*connection));

    // keep cleanup handle
    *output = connection;
    if (connection == NULL)
    {
        return HTTPCLIENT_MEMORY;
    }

    // set connection fields
    connection->socket = INVALID_SOCKET;
    connection->response = response;
    connection->is_https = (boole)url->is_https;
    connection->public_addresses_only = (boole)request->public_addresses_only;

    // start Windows sockets
    result = WSAStartup(MAKEWORD(2, 2), &winsock);
    if (result != 0)
    {
        response->native_error = (uint)result;
        return HTTPCLIENT_SOCKET;
    }
    connection->winsock_started = 1;

    // prepare for HTTPS
    if (connection->is_https)
    {
        // mark handles invalid
        SecInvalidateHandle(&connection->credential);
        SecInvalidateHandle(&connection->context);

        // copy the hostname
        for (index = 0; url->host[index] != 0; index++)
        {
            connection->hostname[index] = (WCHAR)(ubyte)url->host[index];
        }

        // prepare TLS credentials
        clib__memset(&credentials, 0, sizeof(credentials));
        clib__memset(&tls, 0, sizeof(tls));
        tls.grbitDisabledProtocols = SP_PROT_SSL2_CLIENT | SP_PROT_SSL3_CLIENT | SP_PROT_TLS1_CLIENT | SP_PROT_TLS1_1_CLIENT;
        credentials.dwVersion = SCH_CREDENTIALS_VERSION;

        // require certificate verification
        credentials.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO | SCH_CRED_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
        credentials.cTlsParameters = 1;
        credentials.pTlsParameters = &tls;

        // get TLS credentials
        security = AcquireCredentialsHandleW(NULL, UNISP_NAME_W, SECPKG_CRED_OUTBOUND, NULL, &credentials, NULL, NULL, &connection->credential, &expiry);
        if (security != SEC_E_OK)
        {
            response->native_error = (uint)security;
            return HTTPCLIENT_TLS;
        }
        connection->has_credential = 1;
    }

    // connect to website
    error = _httpclient_internal__connect(connection, url->host, url->port, request->timeout_ms == 0 ? 15000u : request->timeout_ms);
    if (error == HTTPCLIENT_OK && connection->is_https)
    {
        // establish TLS connection
        error = _httpclient_internal__handshake(connection, 1);
    }
    return error;
}

/**
 * @brief Sends all request bytes for a URL preview, encrypting them when the connection uses HTTPS.
 *
 * @param httpclient_connection_t* connection -> the open connection and the time limit shared by the request
 * @param const void* data -> the request bytes, kept alive by the caller until this function returns
 * @param uint64 size -> the number of bytes to send
 *
 * @return httpclient_error_t -> HTTPCLIENT_OK when all bytes were sent, or the allocation, TLS, socket or timeout error
 */
httpclient_error_t httpclient_transport__write(httpclient_connection_t *connection, const void *data, uint64 size)
{
    // check the size
    if (size > SIZE_MAX)
    {
        return HTTPCLIENT_LIMIT;
    }

    // encrypt for HTTPS
    if (connection->is_https)
    {
        return _httpclient_internal__encrypt(connection, data, size);
    }

    // send without encryption
    return _httpclient_internal__send(connection, data, size);
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
    httpclient_error_t error = 0;

    // read HTTPS bytes
    if (connection->is_https)
    {
        return _httpclient_internal__read(connection, parser, closed);
    }

    // read HTTP bytes
    error = _httpclient_internal__receive(connection, closed);

    // pass bytes to parser
    if (error == HTTPCLIENT_OK && connection->used != 0)
    {
        error = _httpclient_internal__append(&parser->pending, connection->encrypted, connection->used, HTTP_HEADER_LIMIT + TLS_BUFFER_LIMIT);
        connection->used = 0;
        if (error == HTTPCLIENT_OK)
        {
            // parse received bytes
            error = _httpclient_internal__parse(parser, connection->response);
        }
    }
    return error;
}
