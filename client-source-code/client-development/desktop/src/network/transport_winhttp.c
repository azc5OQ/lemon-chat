#include "platform/memory.h"
// The connection worker owns asynchronous WinHTTP operations and every handle lifetime.
#include <windows.h>
#include <winhttp.h>
#include <stdlib.h>
#include "network/transport.h"
#include "network/endpoint.h"
#include "network/protocol.h"
#include "platform/system.h"
#include "clib.h"

typedef struct transport_operation_t
{
    HANDLE ready;
    HANDLE written;
    HANDLE closed;
    volatile LONG error;
    WINHTTP_WEB_SOCKET_STATUS received;
    boole registered;
} transport_operation_t;

struct desktop_transport_t
{
    CRITICAL_SECTION lock;
    HANDLE worker;
    HANDLE stop;
    HANDLE queued;
    desktop_endpoint_t endpoint;
    desktop_transport_status_t status;
    desktop_message_queue_t incoming;
    desktop_message_queue_t outgoing;
};

// declarations
static void _transport_internal__status(desktop_transport_t* transport, desktop_transport_state_t state, const char* detail);
static void CALLBACK _transport_internal__callback(HINTERNET handle, DWORD_PTR context, DWORD status, void* information, DWORD size);
static boole _transport_internal__operation(transport_operation_t* operation);
static boole _transport_internal__register(HINTERNET handle, transport_operation_t* operation);
static void _transport_internal__close(HINTERNET handle, transport_operation_t* operation);
static void _transport_internal__dispose(transport_operation_t* operation);
static DWORD _transport_internal__wait(desktop_transport_t* transport, transport_operation_t* operation);
static DWORD WINAPI _transport_internal__worker(void* context);
static boole _transport_internal__send(desktop_transport_t* transport, const char* bytes, uint64 size, boole binary, uint64 expires);

/**
 * @brief updates the connection status under its short-lived data lock.
 * @param desktop_transport_t* transport -> owned transport
 * @param desktop_transport_state_t state -> new connection state
 * @param const char* detail -> bounded display text
 * @return void
 */
static void _transport_internal__status(desktop_transport_t* transport, desktop_transport_state_t state, const char* detail)
{
    EnterCriticalSection(&transport->lock);
    transport->status.state = state;
    clib__string_copy(transport->status.detail, sizeof(transport->status.detail), detail);
    LeaveCriticalSection(&transport->lock);
}

/**
 * @brief records completion without invoking networking or application code.
 * @param HINTERNET handle -> borrowed native callback handle
 * @param DWORD_PTR context -> operation retained until HANDLE_CLOSING
 * @param DWORD status -> native completion status
 * @param void* information -> borrowed native result
 * @param DWORD size -> native result size
 * @return void
 */
static void CALLBACK _transport_internal__callback(HINTERNET handle, DWORD_PTR context, DWORD status, void* information, DWORD size)
{
    transport_operation_t* operation = (transport_operation_t*)context;
    WINHTTP_ASYNC_RESULT* result = information;
    (void)handle;
    if (!operation)
    {
        return;
    }
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
    {
        // Last callback: after this signal nothing touches operation again.
        SetEvent(operation->closed);
    }
    else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
    {
        InterlockedCompareExchange(&operation->error,
            result && size >= sizeof(*result) ? (LONG)result->dwError : ERROR_INVALID_DATA, 0);
        SetEvent(operation->ready);
        SetEvent(operation->written);
    }
    else if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE)
    {
        if (information && size == sizeof(operation->received))
        {
            operation->received = *(WINHTTP_WEB_SOCKET_STATUS*)information;
        }
        else
        {
            InterlockedCompareExchange(&operation->error, ERROR_INVALID_DATA, 0);
        }
        SetEvent(operation->ready);
    }
    else if (status == WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE)
    {
        SetEvent(operation->written);
    }
    else if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE || status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE)
    {
        SetEvent(operation->ready);
    }
}

