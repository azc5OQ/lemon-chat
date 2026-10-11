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
    uint used;
    ubyte encrypted[TLS_BUFFER_LIMIT];
    WCHAR hostname[256];
    httpclient_response_t* response;
} https_connection_t;

static httpclient_error_t _httpclient_internal__wait(https_connection_t* connection, boole writing)
{
    fd_set ready;
    fd_set failed;
    struct timeval timeout;
    uint64 now = 0;
    uint64 remaining = 0;
    int result = 0;
    int socket_error = 0;
    int size = sizeof(socket_error);

    now = GetTickCount64();
    if (now >= connection->deadline)
    {
        return HTTPCLIENT_TIMEOUT;
    }
    remaining = connection->deadline - now;
    timeout.tv_sec = (long)(remaining / 1000);
    timeout.tv_usec = (long)((remaining % 1000) * 1000);
    FD_ZERO(&ready);
    FD_ZERO(&failed);
    FD_SET(connection->socket, &ready);
    FD_SET(connection->socket, &failed);
    result = select(0, writing ? NULL : &ready, writing ? &ready : NULL, &failed, &timeout);
    if (result == 0)
    {
        return HTTPCLIENT_TIMEOUT;
    }
    if (result == SOCKET_ERROR || FD_ISSET(connection->socket, &failed))
    {
        socket_error = WSAGetLastError();
        if (result != SOCKET_ERROR && getsockopt(connection->socket, SOL_SOCKET, SO_ERROR, (char*)&socket_error, &size) != 0)
        {
            socket_error = WSAGetLastError();
        }
        connection->response->native_error = (uint)socket_error;
        return HTTPCLIENT_SOCKET;
    }
    return HTTPCLIENT_OK;
}

static httpclient_error_t _httpclient_internal__send(https_connection_t* connection, const void* data, uint size)
{
    uint position = 0;
    int result = 0;
    int native = 0;
    httpclient_error_t error = HTTPCLIENT_OK;

    while (position < size)
    {
        error = _httpclient_internal__wait(connection, 1);
        if (error != HTTPCLIENT_OK)
        {
            return error;
        }
        result = send(connection->socket, (const char*)data + position, (int)(size - position), 0);
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
        position += (uint)result;
    }
    return HTTPCLIENT_OK;
}

static httpclient_error_t _httpclient_internal__receive(https_connection_t* connection, boole* eof)
{
    int received = 0;
    int native = 0;
    httpclient_error_t error = HTTPCLIENT_OK;

    *eof = 0;
    if (connection->used == TLS_BUFFER_LIMIT)
    {
        return HTTPCLIENT_LIMIT;
    }
    for (;;)
    {
        error = _httpclient_internal__wait(connection, 0);
        if (error != HTTPCLIENT_OK)
        {
            return error;
        }
        received = recv(connection->socket, (char*)connection->encrypted + connection->used, (int)(TLS_BUFFER_LIMIT - connection->used), 0);
        if (received >= 0)
        {
            *eof = received == 0;
            connection->used += (uint)received;
            return HTTPCLIENT_OK;
        }
        native = WSAGetLastError();
        if (native != WSAEWOULDBLOCK)
        {
            connection->response->native_error = (uint)native;
            return HTTPCLIENT_SOCKET;
        }
    }
}

