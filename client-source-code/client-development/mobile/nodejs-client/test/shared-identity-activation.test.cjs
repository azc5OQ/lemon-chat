"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const root = path.resolve(__dirname, "../../../browser/main/scripts/app");
const source = function(name)
{
    return fs.readFileSync(path.join(root, name), "utf8");
};
const actions = [];
const storage = new Map();
const context = vm.createContext({});
vm.runInContext(source("globals.js"), context);
Object.assign(context,
{
    console,
    TextEncoder,
    TextDecoder,
    Uint8Array,
    g_identity_slot: null,
    G_ALLOWED_RSA_KEY_BITS: [2048, 3072, 4096, 6144, 8192],
    g_rsa_key_bits: 8192,
    g_is_authenticated: true,
    g_is_rsa_key_generated: true,
    g_is_identity_switch_in_progress: false,
    g_is_identity_import_pending: true,
    g_is_autoconnect_without_user_action_active: false,
    g_rsa_public_key_string: "old-public",
    g_identity_string: "old-seed",
    g_worker_connection_generation: 3,
    g_worker_connection_ended: false,
    g_node_frame_listener: null,
    DBG_WORKER_BOOT_LOG: false,
    G_HAS_DOM: false,
    g_connection_status: {state: "connected"},
    g_data_processing_worker:
    {
        postMessage(message)
        {
            actions.push(message);
        }
    },
    document:
    {
        getElementById()
        {
            return {style: {}};
        }
    },
    window: {__SERVER_CONFIG__: {persist_identity: true}},
    utils__storage_set(key, value)
    {
        storage.set(key, value);
    },
    utils__custom_alert()
    {
    },
    utils__custom_log()
    {
    },
    sounds__play()
    {
    },
    android_host__is_ui_only_runtime()
    {
        return false;
    }
});
vm.runInContext(source("crypto/identity-file.js"), context);
vm.runInContext(source("connection/connection.js"), context);
vm.runInContext(source("msg/dispatch.js"), context);
context.connection__cancel_upload_send = function()
{
};
context.connection__reset_chat_app_keep_identity = function()
{
    actions.push({type: "close-and-invalidate-session"});
};
context.connection__report_connection_status = function(state)
{
    context.g_connection_status.state = state;
};
context.connection__refresh_connect_window_sections = function()
{
};
context.connection__set_connect_button_pending = function()
{
};

function generate(seed, bits, revision)
{
    context.connection__request_identity(seed.repeat(200), bits, revision);
}

function result(request, failure = false)
{
    context.dispatch__mainthread_onmessage(
    {
        data:
        {
            type: failure ? "data_processing_worker__generate_rsa_keypair_failed" : "data_processing_worker__generate_rsa_keypair_result",
            identity_request_id: request.identity_request_id,
            identity_revision: request.identity_revision,
            rsa_key_bits: request.rsa_key_bits,
            identity_string: request.identity_passphrase_string,
            value: "public-" + request.identity_revision
        }
    });
}

generate("A", 2048, 1);
const requestA = actions.find(function(action)
{
    return action.type === "mainthread__generate_rsa_keypair";
});
assert.equal(actions[0].type, "close-and-invalidate-session", "An authenticated old session is invalidated before posting key generation");
generate("B", 3072, 2);
generate("C", 4096, 3);
assert.equal(actions.filter(function(action)
{
    return action.type === "mainthread__generate_rsa_keypair";
}).length, 1);
assert.equal(context.identity_pending_request.passphrase, "C".repeat(200));
result(requestA);
const requests = actions.filter(function(action)
{
    return action.type === "mainthread__generate_rsa_keypair";
});
assert.equal(requests.length, 2);
assert.equal(requests[1].identity_passphrase_string, "C".repeat(200));
assert.equal(context.g_identity_slot.is_set, false);
assert.equal(context.g_is_rsa_key_generated, false);
assert.equal(context.g_identity_string, "old-seed");
result(requestA, true);
assert.equal(context.identity_active_request.id, requests[1].identity_request_id);
assert.equal(context.identity_requested_for, "C".repeat(200));
result(requests[1]);
assert.equal(context.g_identity_slot.is_set, true);
assert.equal(context.g_identity_string, "C".repeat(200));
assert.equal(context.g_identity_rsa_key_bits, 4096);
assert.equal(context.g_rsa_key_bits, 8192, "Selected identity must not overwrite the next-key preference");
assert.equal(JSON.parse(storage.get("lemon_identity_record_v1")).rsa_key_bits, 4096);
assert.equal(JSON.parse(storage.get("lemon_identity_record_v1")).passphrase, "C".repeat(200));
result(requestA);
assert.equal(context.g_identity_string, "C".repeat(200));