/**
 * @brief creates completion events before a callback can reference their owner.
 * @param transport_operation_t* operation -> cleared worker-local operation
 * @return boole -> TRUE when every event exists
 */
static boole _transport_internal__operation(transport_operation_t* operation)
{
    operation->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    operation->written = CreateEventW(NULL, TRUE, FALSE, NULL);
    operation->closed = CreateEventW(NULL, TRUE, FALSE, NULL);
    return operation->ready && operation->written && operation->closed;
}

/**
 * @brief installs a handle-specific context before asynchronous operations start.
 * @param HINTERNET handle -> worker-owned request
 * @param transport_operation_t* operation -> initialized completion state
 * @return boole -> TRUE when the context and callback are registered
 */
static boole _transport_internal__register(HINTERNET handle, transport_operation_t* operation)
{
    DWORD_PTR context = (DWORD_PTR)operation;
    if (!WinHttpSetOption(handle, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)))
    {
        return FALSE;
    }
    operation->registered = WinHttpSetStatusCallback(handle, _transport_internal__callback,
        WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) != WINHTTP_INVALID_STATUS_CALLBACK;
    return operation->registered;
}

/**
 * @brief cancels asynchronous operations and retains their buffers until the final callback.
 * @param HINTERNET handle -> handle closed exactly once by this worker
 * @param transport_operation_t* operation -> operation kept alive through cancellation
 * @return void
 */
static void _transport_internal__close(HINTERNET handle, transport_operation_t* operation)
{
    if (handle)
    {
        WinHttpCloseHandle(handle);
        if (operation->registered)
        {
            WaitForSingleObject(operation->closed, INFINITE);
        }
    }
}

/**
 * @brief frees completion events after the handle's final callback.
 * @param transport_operation_t* operation -> closed worker-local operation
 * @return void
 */
static void _transport_internal__dispose(transport_operation_t* operation)
{
    if (operation->ready)
    {
        CloseHandle(operation->ready);
    }
    if (operation->written)
    {
        CloseHandle(operation->written);
    }
    if (operation->closed)
    {
        CloseHandle(operation->closed);
    }
}

/**
 * @brief waits for handshake completion, cancellation or a bounded timeout on the worker.
 * @param desktop_transport_t* transport -> owned transport
 * @param transport_operation_t* operation -> pending handshake operation
 * @return DWORD -> native error, or ERROR_SUCCESS
 */
static DWORD _transport_internal__wait(desktop_transport_t* transport, transport_operation_t* operation)
{
    HANDLE events[2] = {transport->stop, operation->ready};
    DWORD result = WaitForMultipleObjects(2, events, FALSE, 5000);
    if (result == WAIT_OBJECT_0)
    {
        return ERROR_OPERATION_ABORTED;
    }
    if (result != WAIT_OBJECT_0 + 1)
    {
        return result == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
    }
    ResetEvent(operation->ready);
    return (DWORD)InterlockedCompareExchange(&operation->error, 0, 0);
}

/**
 * @brief owns asynchronous TCP WebSocket I/O outside the session and GUI threads.
 * @param void* context -> transport retained until this worker exits
 * @return DWORD -> zero after every handle and buffer has been released
 */
