"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const crypto = require("node:crypto");
const root = path.resolve(__dirname, "../../../browser");
const read = function(name)
{
    return fs.readFileSync(path.join(root, "main", name), "utf8");
};
const output = [];
const context = vm.createContext(
{
    console,
    performance,
    TextEncoder,
    TextDecoder,
    Uint8Array,
    WebAssembly,
    crypto: crypto.webcrypto,
    setTimeout,
    clearTimeout,
    setInterval,
    clearInterval,
    g_loopback_port: 0,
    g_is_authenticated: true,
    g_historic_keys_of_current_channel: [],
    DBG_WORKER_BOOT_LOG: false,
    atob(value)
    {
        return Buffer.from(value, "base64").toString("binary");
    },
    btoa(value)
    {
        return Buffer.from(value, "binary").toString("base64");
    },
    utils__bytesToBase64String(value)
    {
        return Buffer.from(value).toString("base64");
    },
    utils__base64StringToBytes(value)
    {
        return Uint8Array.from(Buffer.from(value, "base64"));
    },
    utils__custom_log()
    {
    },
    utils__substringByNullTerminator(value)
    {
        return value.split("\0", 1)[0];
    },
    postMessage(message)
    {
        output.push(message);
    }
});
context.global = context;
context.window = context;
context.self = context;
vm.runInContext(read("scripts/vendor/aes-js.js") + "\n}));", context);
vm.runInContext(read("scripts/vendor/sha256.js"), context);
vm.runInContext(read("scripts/vendor/js-sha256.js"), context);
vm.runInContext(read("scripts/app/crypto/rsa-crypto.js").replace("'@@WASM:wasm/rsa_keygen.wasm@@'",
    JSON.stringify(fs.readFileSync(path.join(root, "main/wasm/rsa_keygen.wasm")).toString("base64"))), context);
vm.runInContext(read("scripts/app/crypto/keys.js"), context);
vm.runInContext(read("scripts/app/workers/workers.js"), context);
const vector = JSON.parse(fs.readFileSync(path.join(root, "client-webassemblies/rsa-keygen/test/vectors-2048.json"), "utf8"))[0];
const key = new context.lemon_crypto.RSAKey();
key.setPrivateEx(vector.n, vector.e, vector.d, vector.p, vector.q, vector.dmp1, vector.dmq1, vector.coeff);
context.g_my_rsa_key_object = key;
context.g_rsa_public_key_string = context.lemon_crypto.publicKeyString(key);
context.g_current_channel_keys = context.keys__create_random_message_keys();
context.keys__decrypt_message_metadata = function(value)
{
    // This fixture starts after transport metadata decryption; content crypto below is real production code.
    return value;
};

const message = JSON.stringify({type: "direct_chat_message", value: JSON.stringify({value: "Private café 😀"})});
const directKeys = context.keys__create_random_message_keys();
const directEnvelope = context.keys__seal_direct_content(context.g_rsa_public_key_string, directKeys,
    context.keys__encrypt_direct_content(directKeys, message));
assert.ok(directEnvelope);
assert.equal(JSON.stringify(directEnvelope).includes("Private"), false);
for (const type of ["direct_chat_message", "offline_chat_message"])
{
    context.workers__mainthread__process_received_websocket_message_continue(
    {
        data: {value: JSON.stringify({message: {type, purpose: "user_message", value: JSON.stringify(directEnvelope)}})}
    });
    assert.equal(output.at(-1).type, "data_processing_worker__" + type);
    assert.equal(output.at(-1).value.message.some_json.value, JSON.stringify({value: "Private café 😀"}));
}
const channelPlaintext = JSON.stringify({value: "Channel café 😀"});
const channelEnvelope = context.keys__seal_channel_content(context.g_current_channel_keys,
    context.keys__encrypt_with_aes_keys_and_convert_to_base64(context.g_current_channel_keys, channelPlaintext));
assert.ok(channelEnvelope);
assert.equal(channelEnvelope.includes("Channel"), false);
context.workers__mainthread__process_received_websocket_message_continue(
{
    data: {value: JSON.stringify({message: {type: "channel_chat_message", value: channelEnvelope}})}
});
assert.equal(output.at(-1).type, "data_processing_worker__channel_chat_message");
assert.equal(output.at(-1).value.message.decrypted_value, channelPlaintext);
console.log("PASS actual RSA/AES private, offline and channel contents remain encrypted on wire and pass the new provenance gate after decryption");
