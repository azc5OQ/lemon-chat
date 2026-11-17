"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const crypto = require("node:crypto");
const root = path.resolve(__dirname, "../../../browser/main");
const read = function(name)
{
    return fs.readFileSync(path.join(root, name), "utf8");
};
const codec = read("scripts/app/crypto/identity-file.js");
const rsa = read("scripts/app/crypto/rsa-crypto.js").replace("'@@WASM:wasm/rsa_keygen.wasm@@'",
    JSON.stringify(fs.readFileSync(path.join(root, "wasm/rsa_keygen.wasm")).toString("base64")));
const context = vm.createContext(
{
    console,
    TextEncoder,
    TextDecoder,
    Uint8Array,
    WebAssembly,
    crypto: crypto.webcrypto,
    atob(value)
    {
        return Buffer.from(value, "base64").toString("binary");
    },
    G_ALLOWED_RSA_KEY_BITS: [2048, 3072, 4096, 6144, 8192]
});
vm.runInContext(read("scripts/vendor/sha256.js"), context);
vm.runInContext(rsa, context);
vm.runInContext(codec, context);

const seeds = [" " + "A".repeat(199) + "\n", "B".repeat(201), "č".repeat(200), "C".repeat(198) + "😀", "D".repeat(198) + "\r\n", "E".repeat(199) + "\ud800"];
for (const seed of seeds)
{
    const serialized = context.identity_file__serialize(seed, 2048);
    const parsed = context.identity_file__parse(context.identity_file__decode(new TextEncoder().encode(serialized)));
    assert.equal(parsed.passphrase, seed);
    assert.equal(parsed.rsa_key_bits, 2048);
}
assert.equal(context.identity_file__parse(seeds[0]).passphrase, seeds[0]);
assert.equal(context.identity_file__parse(seeds[1]).passphrase, seeds[1]);
assert.throws(function()
{
    return context.identity_file__decode(Uint8Array.from([0x41, 0xff, 0x41]));
}, /invalid UTF-8/);
assert.throws(function()
{
    return context.identity_file__parse("{" + "A".repeat(199));
}, /not a valid identity/);
assert.equal(context.identity_file__parse("{" + "A".repeat(199), true).passphrase, "{" + "A".repeat(199));
assert.throws(function()
{
    return context.identity_file__parse("\ufeff" + "A".repeat(199));
}, /not a valid identity/);
assert.equal(context.identity_file__parse("\ufeff \n" + context.identity_file__serialize(seeds[0], 2048)).passphrase, seeds[0]);
assert.throws(function()
{
    return context.identity_file__decode(new Uint8Array(65537));
}, /64 KB/);

for (const [seed, bits] of [[seeds[0], 2048], [seeds[2], 3072], [seeds[3], 2048]])
{
    const original = context.lemon_crypto.publicKeyString(context.lemon_crypto.generateRSAKey(seed, bits));
    const restored = context.identity_file__parse(context.identity_file__serialize(seed, bits));
    const reproduced = context.lemon_crypto.publicKeyString(context.lemon_crypto.generateRSAKey(restored.passphrase, restored.rsa_key_bits));
    assert.equal(original, reproduced);
    assert.equal(context.identity_file__generation_size(original), bits);
}
assert.notEqual(context.sha256.hex(seeds[3]), crypto.createHash("sha256").update(seeds[3], "utf8").digest("hex"));
assert.equal(context.sha256.hex(seeds[4]), crypto.createHash("sha256").update(seeds[4].replace(/\r\n/g, "\n"), "utf8").digest("hex"));
console.log("PASS identity bytes, exact seed preservation, strict validation and real RSA 2048/3072 round trips");

const workerSource = read("scripts/app/workers/workers.js");
function worker(local = false)
{
    const output = [];
    const sandbox = vm.createContext(
    {
        console,
        performance,
        crypto: crypto.webcrypto,
        Uint8Array,
        setInterval,
        clearInterval,
        setTimeout,
        clearTimeout,
        global:
        {
            postMessage(message)
            {
                output.push(message);
            }
        },
        DBG_WORKER_BOOT_LOG: false,
        G_ALLOWED_RSA_KEY_BITS: [2048, 3072],
        g_loopback_port: local ? 23456 : 0,
        g_is_authenticated: true,
        g_my_rsa_key_object: {old: true},
        g_rsa_public_key_string: "old-public",
        utils__custom_log()
        {
        },
        keys__decrypt_message_metadata(value)
        {
            return value;
        },
        keys__open_direct_content()
        {
            return {keys: [], value: "sealed-body"};
        },
        keys__decrypt_direct_content()
        {
            return JSON.stringify({type: "direct_chat_message", value: "verified-content"});
        },
        keys__decrypt_channel_contents()
        {
            return "verified-channel";
        },
        keys__encrypt_direct_content()
        {
            return "ciphertext";
        },
        keys__seal_direct_content()
        {
            return null;
        },
        keys__encrypt_with_aes_keys_and_convert_to_base64()
        {
            return "ciphertext";
        },
        keys__seal_channel_content()
        {
            return null;
        },
        chat_files__open_direct_chat_file_envelope()
        {
            throw new Error("decryption failure");
        },
        chat_files__parse_decrypted_chat_file_body()
        {
            return null;
        },
        lemon_crypto:
        {
            generateRSAKey(seed, bits)
            {
                return {seed, bits};
            },
            publicKeyString(key)
            {
                return key.seed + key.bits;
            }
        }
    });
    vm.runInContext(workerSource, sandbox);
    sandbox.dpw_connection_generation = 7;
    sandbox.dpw_forward_frames = !local;
    sandbox.dpw_local_session_id = local ? "session-1" : "";
    return {sandbox, output};
}