static DWORD WINAPI _transport_internal__worker(void* context)
{
    desktop_transport_t* transport = context;
    transport_operation_t request_io = {0};
    transport_operation_t socket_io = {0};
    HINTERNET session = NULL;
    HINTERNET connection = NULL;
    HINTERNET request = NULL;
    HINTERNET socket = NULL;
    wchar_t host[256] = {0};
    wchar_t path[2048] = {0};
    char detail[128] = {0};
    char message_type[64] = {0};
    char* message = NULL;
    desktop_message_t outgoing = {0};
    HANDLE events[4] = {0};
    DWORD error = 0;
    DWORD result = 0;
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    DWORD wait = 0;
    uint64 used = 0;
    uint64 send_deadline = 0;
    boole binary = FALSE;
    boole reading = FALSE;
    boole writing = FALSE;
    boole loopback = FALSE;
    boole envelope = FALSE;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type = 0;

    if (!_transport_internal__operation(&request_io) || !_transport_internal__operation(&socket_io))
    {
        error = ERROR_NOT_ENOUGH_MEMORY;
        goto label_finished;
    }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, transport->endpoint.host, -1, host, 256)
        || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, transport->endpoint.path, -1, path, 2048))
    {
        error = ERROR_INVALID_PARAMETER;
        goto label_finished;
    }
    loopback = clib__strcmp(transport->endpoint.host, "127.0.0.1") == 0
        || clib__strcmp(transport->endpoint.host, "::1") == 0 || clib__strcmp(transport->endpoint.host, "localhost") == 0;
    session = WinHttpOpen(L"LemonChatDesktop/0.2", loopback ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        NULL, NULL, WINHTTP_FLAG_ASYNC);
    if (!session)
    {
        error = GetLastError();
        goto label_finished;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
    connection = WinHttpConnect(session, host, transport->endpoint.port, 0);
    if (connection)
    {
        request = WinHttpOpenRequest(connection, L"GET", path, NULL, NULL, NULL, transport->endpoint.secure ? WINHTTP_FLAG_SECURE : 0);
    }
    if (!request || !_transport_internal__register(request, &request_io)
        || !WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect))
        || !WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0))
    {
        error = GetLastError();
        goto label_finished;
    }
    if (!WinHttpSendRequest(request, NULL, 0, NULL, 0, 0, (DWORD_PTR)&request_io))
    {
        error = GetLastError();
        if (error != ERROR_IO_PENDING)
        {
            goto label_finished;
        }
    }
    error = _transport_internal__wait(transport, &request_io);
    if (error)
    {
        goto label_finished;
    }
    if (!WinHttpReceiveResponse(request, NULL))
    {
        error = GetLastError();
        if (error != ERROR_IO_PENDING)
        {
            goto label_finished;
        }
    }
    error = _transport_internal__wait(transport, &request_io);
    if (error)
    {
        goto label_finished;
    }
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &status_size, NULL)
        || status != 101)
    {
        error = ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
        goto label_finished;
    }
    socket = WinHttpWebSocketCompleteUpgrade(request, (DWORD_PTR)&socket_io);
    if (!socket)
    {
        error = GetLastError();
        goto label_finished;
    }
    // The socket inherits the callback but has its own context and completion events.
    socket_io.registered = TRUE;
    _transport_internal__close(request, &request_io);
    request = NULL;
    message = calloc(1, DESKTOP_MESSAGE_LIMIT + 1);
    if (!message)
    {
        error = ERROR_NOT_ENOUGH_MEMORY;
        goto label_finished;
    }
    events[0] = transport->stop;
    events[1] = socket_io.ready;
    events[2] = socket_io.written;
    events[3] = transport->queued;
    _transport_internal__status(transport, DESKTOP_TRANSPORT_CONNECTED, "WebSocket connected");
    while (WaitForSingleObject(transport->stop, 0) != WAIT_OBJECT_0)
    {
        if (!reading)
        {
            ResetEvent(socket_io.ready);
            reading = TRUE;
            error = WinHttpWebSocketReceive(socket, message + used, (DWORD)(DESKTOP_MESSAGE_LIMIT + 1 - used), NULL, NULL);
            if (error)
            {
                break;
            }
        }
        if (!writing)
        {
            EnterCriticalSection(&transport->lock);
            ResetEvent(transport->queued);
            desktop_message_queue__expire(&transport->outgoing, desktop_system__milliseconds());
            writing = desktop_message_queue__pop(&transport->outgoing, &outgoing);
            LeaveCriticalSection(&transport->lock);
            if (writing)
            {
                ResetEvent(socket_io.written);
                send_deadline = desktop_system__milliseconds() + 5000;
                error = WinHttpWebSocketSend(socket,
                    outgoing.binary ? WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE : WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                    outgoing.size ? outgoing.data : NULL, (DWORD)outgoing.size);
                if (error)
                {
                    break;
                }
            }
        }
        // Queue signals matter only when a new send can start.
        wait = writing ? 3 : 4;
        result = WaitForMultipleObjects(wait, events, FALSE, 50);
        error = (DWORD)InterlockedCompareExchange(&socket_io.error, 0, 0);
        if (error || result == WAIT_OBJECT_0)
        {
            break;
        }
        if (result == WAIT_FAILED)
        {
            error = GetLastError();
            break;
        }
        if (writing && WaitForSingleObject(socket_io.written, 0) == WAIT_OBJECT_0)
        {
            ResetEvent(socket_io.written);
            writing = FALSE;
            desktop_message__release(&outgoing);
        }
        if (writing && desktop_system__milliseconds() >= send_deadline)
        {
            error = ERROR_TIMEOUT;
            break;
        }
        if (reading && WaitForSingleObject(socket_io.ready, 0) == WAIT_OBJECT_0)
        {
            reading = FALSE;
            ResetEvent(socket_io.ready);
            type = socket_io.received.eBufferType;
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            {
                break;
            }
            if (socket_io.received.dwBytesTransferred > DESKTOP_MESSAGE_LIMIT - used)
            {
                error = ERROR_INSUFFICIENT_BUFFER;
                break;
            }
            used += socket_io.received.dwBytesTransferred;
            binary |= type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
            {
                clib__null_memory(message_type, sizeof(message_type));
                envelope = !binary && desktop_protocol__message_type(message, used, message_type, sizeof(message_type));
                EnterCriticalSection(&transport->lock);
                transport->status.messages_received++;
                if (!desktop_message_queue__push(&transport->incoming, message, used, binary))
                {
                    error = ERROR_NOT_ENOUGH_MEMORY;
                }
                clib__string_copy(transport->status.last_type, sizeof(transport->status.last_type),
                    envelope ? message_type : binary ? "binary" : "unrecognized envelope");
                LeaveCriticalSection(&transport->lock);
                clib__null_memory(message, used);
                used = 0;
                binary = FALSE;
                if (error)
                {
                    break;
                }
            }
        }
    }
