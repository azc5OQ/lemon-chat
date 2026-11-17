"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const test = require("node:test");
const root = path.resolve(__dirname, "../../../browser/main/scripts/app");
function read(name)
{
    return fs.readFileSync(path.join(root, name), "utf8");
}
function noop()
{
}
function fixture()
{
    const nodes = new Map();
    const socketCommands = [];
    const dataCommands = [];
    const context = vm.createContext(
    {
        console, clearTimeout, setTimeout,
        navigator: {onLine: false},
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
        window: {}, UI: {clear_chat_button_onclick: noop}, groups_window: {},
        chat_peer_local_state: new Map(), chat_last_delivery_by_context: new Map(),
        dispatch_failed_deliveries: new Set(), dispatch_processed_deliveries: new Set(), dispatch_accepted_deliveries: new Set(),
        g_connection_status: {state: "connected", reason: "", next_retry_at: 99},
        g_audio_transport: {}, g_identity_revision: 1, DBG_WORKER_BOOT_LOG: false,
        connection_crypto__clear_handshake: noop,
        utils__storage_get: noop
    });
    vm.runInContext(read("globals.js") + read("connection/connection.js") + read("mobile/android-host.js"), context);
    const reset = read("connection/connection.js").match(/^function connection__reset_chat_app_keep_identity\([^\n]*\)\r?\n\{[\s\S]*?^\}/m)[0];
    for (const match of reset.matchAll(/\b([A-Za-z][A-Za-z0-9_]*__[A-Za-z0-9_]+)\(/g))
    {
        if (match[1] !== "connection__reset_chat_app_keep_identity")
        {
            context[match[1]] = noop;
        }
    }
    context.connection__is_connect_window_mode = function()
    {
        return false;
    };
    context.connection__reveal_connect_page = noop;
    context.g_websocket_worker =
    {
        postMessage(message)
        {
            socketCommands.push(message);
        }
    };
    context.g_data_processing_worker =
    {
        postMessage(message)
        {
            dataCommands.push(message);
        }
    };
    context.g_is_running_in_android_webview = true;
    context.g_is_authenticated = true;
    context.g_is_client_list_retrieved = true;
    context.g_worker_connection_generation = 3;
    context.connection_ui_connect_pending = true;
    context.connection_loopback_ready = true;
    context.ui_connect_requested = true;
    context.is_connect_loop_wake_pending = true;
    context.g_loopback_port = 1234;
    context.g_loopback_token = "old-capability";
    return {context, socketCommands, dataCommands, nodes};
}

test("native terminal runtime failure overrides authenticated UI, cancels attempts, and survives later offline/status events", function()
{
    const f = fixture();
    let canceled = 0;
    let closed = 0;
    let nudged = 0;
    let connected = 0;
    f.context.connection_attempt_cancellations.add(function()
    {
        canceled++;
    });
    f.context.connection_closed_resolvers.push(function()
    {
        closed++;
    });
    f.context.connect_loop_wake_resolver = function()
    {
        nudged++;
    };
    f.context.Android =
    {
        JavaExportConnect()
        {
            connected++;
        }
    };
    const reason = "The app runtime stopped. Force-stop Lemon Chat, then open it again.";
    f.context.android_js_bridge.show_connection_phase_android("runtime_failed", reason);
    assert.equal(f.context.g_is_authenticated, false);
    assert.equal(f.context.g_is_client_list_retrieved, false);
    assert.equal(f.context.g_worker_connection_ended, true);
    assert.equal(f.context.connection_ui_connect_pending, false);
    assert.equal(f.context.ui_connect_requested, false);
    assert.equal(f.context.connection_loopback_ready, false);
    assert.equal(f.context.g_loopback_port, 0);
    assert.equal(f.context.g_loopback_token, "");
    assert.equal(f.context.g_node_connection_wanted, false);
    assert.equal(canceled, 1);
    assert.equal(closed, 1);
    assert.equal(nudged, 1);
    assert.equal(f.socketCommands.filter(function(command)
    {
        return command.type === "close";
    }).length, 1);
    assert.equal(f.nodes.get("communication-system-container").style.display, "none");
    for (const state of ["connecting", "connected", "waiting_retry", "idle", "error"])
    {
        f.context.android_js_bridge.show_connection_phase_android(state, "stale native phase");
        f.context.connection__report_connection_status(state, "stale socket phase");
        assert.equal(f.context.g_connection_status.state, "runtime_failed");
        assert.equal(f.nodes.get("connection-status-reason").textContent, reason);
        assert.equal(f.nodes.get("connection-status-countdown").textContent, "");
    }
    for (const trigger of ["button", "attach", "resume", "settings", "retry"])
    {
        f.context.connection__request_connect(trigger);
    }
    assert.equal(connected, 0);
    assert.equal(f.context.connection__attempt_is_current(f.context.connection_desired_generation), false);
    f.context.android_js_bridge.show_connection_phase_android("runtime_failed", reason);
    assert.equal(canceled, 1, "Replayed terminal phase does not reset twice");
});

test("terminal runtime failure before worker startup has no socket dependency", function()
{
    const f = fixture();
    f.context.g_websocket_worker = null;
    f.context.g_data_processing_worker = null;
    f.context.android_js_bridge.show_connection_phase_android("runtime_failed", "");
    assert.equal(f.context.g_is_authenticated, false);
    assert.match(f.nodes.get("connection-status-reason").textContent, /Force-stop/);
    assert.equal(f.socketCommands.length, 0);
});
