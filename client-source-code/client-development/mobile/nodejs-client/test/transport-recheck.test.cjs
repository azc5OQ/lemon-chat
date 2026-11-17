"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const {once} = require("node:events");
const adapter = require("../websocket-adapter.js");
const loopback = require("../loopback-ui-server.js");

function waitFor(predicate)
{
    return new Promise(function(resolve, reject)
    {
        const deadline = Date.now() + 2000;
        const timer = setInterval(function()
        {
            if (predicate())
            {
                clearInterval(timer);
                resolve();
            }
            else if (Date.now() >= deadline)
            {
                clearInterval(timer);
                reject(new Error("Timed out waiting for bridge result"));
            }
        }, 5);
    });
}

async function fixture(t, overrides = {})
{
    const listeners = {};
    const state = {g_is_client_list_retrieved: true, g_client_list: [], g_channel_list: [],
        g_local_username: "Me", g_groups: [], g_icons: []};
    const session = {authenticated: true, session_id: "session-one", public_key_string: "public-one",
        identity_revision: 1, local_client_id: 1, current_channel_id: 1};
    const bundle =
    {
        read_state()
        {
            return state;
        },
        get_ui_session_snapshot()
        {
            return session;
        },
        get_connection_status()
        {
            return {state: "connected"};
        },
        get_auth_frame()
        {
            return JSON.stringify({message: {type: "authentication_status", status: true}});
        },
        set_frame_listener(callback)
        {
            listeners.frame = callback;
        },
        set_on_message_processed(callback)
        {
            listeners.processed = callback;
        },
        set_connection_status_listener()
        {
        },
        set_unread_listener()
        {
        },
        ...overrides
    };
    let host;
    const endpoint = await new Promise(function(resolve)
    {
        host = loopback.start(bundle, function(port, token)
        {
            resolve({port, token});
        });
    });
    t.after(function()
    {
        host.close();
    });
    const socket = new adapter.WebSocketClient("ws://127.0.0.1:" + endpoint.port);
    const frames = [];
    socket.onmessage = function(event)
    {
        frames.push(JSON.parse(event.data));
    };
    await once(socket, "open");
    socket.send(endpoint.token);
    await waitFor(function()
    {
        return frames.some(function(frame)
        {
            return frame.message.type === "icon_list";
        });
    });
    function request(message)
    {
        socket.send(JSON.stringify({message}));
    }
    return {bundle, state, session, listeners, socket, frames, request, endpoint};
}

test("a recreated page receives current group access and channel roles", async function(t)
{
    const f = await fixture(t);
    f.state.g_groups = [{group_id: 2, group_name: "Helpers"}];
    f.state.group_access = {view_group_permissions: true, edit_channel_groups: false, powers: {permission_edit_power: 40}};
    f.state.channel_groups = [{group_id: 1, name: "Channel Admin", permissions: {talk_power: 1000}}];
    const page = new adapter.WebSocketClient("ws://127.0.0.1:" + f.endpoint.port);
    const frames = [];
    page.onmessage = function(event)
    {
        frames.push(JSON.parse(event.data));
    };
    await once(page, "open");
    page.send(f.endpoint.token);
    await waitFor(function()
    {
        return frames.some(function(frame)
        {
            return frame.message.type === "group_list";
        });
    });
    const replay = frames.find(function(frame)
    {
        return frame.message.type === "group_list";
    }).message;
    assert.deepEqual(replay.groups, f.state.g_groups);
    assert.deepEqual(replay.access, f.state.group_access);
    assert.deepEqual(replay.channel_groups, f.state.channel_groups);
    assert.equal(replay.access.edit_channel_groups, false);
});

test("initial replay waits for the current maintainer before delivering cached keys", async function(t)
{
    const f = await fixture(t);
    f.state.g_channel_list = [{channel_id: 1, has_maintainer: true}, {channel_id: 2, has_maintainer: true}];
    f.listeners.frame(JSON.stringify({message: {type: "direct_chat_message", sender_id: 7,
        some_json: {type: "channel_keys_from_maintainer", channel_id: 1, maintainer_generation: 3}}}));
    const page = new adapter.WebSocketClient("ws://127.0.0.1:" + f.endpoint.port);
    const frames = [];
    page.onmessage = function(event)
    {
        frames.push(JSON.parse(event.data));
    };
    await once(page, "open");
    page.send(f.endpoint.token);
    await new Promise(resolve => setTimeout(resolve, 50));
    assert.equal(frames.some(frame => frame.message.type === "client_list"), false);
    f.state.g_channel_list[0].maintainer_id = 7;
    f.state.g_channel_list[0].maintainer_generation = 3;
    f.listeners.processed("data_processing_worker__channel_maintainer_id", false);
    await waitFor(() => frames.some(frame => frame.message.some_json));
    const notices = frames.filter(frame => frame.message.type === "channel_maintainer_id");
    assert.equal(notices.length, 1, "incomplete other-channel notices are omitted");
    assert.equal(notices[0].message.maintainer_id, 7);
    assert.equal(notices[0].message.maintainer_generation, 3);
    assert.ok(frames.indexOf(notices[0]) < frames.findIndex(frame => frame.message.some_json));
});