label_finished:
    _transport_internal__close(socket, &socket_io);
    _transport_internal__close(request, &request_io);
    if (connection)
    {
        WinHttpCloseHandle(connection);
    }
    if (session)
    {
        WinHttpCloseHandle(session);
    }
    // Closing callbacks have finished; no I/O can still reference these buffers.
    desktop_message__release(&outgoing);
    if (message)
    {
        clib__null_memory(message, DESKTOP_MESSAGE_LIMIT + 1);
        desktop_memory__erase(message, DESKTOP_MESSAGE_LIMIT + 1);
        free(message);
    }
    _transport_internal__dispose(&socket_io);
    _transport_internal__dispose(&request_io);
    if (WaitForSingleObject(transport->stop, 0) == WAIT_OBJECT_0 || !error)
    {
        _transport_internal__status(transport, DESKTOP_TRANSPORT_DISCONNECTED, "Disconnected");
    }
    else
    {
        clib__string_format(detail, sizeof(detail), "Connection failed (Windows error %lu).", (unsigned long)error);
        _transport_internal__status(transport, DESKTOP_TRANSPORT_FAILED, detail);
    }
    EnterCriticalSection(&transport->lock);
    desktop_message_queue__clear(&transport->outgoing);
    LeaveCriticalSection(&transport->lock);
    return 0;
}

/**
 * @brief allocates an idle transport with bounded queues and cancellation events.
 * @return desktop_transport_t* -> owned transport, or NULL
 */