static httpclient_error_t _httpclient_internal__handshake(https_connection_t* connection, boole initial)
{
    SecBuffer input[2];
    SecBuffer output[2];
    SecBufferDesc incoming;
    SecBufferDesc outgoing;
    SecPkgContext_ConnectionInfo information;
    TimeStamp expiry;
    SECURITY_STATUS status = SEC_E_OK;
    SECURITY_STATUS completed = SEC_E_OK;
    ULONG attributes = 0;
    ULONG flags = ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_CONFIDENTIALITY | ISC_REQ_REPLAY_DETECT |
        ISC_REQ_SEQUENCE_DETECT | ISC_REQ_STREAM | ISC_REQ_EXTENDED_ERROR | ISC_REQ_USE_SUPPLIED_CREDS;
    uint extra = 0;
    uint index = 0;
    boole first = initial;
    boole eof = 0;
    boole need_more = 0;
    httpclient_error_t error = HTTPCLIENT_OK;

    for (;;)
    {
        if (GetTickCount64() >= connection->deadline)
        {
            return HTTPCLIENT_TIMEOUT;
        }
        if (need_more)
        {
            error = _httpclient_internal__receive(connection, &eof);
            if (error != HTTPCLIENT_OK || eof)
            {
                return eof ? HTTPCLIENT_TRUNCATED : error;
            }
        }
        memset(input, 0, sizeof(input));
        memset(output, 0, sizeof(output));
        input[0].BufferType = SECBUFFER_TOKEN;
        input[0].pvBuffer = connection->encrypted;
        input[0].cbBuffer = connection->used;
        input[1].BufferType = SECBUFFER_EMPTY;
        output[0].BufferType = SECBUFFER_TOKEN;
        output[1].BufferType = SECBUFFER_ALERT;
        incoming.ulVersion = SECBUFFER_VERSION;
        incoming.cBuffers = 2;
        incoming.pBuffers = input;
        outgoing.ulVersion = SECBUFFER_VERSION;
        outgoing.cBuffers = 2;
        outgoing.pBuffers = output;
        status = InitializeSecurityContextW(&connection->credential, first ? NULL : &connection->context,
            connection->hostname, flags, 0, 0, first ? NULL : &incoming, 0,
            &connection->context, &outgoing, &attributes, &expiry);
        connection->has_context = (boole)SecIsValidHandle(&connection->context);
        first = 0;
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
        // A successful final handshake can still have a token that must be sent.
        if (output[0].cbBuffer != 0 && output[0].pvBuffer != NULL)
        {
            error = _httpclient_internal__send(connection, output[0].pvBuffer, output[0].cbBuffer);
        }
        for (index = 0; index < 2; index++)
        {
            if (output[index].pvBuffer != NULL)
            {
                FreeContextBuffer(output[index].pvBuffer);
            }
        }
        if (error != HTTPCLIENT_OK)
        {
            return error;
        }
        if (status == SEC_E_INCOMPLETE_MESSAGE)
        {
            need_more = 1;
            continue;
        }
        if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED)
        {
            connection->response->native_error = (uint)status;
            return status == SEC_I_INCOMPLETE_CREDENTIALS ? HTTPCLIENT_UNSUPPORTED : HTTPCLIENT_TLS;
        }
        extra = input[1].BufferType == SECBUFFER_EXTRA ? input[1].cbBuffer : 0;
        if (extra > connection->used)
        {
            return HTTPCLIENT_TLS;
        }
        if (extra != 0)
        {
            memmove(connection->encrypted, connection->encrypted + connection->used - extra, extra);
        }
        connection->used = extra;
        if (status == SEC_E_OK)
        {
            if ((attributes & (ISC_RET_CONFIDENTIALITY | ISC_RET_STREAM)) != (ISC_RET_CONFIDENTIALITY | ISC_RET_STREAM))
            {
                return HTTPCLIENT_TLS;
            }
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
            connection->response->tls_protocol = information.dwProtocol;
            if ((information.dwProtocol & (SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_3_CLIENT)) == 0 ||
                connection->sizes.cbMaximumMessage == 0 || connection->sizes.cbMaximumMessage > TLS_BUFFER_LIMIT ||
                connection->sizes.cbHeader > TLS_BUFFER_LIMIT || connection->sizes.cbTrailer > TLS_BUFFER_LIMIT)
            {
                return HTTPCLIENT_TLS;
            }
            return HTTPCLIENT_OK;
        }
        need_more = extra == 0;
    }
}

static httpclient_error_t _httpclient_internal__encrypt(https_connection_t* connection, const void* data, uint size)
{
    ubyte* record = NULL;
    SecBuffer buffers[4];
    SecBufferDesc message;
    SECURITY_STATUS status = SEC_E_OK;
    uint amount = 0;
    uint position = 0;
    uint index = 0;
    uint capacity = 0;
    httpclient_error_t error = HTTPCLIENT_OK;

    capacity = connection->sizes.cbHeader + connection->sizes.cbMaximumMessage + connection->sizes.cbTrailer;
    record = (ubyte*)malloc(capacity);
    if (record == NULL)
    {
        return HTTPCLIENT_MEMORY;
    }
    while (position < size && error == HTTPCLIENT_OK)
    {
        amount = size - position;
        if (amount > connection->sizes.cbMaximumMessage)
        {
            amount = connection->sizes.cbMaximumMessage;
        }
        memset(buffers, 0, sizeof(buffers));
        buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
        buffers[0].pvBuffer = record;
        buffers[0].cbBuffer = connection->sizes.cbHeader;
        buffers[1].BufferType = SECBUFFER_DATA;
        buffers[1].pvBuffer = record + connection->sizes.cbHeader;
        buffers[1].cbBuffer = amount;
        buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
        buffers[2].pvBuffer = record + connection->sizes.cbHeader + amount;
        buffers[2].cbBuffer = connection->sizes.cbTrailer;
        buffers[3].BufferType = SECBUFFER_EMPTY;
        memcpy(buffers[1].pvBuffer, (const ubyte*)data + position, amount);
        message.ulVersion = SECBUFFER_VERSION;
        message.cBuffers = 4;
        message.pBuffers = buffers;
        status = EncryptMessage(&connection->context, 0, &message, 0);
        if (status != SEC_E_OK)
        {
            connection->response->native_error = (uint)status;
            error = HTTPCLIENT_TLS;
            break;
        }
        for (index = 0; index < 3 && error == HTTPCLIENT_OK; index++)
        {
            error = _httpclient_internal__send(connection, buffers[index].pvBuffer, buffers[index].cbBuffer);
        }
        position += amount;
    }
    free(record);
    return error;
}