test("idle replay does not wait for an unrelated channel maintainer", async function(t)
{
    const f = await fixture(t);
    f.session.current_channel_id = -2;
    f.state.g_channel_list = [{channel_id: 1, has_maintainer: true}];
    const page = new adapter.WebSocketClient("ws://127.0.0.1:" + f.endpoint.port);
    const frames = [];
    page.onmessage = function(event)
    {
        frames.push(JSON.parse(event.data));
    };
    await once(page, "open");
    page.send(f.endpoint.token);
    await waitFor(() => frames.some(frame => frame.message.type === "icon_list"));
    assert.equal(frames.some(frame => frame.message.type === "channel_maintainer_id"), false);
});

test("synchronous upload preparation failure returns refusal and releases the next upload", async function(t)
{
    const source = fs.readFileSync(path.resolve(__dirname, "../../../browser/main/scripts/app/connection/connection.js"), "utf8");
    const prepare = source.match(/^function connection__process_message_before_sending\([^\n]*\)\r?\n\{[\s\S]*?^\}/m);
    assert.ok(prepare);
    const context = vm.createContext(
    {
        utils__custom_log()
        {
        }
    });
    vm.runInContext(prepare[0], context);
    const attempts = [];
    const f = await fixture(t,
    {
        node_forward_upload_request(request)
        {
            // The production formatter throws before the shared upload path creates its Promise.
            const prepared = context.connection__process_message_before_sending(request);
            attempts.push(prepared);
            return Promise.resolve(true);
        }
    });
    f.request({type: "local_upload_request", request_id: "malformed", request: {}});
    await waitFor(function()
    {
        return f.frames.some(function(frame)
        {
            return frame.message.request_id === "malformed";
        });
    });
    assert.equal(f.frames.find(function(frame)
    {
        return frame.message.request_id === "malformed";
    }).message.accepted, false);
    f.request({type: "local_upload_request", request_id: "valid", request: {message: {type: "file_send_chunk", chunk: "abc"}}});
    await waitFor(function()
    {
        return f.frames.some(function(frame)
        {
            return frame.message.request_id === "valid";
        });
    });
    assert.equal(f.frames.find(function(frame)
    {
        return frame.message.request_id === "valid";
    }).message.accepted, true);
    assert.equal(attempts.length, 1);
});

test("refused local acknowledgements, uploads and commands never reach raw forwarding", async function(t)
{
    const forwarded = [];
    const uploads = [];
    let finish_upload;
    const f = await fixture(t,
    {
        node_forward_raw_request(text)
        {
            forwarded.push(JSON.parse(text).message);
        },
        node_forward_upload_request(request)
        {
            uploads.push(request);
            return new Promise(function(resolve)
            {
                finish_upload = resolve;
            });
        }
    });
    f.listeners.frame(JSON.stringify({message: {type: "offline_chat_message", value: "retained"}}));
    await waitFor(function()
    {
        return f.frames.some(function(frame)
        {
            return frame.local_delivery;
        });
    });
    const delivery = f.frames.find(function(frame)
    {
        return frame.local_delivery;
    }).local_delivery;
    f.request({type: "local_delivery_ack", id: "unknown", session_id: delivery.session_id});
    f.request({type: "local_delivery_ack", id: delivery.id, session_id: "old-session"});
    f.request({type: "local_delivery_ack", id: delivery.id, session_id: delivery.session_id, accepted: "yes"});
    f.request({type: "local_delivery_ack", id: delivery.id, session_id: delivery.session_id, accepted: false});
    f.request({type: "local_delivery_ack", id: delivery.id, session_id: delivery.session_id, discard_attachment: true});
    f.request({type: "local_upload_request", request_id: 1, request: {message: {type: "file_send_chunk"}}});
    f.request({type: "local_upload_request", request_id: "invalid", request: []});
    f.request({type: "local_upload_request", request_id: "first", request: {message: {type: "file_send_chunk", chunk: "abc"}}});
    await waitFor(function()
    {
        return typeof finish_upload === "function";
    });
    f.request({type: "local_upload_request", request_id: "busy", request: {message: {type: "file_send_chunk", chunk: "def"}}});
    f.request({type: "local_command", action: "unknown", request_id: "refused"});
    f.request({type: "ordinary_request", value: "queue-fence"});
    await waitFor(function()
    {
        return forwarded.some(function(message)
        {
            return message.type === "ordinary_request";
        }) && ["busy", "refused"].every(function(request_id)
        {
            return f.frames.some(function(frame)
            {
                return frame.message.request_id === request_id;
            });
        });
    });
    assert.deepEqual(forwarded, [{type: "ordinary_request", value: "queue-fence"}]);
    assert.equal(uploads.length, 1);
    for (const request_id of ["busy", "refused"])
    {
        assert.equal(f.frames.find(function(frame)
        {
            return frame.message.request_id === request_id;
        }).message.accepted, false);
    }
    finish_upload(false);
    await waitFor(function()
    {
        return f.frames.some(function(frame)
        {
            return frame.message.request_id === "first";
        });
    });
});
