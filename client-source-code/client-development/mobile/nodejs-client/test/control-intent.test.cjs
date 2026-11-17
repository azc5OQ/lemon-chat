"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const net = require("node:net");
const {once} = require("node:events");
const bridge = require("../android-bridge-client.js");

function waitFor(predicate)
{
    return new Promise(function(resolve, reject)
    {
        const deadline = Date.now() + 3000;
        const timer = setInterval(function()
        {
            if (predicate())
            {
                clearInterval(timer);
                resolve();
            }
            else if (Date.now() > deadline)
            {
                clearInterval(timer);
                reject(new Error("Timed out waiting for control receipt"));
            }
        }, 5);
    });
}

test("connect intent receipts survive control replacement without undoing a later page disconnect", async function(t)
{
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), "lemon-intent-"));
    const config = path.join(directory, "bridge.json");
    const server = net.createServer();
    const connections = [];
    const events = [];
    const actions = [];
    let wanted = false;
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
    server.listen(0, "127.0.0.1");
    await once(server, "listening");
    fs.writeFileSync(config, JSON.stringify({port: server.address().port, token: "a".repeat(32)}));
    const bundle =
    {
        read_state()
        {
            return {g_is_authenticated: false, g_local_username: "", g_client_list: []};
        },
        set_on_message_processed()
        {
        },
        node_request_connect()
        {
            actions.push("connect");
            wanted = true;
            return true;
        },
        node_disconnect_by_user()
        {
            actions.push("disconnect");
            wanted = false;
            return true;
        }
    };
    const client = bridge.start(bundle, null, {config_path: config, retry_ms: 10});
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
    function send(type, id)
    {
        connections.at(-1).write(JSON.stringify({type, command_id: id}) + "\n");
    }
    function receipts(id)
    {
        return events.filter(function(event)
        {
            return event.type === "connect_intent_result" && event.command_id === id;
        });
    }
    await waitFor(function()
    {
        return connections.length === 1 && events.length >= 3;
    });
    send("connect", "one");
    await waitFor(function()
    {
        return receipts("one").length === 1;
    });
    assert.equal(receipts("one")[0].accepted, true);
    bundle.node_disconnect_by_user();
    connections[0].destroy();
    await waitFor(function()
    {
        return connections.length === 2;
    });
    send("connect", "one");
    await waitFor(function()
    {
        return receipts("one").length === 2;
    });
    assert.equal(wanted, false, "Retrying a lost receipt must not undo a later page action");
    assert.deepEqual(actions, ["connect", "disconnect"]);
    send("disconnect", "one");
    await waitFor(function()
    {
        return receipts("one").length === 3;
    });
    assert.equal(receipts("one")[2].accepted, false, "A reused id cannot change its action");
    send("connect", "two");
    send("disconnect", "three");
    await waitFor(function()
    {
        return receipts("three").length === 1;
    });
    assert.equal(wanted, false);
    assert.deepEqual(actions, ["connect", "disconnect", "connect", "disconnect"]);
});

test("native connection commands require a bounded correlation id", function()
{
    for (const type of ["connect", "disconnect"])
    {
        for (const command_id of [undefined, null, "", [], {}, 1, "a".repeat(129)])
        {
            assert.equal(bridge.valid_command({type, command_id}), false);
        }
        assert.equal(bridge.valid_command({type, command_id: "6fc90e3c-d0c5-42ed-b9f0-039077a4e577"}), true);
    }
});
