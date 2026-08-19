// Minimal RFC6455 websocket, client + server, zero dependencies (node's net + crypto only).
// Replaces the npm `ws` package so the project rebuilds offline without npm.
// No extensions, no permessage-deflate: text/binary frames, ping/pong, close. That is all the
// lemon-chat protocol uses, on both the loopback hop and the real server connection.

let net = require("net");
let crypto = require("crypto");

let WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

function accept_key_for(websocket_key)
{
    return crypto.createHash("sha1").update(websocket_key + WS_GUID).digest("base64");
}

// builds one frame. server-to-client is unmasked, client-to-server is masked (the rfc requires it)
function encode_frame(payload_buffer, opcode, use_mask)
{
    let length = payload_buffer.length;
    let header;

    if (length < 126)
    {
        header = Buffer.alloc(2);
        header[1] = length;
    }
    else if (length < 65536)
    {
        header = Buffer.alloc(4);
        header[1] = 126;
        header.writeUInt16BE(length, 2);
    }
    else
    {
        header = Buffer.alloc(10);
        header[1] = 127;
        header.writeUInt32BE(Math.floor(length / 4294967296), 2);
        header.writeUInt32BE(length >>> 0, 6);
    }

    header[0] = 0x80 | opcode; // fin + opcode

    if (use_mask !== true)
    {
        return Buffer.concat([header, payload_buffer]);
    }

    header[1] |= 0x80;
    let mask = crypto.randomBytes(4);
    let masked = Buffer.alloc(length);

    for (let i = 0; i < length; i++)
    {
        masked[i] = payload_buffer[i] ^ mask[i % 4];
    }

    return Buffer.concat([header, mask, masked]);
}

// incremental frame parser; calls on_frame(opcode, payload) for each complete frame
function make_frame_parser(on_frame)
{
    let buffer = Buffer.alloc(0);
    let fragments = [];
    let fragment_opcode = 0;

    return function feed(chunk)
    {
        buffer = Buffer.concat([buffer, chunk]);

        while (true)
        {
            if (buffer.length < 2) { return; }

            let fin = (buffer[0] & 0x80) !== 0;
            let opcode = buffer[0] & 0x0f;
            let masked = (buffer[1] & 0x80) !== 0;
            let length = buffer[1] & 0x7f;
            let offset = 2;

            if (length === 126)
            {
                if (buffer.length < 4) { return; }
                length = buffer.readUInt16BE(2);
                offset = 4;
            }
            else if (length === 127)
            {
                if (buffer.length < 10) { return; }
                length = buffer.readUInt32BE(2) * 4294967296 + buffer.readUInt32BE(6);
                offset = 10;
            }

            let mask = null;

            if (masked)
            {
                if (buffer.length < offset + 4) { return; }
                mask = buffer.slice(offset, offset + 4);
                offset += 4;
            }

            if (buffer.length < offset + length) { return; }

            let payload = buffer.slice(offset, offset + length);
            buffer = buffer.slice(offset + length);

            if (mask != null)
            {
                let unmasked = Buffer.alloc(payload.length);
                for (let i = 0; i < payload.length; i++) { unmasked[i] = payload[i] ^ mask[i % 4]; }
                payload = unmasked;
            }

            // fragmentation: opcode 0 continues the previous data frame
            if (opcode === 0)
            {
                fragments.push(payload);
                if (fin) { on_frame(fragment_opcode, Buffer.concat(fragments)); fragments = []; }
            }
            else if (opcode === 1 || opcode === 2)
            {
                if (fin) { on_frame(opcode, payload); }
                else { fragment_opcode = opcode; fragments = [payload]; }
            }
            else
            {
                on_frame(opcode, payload); // control frame, always fin
            }
        }
    };
}

// wraps a net socket as a websocket endpoint with the small event api both sides use
function make_endpoint(socket, send_masked)
{
    let endpoint = {
        readyState: 1,
        listeners: { message: [], close: [], error: [], open: [] },

        on: function(name, callback) { if (this.listeners[name]) { this.listeners[name].push(callback); } },
        addEventListener: function(name, callback) { this.on(name, callback); },

        emit: function(name, argument)
        {
            this.listeners[name].forEach(function(cb) { try { cb(argument); } catch (e) { console.error("ws listener: " + e); } });
            let direct = endpoint["on" + name];
            if (typeof direct === "function") { try { direct(argument); } catch (e) { console.error("ws on" + name + ": " + e); } }
        },

        send: function(data)
        {
            if (this.readyState !== 1) { return; }
            let payload = Buffer.isBuffer(data) ? data : Buffer.from(String(data), "utf8");
            socket.write(encode_frame(payload, Buffer.isBuffer(data) ? 2 : 1, send_masked));
        },

        close: function()
        {
            if (this.readyState === 1)
            {
                this.readyState = 2;
                try { socket.write(encode_frame(Buffer.alloc(0), 8, send_masked)); } catch (e) { }
            }
            socket.end();
        }
    };

    let parser = make_frame_parser(function(opcode, payload)
    {
        if (opcode === 1 || opcode === 2)
        {
            // handed over in both shapes: .data for the browser-like client api, raw for server use
            endpoint.emit("message", { data: payload.toString("utf8"), toString: function() { return payload.toString("utf8"); } });
        }
        else if (opcode === 8)
        {
            endpoint.close();
        }
        else if (opcode === 9)
        {
            socket.write(encode_frame(payload, 10, send_masked)); // pong
        }
    });

    socket.on("data", function(chunk) { if (endpoint.readyState === 1 || endpoint.readyState === 2) { parser(chunk); } });
    socket.on("close", function() { endpoint.readyState = 3; endpoint.emit("close", {}); });
    socket.on("error", function(error) { endpoint.emit("error", error); });

    return endpoint;
}