desktop_transport_t* desktop_transport__create(void)
{
    desktop_transport_t* transport = calloc(1, sizeof(*transport));
    if (!transport)
    {
        return NULL;
    }
    InitializeCriticalSection(&transport->lock);
    transport->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    transport->queued = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!transport->stop || !transport->queued)
    {
        desktop_transport__destroy(transport);
        return NULL;
    }
    _transport_internal__status(transport, DESKTOP_TRANSPORT_DISCONNECTED, "Ready to connect");
    return transport;
}

/**
 * @brief signals cancellation without executing WinHTTP or waiting on the caller's thread.
 * @param desktop_transport_t* transport -> owned transport, or NULL
 * @return void
 */
void desktop_transport__disconnect(desktop_transport_t* transport)
{
    if (transport && transport->stop)
    {
        SetEvent(transport->stop);
    }
}

/**
 * @brief joins the worker before releasing callback-visible queues and state.
 * @param desktop_transport_t* transport -> owned transport, or NULL
 * @return void
 */
void desktop_transport__destroy(desktop_transport_t* transport)
{
    if (!transport)
    {
        return;
    }
    desktop_transport__disconnect(transport);
    if (transport->worker)
    {
        WaitForSingleObject(transport->worker, INFINITE);
        CloseHandle(transport->worker);
    }
    if (transport->stop)
    {
        CloseHandle(transport->stop);
    }
    if (transport->queued)
    {
        CloseHandle(transport->queued);
    }
    desktop_message_queue__clear(&transport->incoming);
    desktop_message_queue__clear(&transport->outgoing);
    DeleteCriticalSection(&transport->lock);
    desktop_memory__erase(transport, sizeof(*transport));
    free(transport);
}

/**
 * @brief starts a new connection after the previous worker releases its handles.
 * @param desktop_transport_t* transport -> idle owned transport
 * @param const char* url -> TCP WebSocket endpoint
 * @return boole -> TRUE when setup started
 */
boole desktop_transport__connect(desktop_transport_t* transport, const char* url)
{
    if (!transport)
    {
        return FALSE;
    }
    if (transport->worker)
    {
        if (WaitForSingleObject(transport->worker, 0) != WAIT_OBJECT_0)
        {
            return FALSE;
        }
        CloseHandle(transport->worker);
        transport->worker = NULL;
    }
    if (!desktop_endpoint__parse(url, &transport->endpoint))
    {
        _transport_internal__status(transport, DESKTOP_TRANSPORT_FAILED, "Enter a valid ws:// or wss:// address.");
        return FALSE;
    }
    ResetEvent(transport->stop);
    ResetEvent(transport->queued);
    EnterCriticalSection(&transport->lock);
    desktop_message_queue__clear(&transport->incoming);
    desktop_message_queue__clear(&transport->outgoing);
    clib__null_memory(&transport->status, sizeof(transport->status));
    LeaveCriticalSection(&transport->lock);
    _transport_internal__status(transport, DESKTOP_TRANSPORT_CONNECTING, "Connecting...");
    transport->worker = CreateThread(NULL, 0, _transport_internal__worker, transport, 0, NULL);
    if (!transport->worker)
    {
        _transport_internal__status(transport, DESKTOP_TRANSPORT_FAILED, "Could not start the connection worker.");
    }
    return transport->worker != NULL;
}

/**
 * @brief copies the connection status under its data lock.
 * @param desktop_transport_t* transport -> owned transport
 * @param desktop_transport_status_t* status -> receives a value copy
 * @return void
 */
void desktop_transport__status(desktop_transport_t* transport, desktop_transport_status_t* status)
{
    EnterCriticalSection(&transport->lock);
    *status = transport->status;
    status->finished = !transport->worker || WaitForSingleObject(transport->worker, 0) == WAIT_OBJECT_0;
    LeaveCriticalSection(&transport->lock);
}

