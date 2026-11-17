"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const test = require("node:test");
const source = path.resolve(__dirname, "../../../browser/main/scripts/app");

function noop()
{
}

function fixture()
{
    const attempts = [];
    const generated = [];
    const context = vm.createContext(
    {
        console: {log: noop, warn: noop},
        window: {},
        document: {getElementById: function()
        {
            return {style: {}};
        }},
        utils__sleep: function()
        {
            return new Promise(noop);
        },
        connection_crypto__clear_handshake: noop,
        android_host__is_ui_only_runtime: function()
        {
            return false;
        },
        utils__hide_custom_alert: noop
    });
    vm.runInContext(fs.readFileSync(path.join(source, "globals.js"), "utf8"), context);
    vm.runInContext(fs.readFileSync(path.join(source, "crypto/identity-file.js"), "utf8"), context);
    vm.runInContext(fs.readFileSync(path.join(source, "connection/connection.js"), "utf8"), context);
    context.connection__report_connection_status = function(state, reason)
    {
        context.g_connection_status.state = state;
        context.g_connection_status.reason = reason;
    };
    context.connection__set_connect_button_pending = noop;
    context.connection__is_connect_window_mode = function()
    {
        return false;
    };
    context.g_websocket_worker = {postMessage: noop};
    context.g_data_processing_worker = {postMessage: function(message)
    {
        generated.push(message);
    }};
    context.connection__reset_chat_app_keep_identity = function()
    {
        context.g_is_authenticated = false;
        context.g_worker_connection_ended = true;
        context.connection__signal_connection_closed();
    };
    context.connection__attempt_connection = async function(target, identity)
    {
        attempts.push({host: target.host, identity: identity.public_key_string});
        context.g_is_authenticated = true;
        context.g_worker_connection_ended = false;
        return true;
    };
    context.g_node_connection_wanted = true;
    context.g_is_autoconnect_without_user_action_active = false;
    context.g_is_rsa_key_generated = true;
    context.g_identity_slot.set({public_key_string: "identity-a", identity_string: "A".repeat(200)});
    context.target_slot.set({kind: "server", host: "chosen.invalid", port: 1111});
    return {context, attempts, generated};
}

async function settle()
{
    for (let index = 0; index < 6; index++)
    {
        await new Promise(function(resolve)
        {
            setImmediate(resolve);
        });
    }
}

function complete_identity(f)
{
    const request = f.generated.find(function(message)
    {
        return message.type === "mainthread__generate_rsa_keypair";
    });
    assert.ok(request);
    assert.equal(f.context.connection__accept_identity_result(request), true);
    f.context.g_is_rsa_key_generated = true;
    f.context.g_identity_slot.set({public_key_string: "identity-b", identity_string: "B".repeat(200)});
}

test("import while manually connected reconnects to the same target with the new identity", async function()
{
    const f = fixture();
    f.context.connection__connect_loop();
    await settle();
    assert.deepEqual(f.attempts, [{host: "chosen.invalid", identity: "identity-a"}]);
    f.context.connection__request_identity("B".repeat(200), 2048);
    await settle();
    assert.equal(f.attempts.length, 1, "wait for the replacement key rather than reusing the old key");
    complete_identity(f);
    await settle();
    assert.deepEqual(f.attempts, [
        {host: "chosen.invalid", identity: "identity-a"},
        {host: "chosen.invalid", identity: "identity-b"}
    ]);
});

test("import while idle or explicitly disconnected does not create connection intent", function()
{
    for (const explicitly_disconnected of [false, true])
    {
        const f = fixture();
        f.context.g_is_authenticated = false;
        f.context.g_connection_status.state = explicitly_disconnected ? "connecting" : "idle";
        f.context.is_connection_ended_by_user = explicitly_disconnected;
        f.context.connection__request_identity("B".repeat(200), 2048);
        complete_identity(f);
        assert.equal(f.context.is_connect_loop_wake_pending, false);
        assert.equal(f.context.is_connection_ended_by_user, explicitly_disconnected);
        assert.equal(f.attempts.length, 0);
    }
});

test("Disconnect during identity replacement cancels its retained reconnect", async function()
{
    const f = fixture();
    f.context.connection__connect_loop();
    await settle();
    f.context.connection__request_identity("B".repeat(200), 2048);
    await settle();
    f.context.connection__disconnect_by_user();
    complete_identity(f);
    await settle();
    assert.equal(f.attempts.length, 1);
    assert.equal(f.context.is_connection_ended_by_user, true);
    assert.equal(f.context.is_connect_loop_wake_pending, false);
});
