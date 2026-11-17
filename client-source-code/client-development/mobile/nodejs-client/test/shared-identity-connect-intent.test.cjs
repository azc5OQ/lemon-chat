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
function noop()
{
}
function fixture(ui = false)
{
    const socketCommands = [];
    const localCommands = [];
    const sleeps = [];
    const context = vm.createContext(
    {
        console: {log: noop, warn: noop},
        process: {},
        window: {},
        document:
        {
            location: {protocol: "file:"},
            getElementById()
            {
                return {value: "", style: {}};
            }
        },
        g_connection_status: {state: "idle"},
        g_audio_transport: {},
        g_identity_revision: 0,
        g_identity_rsa_key_bits: 2048,
        android_host__is_ui_only_runtime()
        {
            return ui;
        },
        utils__sleep()
        {
            return new Promise(function(resolve)
            {
                sleeps.push(resolve);
            });
        },
        keys__init_keys_object: noop,
        chat__sanitize_string(value)
        {
            return value;
        },
        keys__get_public_mix()
        {
            return 42;
        },
        connection_crypto__begin: noop,
        keys__encrypt_all_message_data_and_convert_to_base64(value)
        {
            return value;
        }
    });
    vm.runInContext(read("globals.js"), context);
    vm.runInContext(read("connection/connection.js"), context);
    context.g_websocket_worker =
    {
        postMessage(message)
        {
            socketCommands.push(message);
        }
    };
    context.g_data_processing_worker = {postMessage: noop};
    context.connection__set_connect_button_pending = noop;
    context.connection__hold_back_connect_page = noop;
    context.connection__wake_connect_loop = noop;
    context.connection__is_connect_window_mode = function()
    {
        return false;
    };
    context.connection__process_message_before_sending = JSON.stringify;
    context.connection__send_local_command = function(action)
    {
        localCommands.push(action);
    };
    context.utils__hide_custom_alert = noop;
    context.g_node_connection_wanted = true;
    return {context, socketCommands, localCommands, sleeps};
}

async function run()
{
    const pending = fixture();
    const identityWait = pending.context.connection__wait_for_identity(pending.context.connection_desired_generation);
    assert.equal(pending.context.g_identity_slot.waiters.length, 1);
    pending.context.connection__invalidate_pending_attempts();
    assert.equal(await identityWait, null);
    assert.equal(pending.context.g_identity_slot.waiters.length, 0);

    const target = fixture();
    target.context.g_are_server_details_predefined = true;
    target.context.g_autoconnect_details = {host: "a.invalid", port: 1111, keys: []};
    target.context.connection__request_connect("button");
    const first = target.context.connection__attempt_connection(target.context.target_slot.value, {public_key_string: "test-key"},
        target.context.connection_desired_generation);
    assert.equal(target.sleeps.length, 1);
    target.context.connection__request_connect("bookmark", {host: "b.invalid", port: 2222});
    target.sleeps.shift()();
    assert.equal(await first, false);
    assert.equal(target.socketCommands.some(function(message)
    {
        return message.type === "create_websocket_object";
    }), false);
    target.context.keys__init_keys_object = function()
    {
        target.context.g_keys_init_status = true;
    };
    assert.equal(await target.context.connection__attempt_connection(target.context.target_slot.value,
        {public_key_string: "test-key"}, target.context.connection_desired_generation), true);
    assert.equal(target.socketCommands.find(function(message)
    {
        return message.type === "create_websocket_object";
    }).value.connection_string,
        "ws://b.invalid:2222/");

    const browser = fixture();
    browser.context.g_are_server_details_predefined = true;
    browser.context.g_autoconnect_details = {host: "saved.example", port: 1111, keys: []};
    browser.context.is_written_in_target_shown_in_window = true;
    browser.context.document.getElementById = function(id)
    {
        return {value: id === "input-ip-address" ? "edited.example" : "2222", style: {}};
    };
    browser.context.connection__request_connect("button");
    assert.equal(browser.context.target_slot.value.host, "edited.example");
    assert.equal(browser.context.target_slot.value.port, "2222");

    const canceled = fixture();
    canceled.context.g_connection_status.state = "connecting";
    const attempt = canceled.context.connection__attempt_connection({kind: "server", host: "a.invalid", port: 1111},
        {public_key_string: "test-key"}, canceled.context.connection_desired_generation);
    canceled.context.connection__disconnect_by_user();
    canceled.sleeps.shift()();
    assert.equal(await attempt, false);
    assert.equal(canceled.socketCommands.some(function(message)
    {
        return message.type === "create_websocket_object";
    }), false);
    canceled.context.g_is_autoconnect_without_user_action_active = true;
    canceled.context.connection__request_connect("settings");
    assert.equal(canceled.context.is_connection_ended_by_user, true);
    assert.equal(canceled.context.target_slot.is_set, false);

    const native = fixture(true);
    let nativeConnects = 0;
    let nativeDisconnects = 0;
    native.context.Android =
    {
        JavaExportConnect()
        {
            nativeConnects++;
        },
        JavaExportDisconnect()
        {
            nativeDisconnects++;
        }
    };
    native.context.g_loopback_port = 0;
    native.context.connection__request_connect("button");
    assert.equal(nativeConnects, 1);
    assert.equal(native.context.ui_connect_requested, true);
    assert.equal(native.context.connection_ui_connect_pending, false, "Native owner retains the explicit intent; the page must not replay it after a later moderation close");
    native.context.connection__disconnect_by_user();
    assert.equal(nativeDisconnects, 1, "Disconnect reaches the native owner even before a page socket exists");
    assert.equal(native.context.connection_ui_disconnect_pending, false);
    native.context.g_loopback_port = 1234;
    native.context.g_loopback_token = "local-token";
    native.context.connection__request_connect("settings");
    assert.equal(native.context.target_slot.value.kind, "loopback");
    assert.deepEqual(native.localCommands, []);

    const local = fixture(true);
    local.context.g_loopback_port = 1234;
    local.context.g_loopback_token = "local-token";
    local.context.connection__request_connect("button");
    assert.equal(local.context.connection_ui_connect_pending, true);
    assert.equal(local.context.target_slot.value.kind, "loopback");
    assert.deepEqual(local.localCommands, []);
    local.context.connection_loopback_ready = true;
    local.context.connection__flush_local_connect_intent();
    local.context.connection__flush_local_connect_intent();
    assert.deepEqual(local.localCommands, ["connect"]);
    local.context.connection_loopback_ready = false;
    local.context.connection__request_connect("button");
    local.context.connection__disconnect_by_user();
    assert.deepEqual(local.localCommands, ["connect"], "A fallback Disconnect is retained while the local socket is unavailable");
    local.context.connection_loopback_ready = true;
    local.context.connection__flush_local_connect_intent();
    assert.deepEqual(local.localCommands, ["connect", "disconnect"]);
    local.context.connection_loopback_ready = false;
    local.context.connection__disconnect_by_user();
    local.context.connection__request_connect("button");
    local.context.connection_loopback_ready = true;
    local.context.connection__flush_local_connect_intent();
    assert.deepEqual(local.localCommands, ["connect", "disconnect", "connect"], "A newer explicit Connect replaces the fallback pending Disconnect");
    console.log("PASS identity/key waits cancel, target replacement opens only B, settings cannot undo disconnect, early native and local Connect retain explicit intent without attach-driven reconnect");
}
run().catch(function(error)
{
    console.error(error);
    process.exitCode = 1;
});
