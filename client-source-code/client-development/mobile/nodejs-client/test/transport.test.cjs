const test = require("node:test");
const assert = require("node:assert/strict");
const net = require("node:net");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const crypto = require("node:crypto");
const vm = require("node:vm");
const { once } = require("node:events");
const bridge = require("../android-bridge-client.js");
const adapter = require("../websocket-adapter.js");
const loopback = require("../loopback-ui-server.js");

function wait_for(check, timeout = 2000)
{
    return new Promise(function(resolve, reject)
    {
        const started = Date.now();
        const timer = setInterval(function()
        {
            if (check())
            {
                clearInterval(timer);
                resolve();
            }
            else if (Date.now() - started > timeout)
            {
                clearInterval(timer);
                reject(new Error("Condition timed out"));
            }
        }, 5);
    });
}

test("control framing preserves every UTF-8 split and enforces byte capacity", function()
{
    const expected = JSON.stringify({ type: "settings", json: "žltý 🍋 日本語" });
    const bytes = Buffer.from(expected + "\n");
    for (let split = 0; split <= bytes.length; split++)
    {
        const received = [];
        const feed = bridge.create_line_reader(function(line)
        {
            received.push(line);
        });
        feed(bytes.subarray(0, split));
        feed(bytes.subarray(split));
        assert.deepEqual(received, [expected]);
    }
    assert.throws(function()
    {
        bridge.create_line_reader(function()
        {
        })(Buffer.from([0xc3, 10]));
    }, TypeError);
    assert.throws(function()
    {
        bridge.create_line_reader(function()
        {
        })(Buffer.alloc(adapter.limits.controlLineBytes + 1, 65));
    }, RangeError);
    for (const input of [null, [], {}, { type: "network", available: "false" }, { type: "ui_visible", visible: 1 }])
    {
        assert.equal(bridge.valid_command(input), false);
    }
});

test("control errors do not discard following commands and reconnect replays state", async function(t)
{
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), "lemon-bridge-test-"));
    const config = path.join(directory, "bridge.json");
    const server = net.createServer();
    server.listen(0, "127.0.0.1");
    await once(server, "listening");
    fs.writeFileSync(config, JSON.stringify({ port: server.address().port, token: "a".repeat(32) }));
    const connections = [];
    const events = [];
    server.on("connection", function(socket)
    {
        connections.push(socket);
        socket.on("data", bridge.create_line_reader(function(line)
        {
            if (line.startsWith("{"))
            {
                events.push(JSON.parse(line));
            }
        }));
    });
    let connected = 0;
    const networks = [];
    let unread_listener;
    let status_listener;
    const bundle = {
        read_state: function()
        {
            return { g_is_authenticated: false, g_local_username: "", g_client_list: [] };
        },
        set_on_message_processed: function()
        {
        },
        set_unread_listener: function(callback)
        {
            unread_listener = callback;
        },
        set_connection_status_listener: function(callback)
        {
            status_listener = callback;
        },
        android_js_bridge: {
            accept_current_settings_from_android: function()
            {
                throw new Error("secret must not be logged");
            }
        },
        node_request_connect: function()
        {
            connected++;
            return true;
        },
        node_set_device_network: function(value)
        {
            networks.push(value);
        }
    };
    const client = bridge.start(bundle, null, { config_path: config, retry_ms: 10 });
    t.after(function()
    {
        client.stop();
        for (const socket of connections)
        {
            socket.destroy();
        }
        server.close();
        fs.unlinkSync(config);
        fs.rmdirSync(directory);
    });
    await wait_for(function()
    {
        return connections.length === 1 && events.length >= 3;
    });
    connections[0].write('null\n[]\n{"type":"settings","json":"{}"}\n{"type":"connect","command_id":"connect-one"}\n{"type":"network","available":true}\n');
    await wait_for(function()
    {
        return connected === 1 && networks.length === 1;
    });
    connections[0].destroy();
    unread_listener(7);
    status_listener({ state: "reconnecting", reason: "Waiting" });
    await wait_for(function()
    {
        return connections.length === 2 && events.some(function(event)
        {
            return event.type === "unread" && event.count === 7;
        }) && events.some(function(event)
        {
            return event.type === "status_text" && event.state === "reconnecting";
        });
    });
    assert.deepEqual(networks, [true]);
});

