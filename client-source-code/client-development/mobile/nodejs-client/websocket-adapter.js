// use the same JavaScript socket implementation in Android and desktop tests

process.env.WS_NO_BUFFER_UTIL = "1";
process.env.WS_NO_UTF_8_VALIDATE = "1";

const http = require("http");
const EventEmitter = require("events");
const WebSocket = require("./vendor/ws");
const limits = require("./transport-limits.js");
const ErrorEvent = require("./vendor/ws/lib/event-target.js").ErrorEvent;

// expose the socket error code expected by shared worker handlers
Object.defineProperty(ErrorEvent.prototype, "code", {
    configurable: true,
    get: function()
    {
        return this.error && this.error.code;
    }
});

/**
 * @brief measures the number of bytes in a socket payload
 *
 * @param data payload to measure
 *
 * @return UTF8 or binary payload length
 */
function data_bytes(data)
{
    if (typeof data === "string")
    {
        return Buffer.byteLength(data, "utf8");
    }
    if (Buffer.isBuffer(data) || ArrayBuffer.isView(data))
    {
        return data.byteLength;
    }
    if (data instanceof ArrayBuffer)
    {
        return data.byteLength;
    }
    return Buffer.byteLength(String(data), "utf8");
}

/**
 * @brief sends a payload within the message and socket queue limits
 *
 * @param socket destination socket
 * @param data payload to send
 * @param options socket send options
 * @param callback completion callback when provided
 *
 * @return undefined after handing the payload to the socket
 */
function send_bounded(socket, data, options, callback)
{
    const bytes = data_bytes(data);
    if (bytes > limits.messageBytes || socket.bufferedAmount + bytes > limits.websocketQueueBytes)
    {
        socket.terminate();
        throw new RangeError("WebSocket output capacity exceeded");
    }
    return WebSocket.prototype.send.call(socket, data, options, callback);
}

class WebSocketClient extends WebSocket
{
    /**
     * @brief starts a client connection with the shared transport limits
     *
     * @param string|URL url -> remote WebSocket address
     * @param string|string[]|undefined protocols -> optional subprotocol names
     *
     * @return WebSocketClient -> client whose connection opens asynchronously
     */
    constructor(url, protocols)
    {
        super(url, protocols, {
            perMessageDeflate: false,
            maxPayload: limits.messageBytes,
            maxHeaderSize: limits.headerBytes,
            handshakeTimeout: limits.handshakeTimeoutMs,
            maxPendingAutoPongs: 16,
            followRedirects: false
        });
    }

    /**
     * @brief sends a payload after checking its size and the queued bytes
     *
     * @param string|Buffer|ArrayBuffer|ArrayBufferView|number data -> text or binary payload
     * @param object|function|undefined options -> send options or the completion callback
     * @param function|undefined callback -> optional callback for write completion or failure
     *
     * @return undefined -> completion is reported through the callback
     *
     * @throws RangeError -> output limit exceeded and the socket was terminated
     * @throws Error -> the connection is still opening
     */
    send(data, options, callback)
    {
        return send_bounded(this, data, options, callback);
    }
}

class WebSocketServer extends EventEmitter
{
    /**
     * @brief starts an HTTP listener with bounded unauthenticated WebSocket connections
     *
     * @param object options -> listening port and host
     *
     * @return WebSocketServer -> server that emits listening and connection events
     */
    constructor(options)
    {
        super();
        this.sockets = new Set();
        this.http_server = http.createServer({ maxHeaderSize: limits.headerBytes }, function(request, response)
        {
            response.writeHead(404, { Connection: "close" });
            response.end();
        });
        this.http_server.maxConnections = limits.pendingConnections;
        this.http_server.headersTimeout = limits.handshakeTimeoutMs;
        this.http_server.requestTimeout = limits.handshakeTimeoutMs;
        this.http_server.maxHeadersCount = 32;
        this.websocket_server = new WebSocket.WebSocketServer({
            noServer: true,
            perMessageDeflate: false,
            maxPayload: limits.tokenFrameBytes,
            maxPendingAutoPongs: 16,
            clientTracking: true
        });

        this.http_server.on("connection", (socket) =>
        {
            this.sockets.add(socket);
            const deadline = setTimeout(() =>
            {
                socket.destroy();
            }, limits.handshakeTimeoutMs);
            socket.bridge_auth_deadline = deadline;
            socket.on("close", () =>
            {
                clearTimeout(deadline);
                this.sockets.delete(socket);
            });
        });
        this.http_server.on("upgrade", (request, socket, head) =>
        {
            if (request.url !== "/")
            {
                socket.destroy();
                return;
            }
            this.websocket_server.handleUpgrade(request, socket, head, (websocket) =>
            {
                websocket.send = function(data, options, callback)
                {
                    return send_bounded(this, data, options, callback);
                };
                this.emit("connection", websocket, request);
            });
        });
        this.http_server.on("clientError", (error, socket) =>
        {
            socket.destroy();
        });
        this.http_server.on("error", (error) =>
        {
            this.emit("error", error);
        });
        this.http_server.on("listening", () =>
        {
            this.emit("listening");
        });
        this.http_server.listen(options.port, options.host);
    }

    /**
     * @brief raises the receive limit and clears the deadline after the caller verifies the token
     *
     * @param WebSocket socket -> connected socket whose token has been accepted
     *
     * @return undefined
     */
    authenticate(socket)
    {
        // raise the receive limit only after token authentication

        socket._receiver._maxPayload = limits.messageBytes;
        clearTimeout(socket._socket.bridge_auth_deadline);
    }

    /**
     * @brief reads the underlying HTTP listener address
     *
     * @return object|string|null -> bound address or pipe name, or null when not listening
     */
    address()
    {
        return this.http_server.address();
    }

    /**
     * @brief destroys accepted sockets and closes the WebSocket and HTTP servers
     *
     * @param function|undefined callback -> optional callback when HTTP shutdown completes or fails
     *
     * @return undefined
     */
    close(callback)
    {
        for (const socket of this.sockets)
        {
            socket.destroy();
        }
        this.websocket_server.close();
        this.http_server.close(callback);
    }
}

module.exports = { WebSocketClient, WebSocketServer, limits };