generate("C", 4096, 4);
const revisionRequest = actions.filter(function(action)
{
    return action.type === "mainthread__generate_rsa_keypair";
}).at(-1);
assert.equal(revisionRequest.identity_revision, 4, "A new canonical revision cannot be skipped as a duplicate");
result(revisionRequest, true);
assert.equal(context.g_identity_slot.is_set, false);
assert.equal(context.g_is_rsa_key_generated, false);
assert.equal(context.identity_requested_for, null);
generate("C", 4096, 4);
const retry = actions.filter(function(action)
{
    return action.type === "mainthread__generate_rsa_keypair";
}).at(-1);
assert.notEqual(retry.identity_request_id, revisionRequest.identity_request_id);
result(revisionRequest, true);
assert.equal(context.identity_active_request.id, retry.identity_request_id);
result(retry);
assert.equal(context.g_identity_revision, 4);

const saved = context.identity_file__read_saved(storage.get("lemon_identity_record_v1"), "old".repeat(200));
assert.equal(saved.rsa_key_bits, 4096);
assert.equal(saved.passphrase, "C".repeat(200));
assert.equal(context.identity_file__read_saved(null, "legacy".repeat(40)).kind, "recovery");
assert.equal(context.identity_file__read_saved("bad-json", "legacy".repeat(40)).kind, "recovery");
assert.equal(context.identity_file__read_saved(null, "short").kind, "recovery");
assert.equal(context.identity_file__read_saved(null, null).kind, "missing");
console.log("PASS A/B/C generation coalescing, stale success/failure rejection, close-before-post, revision retry, selected-size persistence and non-destructive legacy recovery");

const nodes = new Map();
context.document.getElementById = function(id)
{
    if (!nodes.has(id))
    {
        nodes.set(id,
        {
            style: {},
            value: "",
            attributes: {},
            setAttribute(name, value)
            {
                this.attributes[name] = value;
            },
            getAttribute(name)
            {
                return this.attributes[name];
            }
        });
    }
    return nodes.get(id);
};
context.crypto = require("node:crypto").webcrypto;
context.android_host__is_ui_only_runtime = function()
{
    return true;
};
context.g_identity_revision = 7;
context.g_identity_rsa_key_bits = 2048;
context.g_loopback_port = 0;
const nativeCalls = [];
context.Android =
{
    JavaExportReplaceIdentity(seed, bits, operation, revision)
    {
        nativeCalls.push({seed, bits, operation, revision});
        return JSON.stringify({ok: true, identity_revision: 8});
    },
    JavaExportResizeIdentity(bits, operation, revision)
    {
        nativeCalls.push({bits, operation, revision});
        return JSON.stringify({ok: true, identity_revision: 9});
    },
    JavaExportExportIdentity(filename)
    {
        nativeCalls.push({filename});
        return JSON.stringify({ok: true});
    }
};
vm.runInContext(source("ui/ui.js"), context);
context.UI.identity_apply_passphrase(" A".repeat(100), 3072);
assert.equal(nativeCalls[0].seed, " A".repeat(100));
assert.equal(nativeCalls[0].bits, 3072);
assert.equal(nativeCalls[0].revision, 7);
assert.match(nativeCalls[0].operation, /^[a-f0-9]{32}$/);
assert.equal(context.g_identity_import_expected_revision, 8);
assert.equal(context.g_is_identity_import_pending, true);
context.dispatch__mainthread_onmessage({data: {type: "data_processing_worker__local_identity_result",
    value: {identity_revision: 7, success: false}}});
assert.equal(context.g_is_identity_import_pending, true);
context.dispatch__mainthread_onmessage({data: {type: "data_processing_worker__local_identity_result",
    value: {identity_revision: 8, success: true, public_key_string: "native-key", rsa_key_bits: 3072}}});
assert.equal(context.g_is_identity_import_pending, false);
assert.equal(nodes.get("another-buttons-sub-loading-container").style.display, "none");
context.g_identity_string = "";
context.UI.export_identity_file_button_onclick({stopPropagation()
{
}});
assert.ok(nativeCalls.at(-1).filename.endsWith(".lmn"));
context.g_identity_rsa_key_bits = 2048;
context.g_rsa_key_too_weak_prompted_for_bits = 0;
vm.runInContext(source("crypto/keys.js"), context);
context.keys__handle_rsa_key_too_weak_notice(3072);
assert.equal(nodes.get("rsa-key-too-weak-container").style.display, "block");
assert.match(nodes.get("rsa-key-too-weak-text").textContent, /Your key is 2048 bits/);
const main = source("main.js");
const yesBegin = main.indexOf('    document.getElementById("rsa-key-too-weak-yes-button").onclick');
const yesEnd = main.indexOf('    document.getElementById("local-settings-seen-indicator")', yesBegin);
let reconnects = 0;
context.connection__request_connect = function()
{
    reconnects++;
};
vm.runInContext(main.slice(yesBegin, yesEnd), context);
const yes = nodes.get("rsa-key-too-weak-yes-button");
yes.onclick.call(yes);
assert.equal(nativeCalls.at(-1).bits, 3072);
assert.equal("seed" in nativeCalls.at(-1), false, "Resize keeps the native seed out of the page");
assert.equal(reconnects, 1);
assert.equal(context.g_rsa_key_bits, 8192);
console.log("PASS early Android import/export, matching ready-only completion and explicit too-weak native resize use the canonical identity without page key generation");