static httpclient_error_t _httpclient_internal__read(https_connection_t* connection, http_parser_t* parser, boole* closed)
{
    SecBuffer buffers[4];
    SecBufferDesc message;
    SECURITY_STATUS status = SEC_E_OK;
    uint index = 0;
    uint extra = 0;
    boole eof = 0;
    boole need_more = 0;
    httpclient_error_t error = HTTPCLIENT_OK;

    *closed = 0;
    need_more = connection->used == 0;
    for (;;)
    {
        if (GetTickCount64() >= connection->deadline)
        {
            return HTTPCLIENT_TIMEOUT;
        }
        if (need_more)
        {
            error = _httpclient_internal__receive(connection, &eof);
            if (error != HTTPCLIENT_OK || eof)
            {
                return eof ? HTTPCLIENT_TRUNCATED : error;
            }
        }
        memset(buffers, 0, sizeof(buffers));
        buffers[0].BufferType = SECBUFFER_DATA;
        buffers[0].pvBuffer = connection->encrypted;
        buffers[0].cbBuffer = connection->used;
        message.ulVersion = SECBUFFER_VERSION;
        message.cBuffers = 4;
        message.pBuffers = buffers;
        status = DecryptMessage(&connection->context, &message, 0, NULL);
        if (status == SEC_E_INCOMPLETE_MESSAGE)
        {
            need_more = 1;
            continue;
        }
        if (status != SEC_E_OK && status != SEC_I_CONTEXT_EXPIRED && status != SEC_I_RENEGOTIATE)
        {
            connection->response->native_error = (uint)status;
            return HTTPCLIENT_TLS;
        }
        // A shutdown/control status does not promise application plaintext in the input DATA slot.
        if (status == SEC_I_CONTEXT_EXPIRED)
        {
            *closed = 1;
            return HTTPCLIENT_OK;
        }
        extra = 0;
        for (index = 0; index < 4; index++)
        {
            if (status == SEC_E_OK && buffers[index].BufferType == SECBUFFER_DATA && buffers[index].cbBuffer != 0)
            {
                error = _httpclient_internal__append(&parser->pending, buffers[index].pvBuffer, buffers[index].cbBuffer,
                    HTTP_HEADER_LIMIT + TLS_BUFFER_LIMIT);
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
        if (extra > connection->used)
        {
            return HTTPCLIENT_TLS;
        }
        if (extra != 0)
        {
            memmove(connection->encrypted, connection->encrypted + connection->used - extra, extra);
        }
        connection->used = extra;
        if (status == SEC_I_RENEGOTIATE)
        {
            // TLS 1.3 post-handshake messages also use this Schannel status.
            return _httpclient_internal__handshake(connection, 0);
        }
        return HTTPCLIENT_OK;
    }
}

static httpclient_error_t _httpclient_internal__connect(https_connection_t* connection, const char* host, const char* port, uint timeout)
{
    ADDRINFOA hints;
    ADDRINFOA* addresses = NULL;
    ADDRINFOA* current = NULL;
    ULONG nonblocking = 1;
    int native = 0;
    int result = 0;
    int size = 0;
    httpclient_error_t error = HTTPCLIENT_SOCKET;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    native = getaddrinfo(host, port, &hints, &addresses);
    if (native != 0)
    {
        connection->response->native_error = (uint)native;
        return HTTPCLIENT_DNS;
    }
    connection->deadline = GetTickCount64() + timeout;
    for (current = addresses; current != NULL; current = current->ai_next)
    {
        if (connection->public_addresses_only && !httpclient_internal__address_allowed(current->ai_addr))
        {
            continue;
        }
        connection->socket = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (connection->socket == INVALID_SOCKET)
        {
            connection->response->native_error = (uint)WSAGetLastError();
            continue;
        }
        result = ioctlsocket(connection->socket, FIONBIO, &nonblocking);
        if (result == 0)
        {
            result = connect(connection->socket, current->ai_addr, (int)current->ai_addrlen);
        }
        native = result == 0 ? 0 : WSAGetLastError();
        error = result == 0 ? HTTPCLIENT_OK : HTTPCLIENT_SOCKET;
        if (native == WSAEWOULDBLOCK)
        {
            error = _httpclient_internal__wait(connection, 1);
            if (error == HTTPCLIENT_OK)
            {
                size = sizeof(native);
                result = getsockopt(connection->socket, SOL_SOCKET, SO_ERROR, (char*)&native, &size);
                if (result != 0 || native != 0)
                {
                    native = result != 0 ? WSAGetLastError() : native;
                    error = HTTPCLIENT_SOCKET;
                }
            }
        }
        if (error == HTTPCLIENT_OK)
        {
            connection->response->native_error = 0;
            break;
        }
        if (native != 0 && native != WSAEWOULDBLOCK)
        {
            connection->response->native_error = (uint)native;
        }
        closesocket(connection->socket);
        connection->socket = INVALID_SOCKET;
        if (error == HTTPCLIENT_TIMEOUT)
        {
            break;
        }
    }
    freeaddrinfo(addresses);
    return error;
}

void httpclient_transport__close(https_connection_t* connection)
{
    if (connection == NULL)
    {
        return;
    }
    if (connection->has_context)
    {
        DeleteSecurityContext(&connection->context);
    }
    if (connection->has_credential)
    {
        FreeCredentialsHandle(&connection->credential);
    }
    if (connection->socket != INVALID_SOCKET)
    {
        closesocket(connection->socket);
    }
    if (connection->winsock_started)
    {
        WSACleanup();
    }
    free(connection);
}

httpclient_error_t httpclient_transport__open(const httpclient_request_t* request, const httpclient_url_t* url,
    httpclient_response_t* response, httpclient_connection_t** output)
{
    WSADATA winsock;
    SCH_CREDENTIALS credentials;
    TLS_PARAMETERS tls;
    TimeStamp expiry;
    SECURITY_STATUS security;
    httpclient_error_t error;
    int result;
    https_connection_t* connection = calloc(1, sizeof(*connection));
    *output = connection;
    if (connection == NULL)
    {
        return HTTPCLIENT_MEMORY;
    }
    connection->socket = INVALID_SOCKET;
    connection->response = response;
    connection->is_https = (boole)url->is_https;
    connection->public_addresses_only = (boole)request->public_addresses_only;
    result = WSAStartup(MAKEWORD(2, 2), &winsock);
    if (result != 0)
    {
        response->native_error = (uint)result;
        return HTTPCLIENT_SOCKET;
    }
    connection->winsock_started = 1;
    if (connection->is_https)
    {
        SecInvalidateHandle(&connection->credential);
        SecInvalidateHandle(&connection->context);
        for (uint index = 0; url->host[index] != 0; index++)
        {
            connection->hostname[index] = (WCHAR)(ubyte)url->host[index];
        }
        memset(&credentials, 0, sizeof(credentials));
        memset(&tls, 0, sizeof(tls));
        tls.grbitDisabledProtocols = SP_PROT_SSL2_CLIENT | SP_PROT_SSL3_CLIENT | SP_PROT_TLS1_CLIENT | SP_PROT_TLS1_1_CLIENT;
        credentials.dwVersion = SCH_CREDENTIALS_VERSION;
        credentials.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO |
            SCH_CRED_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
        credentials.cTlsParameters = 1;
        credentials.pTlsParameters = &tls;
        security = AcquireCredentialsHandleW(NULL, UNISP_NAME_W, SECPKG_CRED_OUTBOUND, NULL, &credentials, NULL, NULL,
            &connection->credential, &expiry);
        if (security != SEC_E_OK)
        {
            response->native_error = (uint)security;
            return HTTPCLIENT_TLS;
        }
        connection->has_credential = 1;
    }
    error = _httpclient_internal__connect(connection, url->host, url->port,
        request->timeout_ms == 0 ? 15000u : request->timeout_ms);
    if (error == HTTPCLIENT_OK && connection->is_https)
    {
        error = _httpclient_internal__handshake(connection, 1);
    }
    return error;
}

httpclient_error_t httpclient_transport__write(httpclient_connection_t* connection, const void* data, uint size)
{
    if (connection->is_https)
    {
        return _httpclient_internal__encrypt(connection, data, size);
    }
    return _httpclient_internal__send(connection, data, size);
}

httpclient_error_t httpclient_transport__read(httpclient_connection_t* connection, http_parser_t* parser, boole* closed)
{
    if (connection->is_https)
    {
        return _httpclient_internal__read(connection, parser, closed);
    }
    httpclient_error_t error = _httpclient_internal__receive(connection, closed);
    if (error == HTTPCLIENT_OK && connection->used != 0)
    {
        error = _httpclient_internal__append(&parser->pending, connection->encrypted, connection->used,
            HTTP_HEADER_LIMIT + TLS_BUFFER_LIMIT);
        connection->used = 0;
        if (error == HTTPCLIENT_OK)
        {
            error = _httpclient_internal__parse(parser, connection->response);
        }
    }
    return error;
}
