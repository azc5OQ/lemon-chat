"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const root = path.resolve(__dirname, "../../../browser/main/scripts/app");

function noop()
{
}

function extract(relative, name)
{
    const source = fs.readFileSync(path.join(root, relative), "utf8");
    const found = source.match(new RegExp("^function " + name + "\\([^\\n]*\\)\\r?\\n\\{[\\s\\S]*?^\\}", "m"));
    assert.ok(found, name);
    return found[0];
}

function fixture(oldKeys)
{
    const closes = [];
    const requests = [];
    const targets = [];
    const elements = new Map();
    const context =
    {
        console: { log: noop },
        G_HAS_DOM: true,
        node_identity_blocked: false,
        g_identity_revision: 0,
        is_connection_ended_by_user: false,
        connection__invalidate_pending_attempts: noop,
        CONNECTION_CIPHERS: ["aes256", "chacha20", "camellia256", "twofish256", "serpent256", "trivium80"],
        g_is_authenticated: true,
        g_autoconnect_details: { host: "synthetic.invalid", port: 9000, keys: oldKeys },
        g_loopback_port: 0,
        g_loopback_token: "",
        g_is_microphone_always_on: false,
        g_have_received_android_settings: true,
        g_connection_status: { state: "connected" },
        ui_connect_requested: false,
        connection_chosen_target: null,
        main__apply_theme_for_app_mode: noop,
        utils__storage_get()
        {
            return null;
        },
        connection__request_identity(seed)
        {
            requests.push(seed);
        },
        connection__wake_connect_loop: noop,
        target_slot:
        {
            set(target)
            {
                targets.push(target);
            }
        },
        g_websocket_worker:
        {
            postMessage(message)
            {
                closes.push(message);
            }
        },
        g_data_processing_worker: { postMessage: noop },
        document:
        {
            getElementById(id)
            {
                if (!elements.has(id))
                {
                    elements.set(id, { style: {}, value: "" });
                }
                return elements.get(id);
            }
        }
    };
    vm.createContext(context);
    const host = fs.readFileSync(path.join(root, "mobile/android-host.js"), "utf8");
    const method = host.match(/^    accept_current_settings_from_android: function\([^\n]*\)\r?\n    \{[\s\S]*?^    \}/m);
    assert.ok(method);
    vm.runInContext("var acceptSettings = " + method[0].replace(/^\s*accept_current_settings_from_android: /, "")
        + ";\n" + extract("mobile/android-host.js", "android_host__is_ui_only_runtime")
        + "\n" + extract("mobile/android-host.js", "android_host__metadata_keys_signature")
        + "\n" + extract("mobile/android-host.js", "android_host__apply_settings_presentation")
        + "\n" + extract("connection/connection.js", "connection__request_connect")
        + "\n" + extract("crypto/connection-crypto.js", "connection_crypto__password_entry"), context);
    return {
        context,
        closes,
        targets,
        apply(keys)
        {
            for (const key of keys)
            {
                context.connection_crypto__password_entry(key);
            }
            context.acceptSettings(JSON.stringify(
            {
                host: "synthetic.invalid",
                port: 9000,
                metadata_keys: keys,
                identity_string: "S".repeat(200),
                is_autoconnect_enabled: true,
                is_microphone_always_on: false
            }));
        }
    };
}

for (const replacement of [
    { type: "chacha20", password: "synthetic-B" },
    { type: "serpent256", password: "synthetic-A" }
])
{
    const f = fixture([{ type: "chacha20", password: "synthetic-A" }]);
    f.apply([replacement]);
    assert.equal(f.closes.length, 1);
    assert.equal(f.context.g_is_authenticated, true);
    assert.equal(f.context.g_autoconnect_details.keys[0].type, replacement.type);
    assert.equal(f.context.g_autoconnect_details.keys[0].password, replacement.password);
    assert.equal(f.targets.length, 1);
    console.log("PASS: typed password/cipher changes invalidate the old authenticated socket.");
}

{
    const f = fixture(["synthetic-A"]);
    f.apply(["synthetic-B"]);
    assert.equal(f.closes.length, 1);
    assert.equal(f.closes[0].type, "close");
    console.log("CONTROL: ordinary legacy string password edit closes the old authenticated socket.");
}

{
    const f = fixture([{ type: "chacha20", password: "synthetic-A" }]);
    f.apply([{ type: "chacha20", password: "synthetic-A" }]);
    assert.equal(f.closes.length, 0);
    console.log("CONTROL: unchanged typed credentials do not close the current connection.");
}