// ---- server ----

function WebSocketServer(options)
{
    let self = this;
    this.listeners = { connection: [], listening: [] };

    this.on = function(name, callback) { if (self.listeners[name]) { self.listeners[name].push(callback); } };

    this.net_server = net.createServer(function(socket)
    {
        let request = "";

        function on_handshake_data(chunk)
        {
            request += chunk.toString("utf8");

            let header_end = request.indexOf("\r\n\r\n");
            if (header_end === -1) { return; }

            socket.removeListener("data", on_handshake_data);

            let key_match = request.match(/Sec-WebSocket-Key:\s*(\S+)/i);

            if (request.indexOf("Upgrade") === -1 || key_match == null)
            {
                socket.end("HTTP/1.1 400 Bad Request\r\n\r\n");
                return;
            }

            socket.write("HTTP/1.1 101 Switching Protocols\r\n"
                + "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                + "Sec-WebSocket-Accept: " + accept_key_for(key_match[1]) + "\r\n\r\n");

            let endpoint = make_endpoint(socket, false);
            self.listeners.connection.forEach(function(cb) { cb(endpoint); });

            // bytes that arrived glued to the handshake
            let leftover = request.substring(header_end + 4);
            if (leftover.length > 0) { socket.emit("data", Buffer.from(leftover, "binary")); }
        }

        socket.on("data", on_handshake_data);
        socket.on("error", function() { });
    });

    this.net_server.listen(options.port, options.host, function()
    {
        self.listeners.listening.forEach(function(cb) { cb(); });
    });

    this.address = function() { return self.net_server.address(); };
    this.close = function() { self.net_server.close(); };
}

// ---- client (browser WebSocket lookalike, enough for websocket_worker_onmessage) ----

function WebSocketClient(url)
{
    let self = this;
    let match = String(url).match(/^ws:\/\/([^:/]+):?(\d+)?(\/.*)?$/);

    if (match == null)
    {
        throw new Error("mini-ws client: unsupported url " + url);
    }

    let host = match[1];
    let port = match[2] ? parseInt(match[2], 10) : 80;
    let path = match[3] || "/";
    let websocket_key = crypto.randomBytes(16).toString("base64");

    this.readyState = 0;
    this.endpoint = null;

    let socket = net.connect({ host: host, port: port }, function()
    {
        socket.write("GET " + path + " HTTP/1.1\r\n"
            + "Host: " + host + ":" + port + "\r\n"
            + "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            + "Sec-WebSocket-Key: " + websocket_key + "\r\n"
            + "Sec-WebSocket-Version: 13\r\n\r\n");
    });

    let response = "";

    function on_handshake_data(chunk)
    {
        response += chunk.toString("utf8");

        let header_end = response.indexOf("\r\n\r\n");
        if (header_end === -1) { return; }

        socket.removeListener("data", on_handshake_data);

        let accept_match = response.match(/Sec-WebSocket-Accept:\s*(\S+)/i);

        if (response.indexOf("101") === -1 || accept_match == null || accept_match[1] !== accept_key_for(websocket_key))
        {
            self.readyState = 3;
            if (typeof self.onerror === "function") { self.onerror(new Error("handshake rejected")); }
            socket.destroy();
            return;
        }

        self.endpoint = make_endpoint(socket, true); // client frames are masked
        self.readyState = 1;

        // mirror the endpoint events onto the browser-style on* surface
        self.endpoint.onmessage = function(event) { self.message_listeners.forEach(function(cb) { cb(event); }); if (typeof self.onmessage === "function") { self.onmessage(event); } };
        self.endpoint.onclose = function() { self.readyState = 3; if (typeof self.onclose === "function") { self.onclose({}); } };
        self.endpoint.onerror = function(error) { if (typeof self.onerror === "function") { self.onerror(error); } };

        if (typeof self.onopen === "function") { self.onopen({}); }

        let leftover = response.substring(header_end + 4);
        if (leftover.length > 0) { socket.emit("data", Buffer.from(leftover, "binary")); }
    }

    socket.on("data", on_handshake_data);
    socket.on("error", function(error) { self.readyState = 3; if (typeof self.onerror === "function") { self.onerror(error); } });

    this.message_listeners = [];
    this.addEventListener = function(name, callback)
    {
        if (name === "message") { self.message_listeners.push(callback); }
        else { self["on" + name] = callback; }
    };
    this.on = this.addEventListener; // ws-package style alias the tests use

    this.send = function(data) { if (self.endpoint != null) { self.endpoint.send(data); } };
    this.close = function() { if (self.endpoint != null) { self.endpoint.close(); } else { socket.destroy(); self.readyState = 3; } };
}

module.exports = { WebSocketServer: WebSocketServer, WebSocketClient: WebSocketClient };