/**
 * @brief copies a message while preserving reliable order and rejecting expired voice.
 * @param desktop_transport_t* transport -> connected transport
 * @param const char* bytes -> readable bytes, or NULL for an empty binary message
 * @param uint64 size -> bounded byte count
 * @param boole binary -> WebSocket message kind
 * @param uint64 expires -> zero for reliable data, otherwise a monotonic deadline
 * @return boole -> TRUE when the queue owns a copy
 */
static boole _transport_internal__send(desktop_transport_t* transport, const char* bytes, uint64 size, boole binary, uint64 expires)
{
    boole accepted = FALSE;
    uint64 now = desktop_system__milliseconds();
    if (!transport || (!bytes && size) || (!binary && !size) || size > DESKTOP_MESSAGE_LIMIT || (expires && now >= expires))
    {
        return FALSE;
    }
    EnterCriticalSection(&transport->lock);
    if (transport->status.state == DESKTOP_TRANSPORT_CONNECTED && WaitForSingleObject(transport->stop, 0) != WAIT_OBJECT_0)
    {
        desktop_message_queue__expire(&transport->outgoing, now);
        accepted = desktop_message_queue__push_until(&transport->outgoing, bytes ? bytes : "", size, binary, expires);
        if (accepted)
        {
            SetEvent(transport->queued);
        }
    }
    LeaveCriticalSection(&transport->lock);
    return accepted;
}

/**
 * @brief queues reliable UTF-8 metadata without performing network I/O.
 * @param desktop_transport_t* transport -> connected transport
 * @param const char* bytes -> complete text message
 * @param uint64 size -> byte count
 * @return boole -> TRUE when queued
 */
boole desktop_transport__send(desktop_transport_t* transport, const char* bytes, uint64 size)
{
    return _transport_internal__send(transport, bytes, size, FALSE, 0);
}

/**
 * @brief queues reliable binary data, including empty audio keepalives.
 * @param desktop_transport_t* transport -> connected transport
 * @param const unsigned char* bytes -> complete message, or NULL when empty
 * @param uint64 size -> byte count
 * @return boole -> TRUE when queued
 */
boole desktop_transport__send_binary(desktop_transport_t* transport, const unsigned char* bytes, uint64 size)
{
    return _transport_internal__send(transport, (const char*)bytes, size, TRUE, 0);
}

/**
 * @brief queues voice that expires before it can accumulate a long sending delay.
 * @param desktop_transport_t* transport -> connected audio transport
 * @param const unsigned char* bytes -> encrypted voice frame
 * @param uint64 size -> frame size
 * @param uint64 expires -> nonzero monotonic expiry
 * @return boole -> TRUE when queued
 */
boole desktop_transport__send_media(desktop_transport_t* transport, const unsigned char* bytes, uint64 size, uint64 expires)
{
    return expires && _transport_internal__send(transport, (const char*)bytes, size, TRUE, expires);
}

/**
 * @brief removes unsent voice when push-to-talk stops, retaining reliable attachment data.
 * @param desktop_transport_t* transport -> owned audio transport
 * @return void
 */
void desktop_transport__discard_media(desktop_transport_t* transport)
{
    EnterCriticalSection(&transport->lock);
    desktop_message_queue__expire(&transport->outgoing, 0);
    LeaveCriticalSection(&transport->lock);
}

/**
 * @brief transfers the next received message to its caller.
 * @param desktop_transport_t* transport -> owned transport
 * @param desktop_message_t* message -> cleared output, released by caller
 * @return boole -> TRUE when available
 */
boole desktop_transport__receive(desktop_transport_t* transport, desktop_message_t* message)
{
    boole received = FALSE;
    EnterCriticalSection(&transport->lock);
    received = desktop_message_queue__pop(&transport->incoming, message);
    LeaveCriticalSection(&transport->lock);
    return received;
}