test("adapter preserves moderator close metadata and rejects CONNECTING sends", async function(t)
{
    const server = new adapter.WebSocketServer({ host: "127.0.0.1", port: 0 });
    t.after(function()
    {
        server.close();
    });
    await once(server, "listening");
    const codes = [4001, 4002];
    server.on("connection", function(socket)
    {
        server.authenticate(socket);
        socket.close(codes.shift(), "Moderator action");
    });
    for (const code of [4001, 4002])
    {
        const client = new adapter.WebSocketClient("ws://127.0.0.1:" + server.address().port + "/");
        assert.throws(function()
        {
            client.send("too early");
        }, /CONNECTING/);
        let closes = 0;
        const event = await new Promise(function(resolve)
        {
            client.onclose = function(value)
            {
                closes++;
                resolve(value);
            };
        });
        assert.equal(event.code, code);
        assert.equal(event.reason, "Moderator action");
        assert.equal(event.wasClean, true);
        assert.equal(closes, 1);
    }
});

test("upgrade leftovers remain binary safe and clean EOF before upgrade emits one close", async function(t)
{
    let mode = "binary";
    const sockets = [];
    const raw = net.createServer(function(socket)
    {
        sockets.push(socket);
        socket.once("data", function(data)
        {
            if (mode === "eof")
            {
                socket.end();
                return;
            }
            const key = /Sec-WebSocket-Key: (.+)\r\n/i.exec(data.toString())[1];
            const accept = crypto.createHash("sha1").update(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest("base64");
            const upgrade = Buffer.from("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n");
            socket.write(Buffer.concat([upgrade, Buffer.from([0x82, 4, 0, 0x80, 0xff, 0xc3])]));
        });
    });
    raw.listen(0, "127.0.0.1");
    t.after(function()
    {
        for (const socket of sockets)
        {
            socket.destroy();
        }
        raw.close();
    });
    await once(raw, "listening");
    const client = new adapter.WebSocketClient("ws://127.0.0.1:" + raw.address().port);
    const message = await new Promise(function(resolve)
    {
        client.onmessage = resolve;
    });
    assert.deepEqual(Buffer.from(message.data), Buffer.from([0, 0x80, 0xff, 0xc3]));
    client.terminate();
    mode = "eof";
    const early = new adapter.WebSocketClient("ws://127.0.0.1:" + raw.address().port);
    let errors = 0;
    early.onerror = function(event)
    {
        errors++;
        assert.ok(event.error);
    };
    const closed = await new Promise(function(resolve)
    {
        early.onclose = resolve;
    });
    assert.equal(closed.code, 1006);
    assert.equal(closed.wasClean, false);
    assert.equal(errors, 1);
});

test("pre-auth payload bound rejects oversized token before application dispatch", async function(t)
{
    const server = new adapter.WebSocketServer({ host: "127.0.0.1", port: 0 });
    t.after(function()
    {
        server.close();
    });
    await once(server, "listening");
    let messages = 0;
    server.on("connection", function(socket)
    {
        socket.on("error", function()
        {
        });
        socket.on("message", function()
        {
            messages++;
        });
    });
    const client = new adapter.WebSocketClient("ws://127.0.0.1:" + server.address().port);
    client.onopen = function()
    {
        client.send("a".repeat(129));
    };
    const event = await new Promise(function(resolve)
    {
        client.onclose = resolve;
    });
    assert.equal(event.code, 1009);
    assert.equal(messages, 0);
});

function fixture_bundle()
{
    const listeners = {};
    const state = {
        g_is_authenticated: true, g_is_client_list_retrieved: true, g_local_username: "Me",
        g_client_list: [], g_channel_list: [{ channel_id: 1, has_maintainer: true, maintainer_id: 3, maintainer_generation: 9 }],
        g_groups: [], g_icons: []
    };
    const session = { authenticated: true, session_id: "session-1", public_key_string: "public-1", identity_revision: 4,
        local_client_id: 2, current_channel_id: 1, server_policy: { is_file_upload_allowed: true } };
    const forwarded = [];
    const bundle = {
        read_state: function()
        {
            return state;
        },
        get_ui_session_snapshot: function()
        {
            return session;
        },
        get_auth_frame: function()
        {
            return JSON.stringify({ message: { type: "authentication_status", status: true } });
        },
        get_connection_status: function()
        {
            return { state: session.authenticated ? "connected" : "reconnecting" };
        },
        set_frame_listener: function(callback)
        {
            listeners.frame = callback;
        },
        set_on_message_processed: function(callback)
        {
            listeners.processed = callback;
        },
        set_connection_status_listener: function(callback)
        {
            listeners.status = callback;
        },
        set_unread_listener: function(callback)
        {
            listeners.unread = callback;
        },
        node_forward_raw_request: function(value)
        {
            forwarded.push(value);
        },
        node_fail_delivery: function(reason)
        {
            throw new Error(reason);
        }
    };
    return { bundle, listeners, state, session, forwarded };
}

async function attach(port, token)
{
    const socket = new adapter.WebSocketClient("ws://127.0.0.1:" + port);
    const frames = [];
    socket.onmessage = function(event)
    {
        frames.push(JSON.parse(event.data));
    };
    await once(socket, "open");
    socket.send(token);
    await wait_for(function()
    {
        return frames.length > 0;
    });
    return { socket, frames };
}

test("replay requires current authentication and includes identity, policy and maintainer generation", async function(t)
{
    const fixture = fixture_bundle();
    fixture.session.authenticated = false;
    let host;
    const endpoint = await new Promise(function(resolve)
    {
        host = loopback.start(fixture.bundle, function(port, token)
        {
            resolve({ port, token });
        });
    });
    t.after(function()
    {
        host.close();
    });
    const page = await attach(endpoint.port, endpoint.token);
    assert.deepEqual(page.frames.map(function(frame)
    {
        return frame.message.type;
    }), ["loopback_status"]);
    fixture.listeners.frame(JSON.stringify({ message: { type: "offline_chat_message", value: "queued during authentication" } }));
    fixture.session.session_id = "fresh-session";
    fixture.session.authenticated = true;
    fixture.listeners.processed("client_list");
    await wait_for(function()
    {
        return page.frames.some(function(frame)
        {
            return frame.message.type === "server_policy";
        });
    });
    const replay = page.frames.map(function(frame)
    {
        return frame.message;
    });
    assert.equal(replay[1].type, "local_session_state");
    assert.equal(replay[1].public_key_string, "public-1");
    assert.equal(replay[2].type, "authentication_status");
    assert.equal(replay.find(function(message)
    {
        return message.type === "channel_maintainer_id";
    }).maintainer_generation, 9);
    const delivery = page.frames.find(function(frame)
    {
        return frame.local_delivery;
    });
    assert.equal(delivery.local_delivery.session_id, "fresh-session");
});

test("detached content remains until matching current-page acknowledgement", async function(t)
{
    const fixture = fixture_bundle();
    let host;
    const endpoint = await new Promise(function(resolve)
    {
        host = loopback.start(fixture.bundle, function(port, token)
        {
            resolve({ port, token });
        });
    });
    t.after(function()
    {
        host.close();
    });
    fixture.listeners.frame(JSON.stringify({ message: { type: "offline_chat_message", value: "opened content" } }));
    const first = await attach(endpoint.port, endpoint.token);
    await wait_for(function()
    {
        return first.frames.some(function(frame)
        {
            return frame.local_delivery;
        });
    });
    const delivery = first.frames.find(function(frame)
    {
        return frame.local_delivery;
    }).local_delivery;
    const second = await attach(endpoint.port, endpoint.token);
    await wait_for(function()
    {
        return second.frames.some(function(frame)
        {
            return frame.local_delivery;
        });
    });
    second.socket.send(JSON.stringify({ message: { type: "local_delivery_ack", id: delivery.id, session_id: "wrong" } }));
    second.socket.send(JSON.stringify({ message: { type: "local_delivery_ack", id: delivery.id, session_id: delivery.session_id } }));
    second.socket.send(JSON.stringify({ message: { type: "ordinary_request" } }));
    await wait_for(function()
    {
        return fixture.forwarded.length === 1;
    });
    const third = await attach(endpoint.port, endpoint.token);
    assert.equal(third.frames.some(function(frame)
    {
        return frame.local_delivery;
    }), false);
    assert.equal(fixture.forwarded.length, 1);
});

test("large retained content crosses the real local chunk receiver and waits for final page acceptance", async function(t)
{
    const source = fs.readFileSync(path.resolve(__dirname, "../../../browser/main/scripts/app/workers/local-transfer.js"), "utf8");
    const receiver_factory = vm.runInNewContext(source + "\nlocal_transfer__create_receiver;",
        { TextDecoder, Uint8Array, atob, btoa });
    const fixture = fixture_bundle();
    let host;
    const endpoint = await new Promise(function(resolve)
    {
        host = loopback.start(fixture.bundle, function(port, token)
        {
            resolve({ port, token });
        });
    });
    t.after(function()
    {
        host.close();
    });
    const original = { message: { type: "direct_chat_file_decrypted", file_id: "file-1", client_id: 3,
        file: crypto.randomBytes(500000).toString("base64"), filename: "žltý 🍋 日本語.bin" } };
    fixture.listeners.frame(JSON.stringify(original));

    async function receive(page, accept_content)
    {
        let delivered = null;
        let cursor = 0;
        const failures = [];
        const receiver = receiver_factory({
            getScope: function()
            {
                return { session_id: fixture.session.session_id, generation: 1 };
            },
            setTimer: setTimeout,
            clearTimer: clearTimeout,
            onAck: function(message)
            {
                page.socket.send(JSON.stringify({ message }));
            },
            onFailure: function(metadata, reason)
            {
                failures.push(reason);
            },
            onFrame: function(text)
            {
                delivered = JSON.parse(text);
                if (accept_content)
                {
                    // This is deliberately synchronous: the receiver must acknowledge the last
                    // chunk before main-thread content acceptance can acknowledge this delivery.
                    page.socket.send(JSON.stringify({ message: Object.assign({ type: "local_delivery_ack" }, delivered.local_delivery) }));
                }
            }
        });
        t.after(function()
        {
            receiver.cancelAll("test_complete");
        });
        await wait_for(function()
        {
            while (cursor < page.frames.length)
            {
                const message = page.frames[cursor++].message;
                if (message.type.startsWith("local_transfer_"))
                {
                    receiver.receive(message);
                }
            }
            return delivered !== null || failures.length > 0;
        });
        assert.deepEqual(failures, []);
        assert.deepEqual(delivered.message, original.message);
        assert.equal(receiver.readState().active_count, 0);
        assert.ok(page.frames.filter(function(frame)
        {
            return frame.message.type === "local_transfer_chunk";
        }).every(function(frame)
        {
            return frame.message.data.length <= 65536;
        }));
        return delivered;
    }

    const first = await attach(endpoint.port, endpoint.token);
    const received_first = await receive(first);
    assert.equal(first.frames.some(function(frame)
    {
        return frame.message.type === original.message.type;
    }), false);
    // All chunks were accepted, but content acceptance has not been acknowledged yet.
    const second = await attach(endpoint.port, endpoint.token);
    const received_second = await receive(second, true);
    assert.deepEqual(received_second.local_delivery, received_first.local_delivery);
    second.socket.send(JSON.stringify({ message: { type: "ordinary_request" } }));
    await wait_for(function()
    {
        return fixture.forwarded.length === 1;
    });
    const third = await attach(endpoint.port, endpoint.token);
    assert.equal(third.frames.some(function(frame)
    {
        return frame.message.type === "local_transfer_begin";
    }), false);
});

test("owner handler failure revokes reservations before any delayed release", async function(t)
{
    const fixture = fixture_bundle();
    const failures = [];
    fixture.bundle.node_fail_delivery = function(reason)
    {
        failures.push(reason);
    };
    let host;
    const endpoint = await new Promise(function(resolve)
    {
        host = loopback.start(fixture.bundle, function(port, token)
        {
            resolve({ port, token });
        });
    });
    t.after(function()
    {
        host.close();
    });
    const first = await attach(endpoint.port, endpoint.token);
    const token = fixture.listeners.frame(JSON.stringify({ message: { type: "offline_chat_message", value: "must remain hidden" } }),
        { defer_delivery: true });
    fixture.listeners.processed("data_processing_worker__offline_chat_message", true);
    fixture.listeners.frame(null, { release_delivery: token });
    assert.deepEqual(failures, ["Could not process an incoming message. Reconnect to continue."]);
    const second = await attach(endpoint.port, endpoint.token);
    for (const page of [first, second])
    {
        assert.equal(page.frames.some(function(frame)
        {
            return frame.message.type === "offline_chat_message";
        }), false);
    }
});

test("configured frame budget preserves the current maximum file representation", function()
{
    const maximum_file_bytes = 100 * 1024 * 1024;
    const complete_base64_bytes = 4 * Math.ceil(maximum_file_bytes / 3);
    assert.ok(complete_base64_bytes + 1024 * 1024 < adapter.limits.messageBytes);
    assert.ok(adapter.limits.messageBytes < adapter.limits.websocketQueueBytes);
});

test("rejected local download admission clears the receiving card and preserves content for a replacement page", async function(t)
{
    const fixture = fixture_bundle();
    let host;
    const endpoint = await new Promise(function(resolve)
    {
        host = loopback.start(fixture.bundle, function(port, token)
        {
            resolve({ port, token });
        });
    });
    t.after(function()
    {
        host.close();
    });
    fixture.listeners.frame(JSON.stringify({ message: { type: "direct_chat_file_decrypted", file_id: "file-rejected", file: "A".repeat(80000) } }));
    const first = await attach(endpoint.port, endpoint.token);
    const begin = first.frames.find(function(frame)
    {
        return frame.message.type === "local_transfer_begin";
    }).message;
    first.socket.send(JSON.stringify({ message: { type: "local_transfer_ack", session_id: begin.session_id,
        transfer_id: begin.transfer_id, index: -1, accepted: false, reason: "allocation_failed" } }));
    await wait_for(function()
    {
        return first.frames.some(function(frame)
        {
            return frame.message.type === "chat_file_decrypt_failed";
        });
    });
    const failure = first.frames.find(function(frame)
    {
        return frame.message.type === "chat_file_decrypt_failed";
    }).message;
    assert.equal(failure.file_id, "file-rejected");
    assert.equal(failure.reason, "could not receive this file");
    assert.equal(first.frames.filter(function(frame)
    {
        return frame.message.type === "local_transfer_begin";
    }).length, 1);
    const second = await attach(endpoint.port, endpoint.token);
    assert.equal(second.frames.some(function(frame)
    {
        return frame.message.type === "local_transfer_begin" && frame.message.file_id === "file-rejected";
    }), true);
});

test("authenticated 100 MiB file completion representation crosses the adapter intact", async function(t)
{
    const server = new adapter.WebSocketServer({ host: "127.0.0.1", port: 0 });
    t.after(function()
    {
        server.close();
    });
    await once(server, "listening");
    const encoded_bytes = 4 * Math.ceil(100 * 1024 * 1024 / 3);
    // Build the stress payload before the handshake deadline starts; this fixture deliberately
    // constructs a representation much larger than normal control messages on the same event loop.
    const payload = JSON.stringify({ message: { type: "direct_chat_file_decrypted", base64: "A".repeat(encoded_bytes) } });
    const expected_bytes = Buffer.byteLength(payload);
    let peer;
    server.on("connection", function(socket)
    {
        server.authenticate(socket);
        peer = socket;
    });
    const client = new adapter.WebSocketClient("ws://127.0.0.1:" + server.address().port);
    const pending_message = new Promise(function(resolve, reject)
    {
        client.onmessage = resolve;
        client.onerror = reject;
    });
    await once(client, "open");
    peer.send(payload);
    const message = await pending_message;
    assert.equal(Buffer.byteLength(message.data), expected_bytes);
    assert.equal(JSON.parse(message.data).message.base64.length, encoded_bytes);
    client.terminate();
});

test("upload completion from a replaced attachment cannot reply to its successor", async function(t)
{
    const fixture = fixture_bundle();
    let finish_upload;
    fixture.bundle.node_forward_upload_request = function()
    {
        return new Promise(function(resolve)
        {
            finish_upload = resolve;
        });
    };
    let host;
    const endpoint = await new Promise(function(resolve)
    {
        host = loopback.start(fixture.bundle, function(port, token)
        {
            resolve({ port, token });
        });
    });
    t.after(function()
    {
        host.close();
    });
    const first = await attach(endpoint.port, endpoint.token);
    first.socket.send(JSON.stringify({ message: { type: "local_upload_request", request_id: "upload-1", request: { type: "chunk" } } }));
    await wait_for(function()
    {
        return typeof finish_upload === "function";
    });
    const second = await attach(endpoint.port, endpoint.token);
    finish_upload(true);
    second.socket.send(JSON.stringify({ message: { type: "ordinary_request" } }));
    await wait_for(function()
    {
        return fixture.forwarded.length === 1;
    });
    assert.equal(second.frames.some(function(frame)
    {
        return frame.message.type === "local_upload_result";
    }), false);
});

test("read acknowledgement cannot clear content that arrived after the page's watermark", async function(t)
{
    const fixture = fixture_bundle();
    let reads = 0;
    fixture.bundle.node_delivery_context = function(frame)
    {
        return frame.message.type === "offline_chat_message" ? "identity:maya" : "";
    };
    fixture.bundle.node_read_conversation = function()
    {
        reads++;
        return true;
    };
    let host;
    const endpoint = await new Promise(function(resolve)
    {
        host = loopback.start(fixture.bundle, function(port, token)
        {
            resolve({ port, token });
        });
    });
    t.after(function()
    {
        host.close();
    });
    const page = await attach(endpoint.port, endpoint.token);
    function delivered()
    {
        return page.frames.filter(function(frame)
        {
            return frame.local_delivery;
        });
    }
    fixture.listeners.frame(JSON.stringify({ message: { type: "offline_chat_message", value: "first" } }));
    await wait_for(function()
    {
        return delivered().length === 1;
    });
    const first = delivered()[0].local_delivery;
    page.socket.send(JSON.stringify({ message: Object.assign({ type: "local_delivery_ack" }, first) }));
    fixture.listeners.frame(JSON.stringify({ message: { type: "offline_chat_message", value: "second" } }));
    page.socket.send(JSON.stringify({ message: { type: "local_command", action: "read_conversation", request_id: "read-old",
        arguments: { context_id: "identity:maya", delivery_id: first.id } } }));
    await wait_for(function()
    {
        return page.frames.some(function(frame)
        {
            return frame.message.request_id === "read-old";
        });
    });
    assert.equal(reads, 0);
    const second = delivered()[1].local_delivery;
    page.socket.send(JSON.stringify({ message: Object.assign({ type: "local_delivery_ack" }, second) }));
    page.socket.send(JSON.stringify({ message: { type: "local_command", action: "read_conversation", request_id: "read-new",
        arguments: { context_id: "identity:maya", delivery_id: second.id } } }));
    await wait_for(function()
    {
        return reads === 1;
    });
});

test("identity completion reaches an unauthenticated page and its replacement", async function(t)
{
    const fixture = fixture_bundle();
    fixture.session.authenticated = false;
    let host;
    const endpoint = await new Promise(function(resolve)
    {
        host = loopback.start(fixture.bundle, function(port, token)
        {
            resolve({ port, token });
        });
    });
    t.after(function()
    {
        host.close();
    });
    const first = await attach(endpoint.port, endpoint.token);
    fixture.listeners.frame(JSON.stringify({ message: { type: "local_identity_result", identity_revision: 5,
        public_key_string: "public-5", rsa_key_bits: 3072, success: true } }));
    await wait_for(function()
    {
        return first.frames.some(function(frame)
        {
            return frame.message.type === "local_identity_result";
        });
    });
    const second = await attach(endpoint.port, endpoint.token);
    assert.ok(second.frames.some(function(frame)
    {
        return frame.message.type === "local_identity_result" && frame.message.identity_revision === 5;
    }));
    assert.equal(second.frames.some(function(frame)
    {
        return frame.message.type === "authentication_status";
    }), false);
});

test("an idle unauthenticated TCP peer is closed by the handshake deadline", async function(t)
{
    const server = new adapter.WebSocketServer({ host: "127.0.0.1", port: 0 });
    t.after(function()
    {
        server.close();
    });
    await once(server, "listening");
    const socket = net.connect({ host: "127.0.0.1", port: server.address().port });
    await once(socket, "connect");
    socket.write("GET / HTTP/1.1\r\n");
    const began = Date.now();
    await once(socket, "close");
    assert.ok(Date.now() - began <= adapter.limits.handshakeTimeoutMs + 2000);
});

test("outgoing capacity closes the peer before accepting another queued frame", async function(t)
{
    const server = new adapter.WebSocketServer({ host: "127.0.0.1", port: 0 });
    t.after(function()
    {
        server.close();
    });
    await once(server, "listening");
    let checked = false;
    server.on("connection", function(socket)
    {
        server.authenticate(socket);
        Object.defineProperty(socket, "bufferedAmount", { value: adapter.limits.websocketQueueBytes });
        assert.throws(function()
        {
            socket.send("one byte beyond capacity");
        }, RangeError);
        checked = true;
    });
    const client = new adapter.WebSocketClient("ws://127.0.0.1:" + server.address().port);
    client.onerror = function()
    {
    };
    await new Promise(function(resolve)
    {
        client.onclose = resolve;
    });
    assert.equal(checked, true);
});

test("Node publishes authoritative peer and channel state independently of content replay", async function(t)
{
    const fixture = fixture_bundle();
    fixture.session.peer_states = [{ key: "identity:maya", unread_count: 3, is_ignored_by_local_client: true, is_muted_by_local_client: false }];
    fixture.session.channel_counts = [{ channel_id: 1, unread_count: 4 }];
    let host;
    const endpoint = await new Promise(function(resolve)
    {
        host = loopback.start(fixture.bundle, function(port, token)
        {
            resolve({ port, token });
        });
    });
    t.after(function()
    {
        host.close();
    });
    const page = await attach(endpoint.port, endpoint.token);
    const initial = page.frames.find(function(frame)
    {
        return frame.message.type === "local_session_state";
    }).message;
    assert.deepEqual(initial.peer_states, fixture.session.peer_states);
    assert.deepEqual(initial.channel_counts, fixture.session.channel_counts);
    fixture.session.peer_states[0].unread_count = 5;
    fixture.session.channel_counts[0].unread_count = 6;
    fixture.listeners.unread(11);
    await wait_for(function()
    {
        return page.frames.some(function(frame)
        {
            return frame.message.type === "local_peer_state";
        });
    });
    const update = page.frames.find(function(frame)
    {
        return frame.message.type === "local_peer_state";
    }).message.value;
    assert.deepEqual(update.peer_states, fixture.session.peer_states);
    assert.deepEqual(update.channel_counts, fixture.session.channel_counts);
    assert.equal(update.session_id, fixture.session.session_id);
});

test("reserved content advances the read watermark but remains hidden until owner processing completes", async function(t)
{
    const fixture = fixture_bundle();
    fixture.bundle.node_delivery_context = function()
    {
        return "identity:maya";
    };
    let reads = 0;
    fixture.bundle.node_read_conversation = function()
    {
        reads++;
        return true;
    };
    let host;
    const endpoint = await new Promise(function(resolve)
    {
        host = loopback.start(fixture.bundle, function(port, token)
        {
            resolve({ port, token });
        });
    });
    t.after(function()
    {
        host.close();
    });
    const page = await attach(endpoint.port, endpoint.token);
    fixture.listeners.frame(JSON.stringify({ message: { type: "offline_chat_message", value: "first" } }));
    await wait_for(function()
    {
        return page.frames.some(function(frame)
        {
            return frame.local_delivery;
        });
    });
    const first = page.frames.find(function(frame)
    {
        return frame.local_delivery;
    }).local_delivery;
    page.socket.send(JSON.stringify({ message: Object.assign({ type: "local_delivery_ack" }, first) }));
    const token = fixture.listeners.frame(JSON.stringify({ message: { type: "offline_chat_message", value: "second" } }), { defer_delivery: true });
    assert.equal(typeof token, "string");
    page.socket.send(JSON.stringify({ message: { type: "local_command", action: "read_conversation", request_id: "old-read",
        arguments: { context_id: "identity:maya", delivery_id: first.id } } }));
    await wait_for(function()
    {
        return page.frames.some(function(frame)
        {
            return frame.message.request_id === "old-read";
        });
    });
    assert.equal(reads, 0);
    assert.equal(page.frames.filter(function(frame)
    {
        return frame.local_delivery;
    }).length, 1);
    const replacement = await attach(endpoint.port, endpoint.token);
    assert.equal(replacement.frames.some(function(frame)
    {
        return frame.local_delivery;
    }), false, "an attachment replay must not leak a reservation before processing");
    fixture.listeners.frame(null, { release_delivery: token });
    await wait_for(function()
    {
        return replacement.frames.some(function(frame)
        {
            return frame.local_delivery;
        });
    });
    assert.equal(replacement.frames.find(function(frame)
    {
        return frame.local_delivery;
    }).message.value, "second");
});
