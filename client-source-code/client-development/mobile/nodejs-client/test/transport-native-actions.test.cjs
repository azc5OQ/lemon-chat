"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const net = require("node:net");
const {once} = require("node:events");
const bridge = require("../android-bridge-client.js");

test("native force-idle combinations are strictly validated", function()
{
    assert.equal(bridge.valid_command({type: "ui_visible", visible: false, force_idle: true}), true);
    assert.equal(bridge.valid_command({type: "ui_visible", visible: true, preserve_idle: true}), true);
    for (const command of [
        {type: "ui_visible", visible: false, force_idle: "true"},
        {type: "ui_visible", visible: true, force_idle: true},
        {type: "ui_visible", visible: false, force_idle: true, preserve_idle: true},
        {type: "call_action", action: "accept", call_id: "a".repeat(32), session_id: "x".repeat(129)}
    ])
    {
        assert.equal(bridge.valid_command(command), false);
    }
});

test("authenticated native control forwards forced idle and returns correlated call outcomes", async function(t)
{
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), "lemon-native-actions-"));
    const config = path.join(directory, "bridge.json");
    const server = net.createServer();
    server.listen(0, "127.0.0.1");
    await once(server, "listening");
    fs.writeFileSync(config, JSON.stringify({port: server.address().port, token: "a".repeat(32)}));
    const call_id = "b".repeat(32);
    const visible = [];
    const calls = [];
    const results = [];
    let socket;
    let finish;
    const completed = new Promise(function(resolve)
    {
        finish = resolve;
    });
    server.on("connection", function(peer)
    {
        socket = peer;
        peer.on("data", bridge.create_line_reader(function(line)
        {
            if (!line.startsWith("{"))
            {
                peer.write(JSON.stringify({type: "ui_visible", visible: false, force_idle: true}) + "\n");
                peer.write(JSON.stringify({type: "call_action", action: "accept", call_id, session_id: "session-1"}) + "\n");
                peer.write(JSON.stringify({type: "call_action", action: "accept", call_id, session_id: "session-1"}) + "\n");
                return;
            }
            const event = JSON.parse(line);
            if (event.type === "call_action_result")
            {
                results.push(event);
                if (results.length === 2)
                {
                    finish();
                }
            }
        }));
    });
    const bundle =
    {
        read_state: function()
        {
            return {g_is_authenticated: false, g_client_list: [], g_local_username: ""};
        },
        set_on_message_processed: function()
        {
        },
        node_set_ui_attached: function(...args)
        {
            visible.push(args);
        },
        node_handle_call_action: function(action, command)
        {
            calls.push({action, call_id: command.call_id, session_id: command.session_id});
            return calls.length === 1;
        }
    };
    const client = bridge.start(bundle, null, {config_path: config});
    t.after(function()
    {
        client.stop();
        if (socket)
        {
            socket.destroy();
        }
        server.close();
        fs.unlinkSync(config);
        fs.rmdirSync(directory);
    });
    let timeout;
    try
    {
        await Promise.race([completed, new Promise(function(resolve, reject)
        {
            timeout = setTimeout(function()
            {
                reject(new Error("Native action results did not arrive"));
            }, 3000);
        })]);
    }
    finally
    {
        clearTimeout(timeout);
    }
    assert.deepEqual(visible, [[false, true, false, true]]);
    assert.deepEqual(calls.map(function(call)
    {
        return call.action;
    }), ["accept_call", "accept_call"]);
    assert.deepEqual(results, [
        {type: "call_action_result", call_id, session_id: "session-1", accepted: true},
        {type: "call_action_result", call_id, session_id: "session-1", accepted: false}
    ]);
});