{
    const f = fixture(["synthetic-A"]);
    f.apply([{type: "aes256", password: "synthetic-A"}]);
    assert.equal(f.closes.length, 0);
    assert.equal(f.context.g_autoconnect_details.keys[0].type, "aes256");
    console.log("PASS: saving a legacy password as a typed AES key keeps the current connection.");
}

{
    const types = ["aes256", "chacha20", "camellia256", "twofish256", "serpent256", "trivium80"];
    const keys = types.map(function(type, index)
    {
        return {type: type, password: "synthetic-layer-" + index};
    });
    const f = fixture(keys.slice());
    f.apply(keys);
    assert.equal(f.closes.length, 0);
    assert.deepEqual(JSON.parse(JSON.stringify(f.context.g_autoconnect_details.keys)), keys);
    const reordered = keys.slice().reverse();
    f.apply(reordered);
    assert.equal(f.closes.length, 1);
    assert.deepEqual(JSON.parse(JSON.stringify(f.context.g_autoconnect_details.keys)), reordered);
    console.log("PASS: all six typed keys retain their order and changing that order reconnects.");
}

{
    const f = fixture([]);
    assert.throws(function()
    {
        f.context.connection_crypto__password_entry({ type: "unsupported", password: "synthetic" });
    }, /Invalid connection password or cipher type/);
    const literal = f.context.connection_crypto__password_entry(JSON.stringify({ type: "chacha20", password: "synthetic-A" }));
    assert.equal(literal.type, "aes256");
    assert.match(literal.password, /^\{/);
    console.log("CONTROL: invalid cipher objects are rejected; stringified JSON is explicitly interpreted as a literal legacy AES password.");
}

{
    const f = fixture([]);
    const events = [];
    f.context.main__apply_theme_for_app_mode = function()
    {
        events.push("theme");
    };
    f.context.document.getElementById = function(id)
    {
        return {style: new Proxy({},
        {
            set(target, property, value)
            {
                assert.equal(f.context.g_autoconnect_details.host, "synthetic.invalid");
                events.push(id + "." + property);
                return true;
            }
        })};
    };
    f.context.connection__reveal_connect_page = function()
    {
        assert.equal(f.context.g_autoconnect_details.host, "synthetic.invalid");
        events.push("reveal");
    };
    f.context.g_data_processing_worker.postMessage = function(message)
    {
        events.push(message.type);
    };
    f.context.connection__invalidate_pending_attempts = function()
    {
        events.push("invalidate");
    };
    f.context.g_websocket_worker.postMessage = function(message)
    {
        events.push(message.type);
    };
    f.context.connection__request_identity = function()
    {
        events.push("identity");
    };
    const result = f.context.acceptSettings(JSON.stringify(
    {
        host: "replacement.invalid", port: 9001, metadata_keys: [],
        is_app_log_enabled: false, is_autoconnect_enabled: false, is_audio_effect_enabled: true
    }));
    assert.equal(result, undefined);
    assert.deepEqual(events,
    [
        "theme",
        "show-hide-log-button.display", "textarea-log.display",
        "connect-form-grid.display", "add-key-button.display", "server-bookmarks-container.display",
        "another-buttons-sub-container.display", "another-buttons-sub-loading-container.display",
        "connect-button.visibility", "import-identity-button.display", "reveal",
        "sound-effects-button.backgroundImage", "mainthread__set_loopback_mode",
        "invalidate", "close", "identity"
    ]);
    assert.equal(f.context.g_autoconnect_details.host, "replacement.invalid");
    assert.equal(f.targets.length, 0);
    console.log("PASS: manual Android settings update presentation before replacing the endpoint and preserve connection ordering.");
}

{
    const f = fixture([]);
    const events = [];
    f.context.main__apply_theme_for_app_mode = function()
    {
        events.push("theme");
    };
    f.context.document.getElementById = function()
    {
        events.push("presentation");
        return {style: {}};
    };
    assert.throws(function()
    {
        f.context.acceptSettings(JSON.stringify(
        {
            host: "replacement.invalid", port: 9001,
            metadata_keys: [{type: "unsupported", password: "synthetic"}]
        }));
    }, /Invalid connection password or cipher type/);
    assert.deepEqual(events, []);
    assert.equal(f.context.g_autoconnect_details.host, "synthetic.invalid");
    assert.equal(f.closes.length, 0);
    assert.equal(f.targets.length, 0);
    console.log("PASS: invalid connection credentials do not apply presentation or replace the endpoint.");
}
