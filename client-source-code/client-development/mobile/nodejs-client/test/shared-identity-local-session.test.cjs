"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const root = path.resolve(__dirname, "../../../browser/main/scripts/app");
const read = function(name)
{
    return fs.readFileSync(path.join(root, name), "utf8");
};
function extract(name, functionName)
{
    const match = read(name).match(new RegExp("^(?:async )?function " + functionName + "\\([^\\n]*\\)\\r?\\n\\{[\\s\\S]*?^\\}", "m"));
    assert.ok(match, functionName);
    return match[0];
}
function noop()
{
}
const socketCommands = [];
const dataCommands = [];
const resetSource = extract("connection/connection.js", "connection__reset_chat_app_keep_identity");
const nodes = new Map();
const page = vm.createContext(
{
    console,
    clearTimeout,
    setTimeout,
    document:
    {
        getElementById(id)
        {
            if (!nodes.has(id))
            {
                nodes.set(id, {style: {}, classList: {add: noop, remove: noop, toggle: noop}});
            }
            return nodes.get(id);
        },
        getElementsByClassName()
        {
            return [];
        },
        querySelectorAll()
        {
            return [];
        }
    },
    window: {},
    UI: {clear_chat_button_onclick: noop},
    groups_window: {},
    chat_peer_local_state: new Map(),
    chat_last_delivery_by_context: new Map(),
    node_session_id: "old-session",
    node_current_call: null,
    g_connection_status: {},
    g_audio_transport: {},
    dispatch_failed_deliveries: new Set(),
    dispatch_accepted_deliveries: new Set(),
    DBG_WORKER_BOOT_LOG: false,
    g_identity_revision: 1,
    connection__flush_local_connect_intent: noop,
    chat__hydrate_peer_states: noop,
    chat__hydrate_channel_counts: noop,
    android_host__is_ui_only_runtime()
    {
        return true;
    }
});
vm.runInContext(read("globals.js"), page);
for (const match of resetSource.matchAll(/\b([A-Za-z][A-Za-z0-9_]*__[A-Za-z0-9_]+)\(/g))
{
    page[match[1]] = noop;
}
page.connection__is_connect_window_mode = function()
{
    return false;
};
page.g_websocket_worker =
{
    postMessage(message)
    {
        socketCommands.push(message);
    }
};
page.g_data_processing_worker =
{
    postMessage(message)
    {
        dataCommands.push(message);
    }
};
page.connection__extend_connect_page_holdback = noop;
page.connection__reveal_connect_page = noop;
page.connection__render_connection_status = noop;
page.g_is_running_in_android_webview = true;
page.g_worker_connection_generation = 3;
page.g_is_authenticated = true;
page.g_is_client_list_retrieved = true;
vm.runInContext(resetSource, page);
vm.runInContext(read("msg/dispatch.js"), page);
page.dispatch__mainthread_onmessage({data: {type: "data_processing_worker__loopback_status", value: {state: "idle"}, connection_generation: 3}});
assert.equal(page.g_is_authenticated, false);
assert.equal(page.g_is_client_list_retrieved, false);
assert.equal(page.g_worker_connection_ended, false);
assert.equal(socketCommands.length, 0);
assert.equal(dataCommands.length, 0);

const workerOutput = [];
const worker = vm.createContext(
{
    console,
    performance,
    clearTimeout,
    clearInterval,
    g_loopback_port: 1234,
    g_is_authenticated: true,
    g_identity_revision: 1,
    g_rsa_public_key_string: "old-public",
    DBG_WORKER_BOOT_LOG: false,
    keys__clear_channel_keys: noop,
    utils__custom_log: noop,
    global:
    {
        postMessage(message)
        {
            workerOutput.push(message);
        }
    }
});
vm.runInContext(read("workers/workers.js"), worker);
worker.workers__apply_keepalive_policy = noop;
worker.workers__apply_channel_key_policy = noop;
function frame(message)
{
    worker.workers__mainthread__process_received_websocket_message_continue({data: {is_plaintext: true, value: JSON.stringify({message})}});
}
frame({type: "loopback_status", value: {state: "idle"}});
frame({type: "local_session_state", value: {session_id: "new-session", public_key_string: "new-public", identity_revision: 2}});
frame({type: "authentication_status", value: "success"});
assert.equal(worker.g_is_authenticated, true);
assert.equal(worker.g_rsa_public_key_string, "new-public");
page.node_session_id = "old-session";
page.g_is_client_list_retrieved = true;
page.g_is_authenticated = true;
page.dispatch__mainthread_onmessage({data: workerOutput.find(function(event)
{
    return event.type === "data_processing_worker__loopback_status";
})});
page.dispatch__mainthread_onmessage({data: workerOutput.find(function(event)
{
    return event.type === "data_processing_worker__local_session_state";
})});
assert.equal(page.node_session_id, "new-session");
assert.equal(page.g_rsa_public_key_string, "new-public");
assert.equal(socketCommands.length, 0);
assert.equal(dataCommands.length, 0, "A delayed main-thread reset cannot reset the worker after it processed the new login burst");
assert.equal(worker.g_is_authenticated, true);
page.g_is_authenticated = true;
page.dispatch__mainthread_onmessage({data: {type: "data_processing_worker__loopback_status", value: {state: "connecting"}}});
assert.equal(page.g_is_authenticated, false);
page.connection__reset_chat_app_keep_identity();
assert.ok(socketCommands.some(function(message)
{
    return message.type === "close";
}));
assert.ok(dataCommands.some(function(message)
{
    return message.type === "mainthread_reset_data";
}));
console.log("PASS local disconnect clears authenticated UI while keeping status transport; queued reset cannot invalidate a subsequent worker login; ordinary reset still closes transport");