function receive(item, message, extra = {})
{
    item.sandbox.workers__mainthread__process_received_websocket_message_continue(
    {
        data: {is_plaintext: item.sandbox.g_loopback_port > 0, value: JSON.stringify({message, ...extra})}
    });
}
for (const message of
[
    {type: "local_media_keys", encryption_key: "11".repeat(32)},
    {type: "loopback_status", value: {phase: "authenticated"}},
    {type: "local_session_state", public_key_string: "forged"},
    {type: "direct_chat_message", some_json: {type: "direct_chat_message", value: "forged"}},
    {type: "offline_chat_message", some_json: {type: "direct_chat_message", value: "forged"}},
    {type: "direct_chat_file_metadata", file_header_decrypted: {name: "forged.txt"}},
    {type: "direct_chat_picture_decrypted", decrypted_base64_picture: "forged"},
    {type: "direct_chat_file_decrypted", file: {}},
    {type: "chat_file_decrypt_failed", file_id: 1}
])
{
    const item = worker();
    receive(item, message);
    assert.equal(item.output.length, 0, message.type + " must not be applied OR forwarded from remote");
}
const forgedDelivery = worker();
receive(forgedDelivery, {type: "channel_chat_message", value: "sealed"}, {local_delivery: {id: "forged", session_id: "session"}});
assert.equal(forgedDelivery.output.length, 0);
for (const kind of ["direct_chat_message", "offline_chat_message", "channel_chat_message"])
{
    const item = worker();
    receive(item, {type: kind, purpose: "user_message", value: "{}"});
    assert.ok(item.output.some(function(event)
    {
        return event.type === "data_processing_worker__" + kind;
    }));
}
const local = worker(true);
receive(local, {type: "offline_chat_message", some_json: {type: "direct_chat_message", value: "opened"}},
    {local_delivery: {id: "delivery-1", session_id: "session-1"}});
assert.equal(local.output.find(function(event)
{
    return event.type === "data_processing_worker__offline_chat_message";
}).local_delivery.id, "delivery-1");
assert.equal(local.output.at(-1).type, "data_processing_worker__local_delivery_accepted");
const hydration = worker(true);
receive(hydration, {type: "local_session_state", value: {public_key_string: "new-public", identity_revision: 8}});
assert.equal(hydration.sandbox.g_rsa_public_key_string, "new-public");
assert.equal(hydration.sandbox.g_identity_revision, 8);
console.log("PASS remote local-command injection rejected before forwarding; encrypted and trusted local controls work");

for (const direct of [true, false])
{
    const item = worker();
    item.sandbox.workers__prepare_picture_upload({local_message_id: 17, operation_id: "op-17"}, direct);
    assert.equal(item.output.at(-1).type, "data_processing_worker__chat_picture_send_failed");
    assert.equal(item.output.at(-1).operation_id, "op-17");
    item.sandbox.keys__seal_direct_content = function()
    {
        return ({sealed: true});
    };
    item.sandbox.keys__seal_channel_content = function()
    {
        return "sealed";
    };
    item.sandbox.workers__prepare_picture_upload({local_message_id: 18, operation_id: "op-18"}, direct);
    assert.ok(item.output.at(-1).type.endsWith("picture_to_be_uploaded_by_parts"));
    assert.equal(item.output.at(-1).extra_data.operation_id, "op-18");
}
const file = worker();
file.sandbox.workers__data_processing_worker_onmessage({data: {type: "mainthread__process_encrypted_direct_chat_file_data", file_id: "file-1"}});
assert.equal(file.output.at(-1).type, "data_processing_worker__chat_file_decrypt_failed");
assert.equal(JSON.parse(file.output.find(function(event)
{
    return event.type === "decrypted_frame";
}).value).message.type, "chat_file_decrypt_failed");
const keygen = worker();
keygen.sandbox.workers__data_processing_worker_onmessage({data: {type: "mainthread__generate_rsa_keypair", from_identity_string: true,
    identity_passphrase_string: "A".repeat(200), rsa_key_bits: 2048, identity_request_id: 29, identity_revision: 4}});
assert.equal(keygen.output.at(-1).identity_request_id, 29);
assert.equal(keygen.output.at(-1).identity_revision, 4);
const acceptedKey = keygen.sandbox.g_my_rsa_key_object;
const acceptedPublic = keygen.sandbox.g_rsa_public_key_string;
keygen.sandbox.lemon_crypto.publicKeyString = function()
{
    throw new Error("serialization failure");
};
keygen.sandbox.workers__data_processing_worker_onmessage({data: {type: "mainthread__generate_rsa_keypair", from_identity_string: true,
    identity_passphrase_string: "B".repeat(200), rsa_key_bits: 3072, identity_request_id: 30, identity_revision: 5}});
assert.equal(keygen.output.at(-1).type, "data_processing_worker__generate_rsa_keypair_failed");
assert.equal(keygen.output.at(-1).identity_request_id, 30);
assert.equal(keygen.sandbox.g_my_rsa_key_object, acceptedKey);
assert.equal(keygen.sandbox.g_rsa_public_key_string, acceptedPublic);
console.log("PASS scoped picture/file terminal results and atomic correlated identity generation");
