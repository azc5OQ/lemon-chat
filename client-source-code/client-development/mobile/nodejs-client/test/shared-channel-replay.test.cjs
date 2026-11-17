"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const crypto = require("node:crypto");
const root = path.resolve(__dirname, "../../../browser/main");
const outputs = [];
function read(relative)
{
    return fs.readFileSync(path.join(root, relative), "utf8");
}
function noop()
{
}
const context = vm.createContext(
{
    console, performance, TextEncoder, TextDecoder, Uint8Array, atob, btoa,
    crypto: crypto.webcrypto, setTimeout, clearTimeout, clearInterval,
    g_loopback_port: 0, g_is_authenticated: true, DBG_WORKER_BOOT_LOG: false,
    g_historic_keys_of_current_channel: [], utils__custom_log: noop,
    utils__bytesToBase64String(value)
    {
        return Buffer.from(value).toString("base64");
    },
    utils__base64StringToBytes(value)
    {
        return Uint8Array.from(Buffer.from(value, "base64"));
    },
    utils__substringByNullTerminator(value)
    {
        return value.split("\0", 1)[0];
    },
    postMessage(message)
    {
        outputs.push(message);
    }
});
context.global = context;
context.window = context;
context.self = context;
vm.runInContext(read("scripts/vendor/aes-js.js") + "\n}));", context);
for (const file of ["scripts/vendor/sha256.js", "scripts/vendor/js-sha256.js", "scripts/app/crypto/keys.js", "scripts/app/chat/chat-files.js", "scripts/app/workers/workers.js"])
{
    vm.runInContext(read(file), context);
}
const sanitize = read("scripts/app/chat/chat.js").match(/^function chat__sanitize_image_data_url\([^\n]*\)\r?\n\{[\s\S]*?^\}/m);
vm.runInContext(sanitize[0], context);
context.keys__decrypt_message_metadata = function(value)
{
    return value;
};
context.g_current_channel_keys = context.keys__create_random_message_keys();
context.dpw_forward_frames = true;

function receive(frame, local = false)
{
    context.workers__mainthread__process_received_websocket_message_continue({data: {is_plaintext: local, value: JSON.stringify(frame)}});
}
function seal(text, file)
{
    return context.keys__seal_channel_content(context.g_current_channel_keys, file
        ? context.chat_files__encrypt_string_with_aes_keys_fast(context.g_current_channel_keys, text)
        : context.keys__encrypt_with_aes_keys_and_convert_to_base64(context.g_current_channel_keys, text));
}
const text = JSON.stringify({value: "retained café 😀"});
const picture = "data:image/png;base64,AA==";
const file = {name: "notes.txt", size: 5, mime: "text/plain", base64: "aGVsbG8="};
const encryptedText = seal(text, false);
const encryptedHeader = seal(context.chat_files__build_chat_file_header_json(file), true);
const encryptedFile = seal(context.chat_files__build_chat_file_body_json(file), true);
const encryptedPicture = seal(picture, false);
receive({message: {type: "channel_chat_message", sender_id: 7, channel_id: 2, value: encryptedText}});
receive({message: {type: "channel_chat_file_metadata", sender_id: 7, channel_id: 2, file_id: 42, file_header: encryptedHeader}});
receive({message: {type: "file_receive_completed", receive_type: "channel_chat_file", server_chat_message_id: 42, sender_id: 7}});
for (const kind of ["file", "picture"])
{
    context.workers__data_processing_worker_onmessage({data:
    {
        type: "mainthread__process_encrypted_channel_chat_" + kind + "_data",
        [kind + "_id"]: 42,
        sender_id: 7,
        message_raw: kind === "file" ? encryptedFile : encryptedPicture
    }});
}
const prepared = outputs.filter(function(message)
{
    return message.type === "decrypted_frame";
}).map(function(message)
{
    return JSON.parse(message.value);
});
assert.deepEqual(prepared.map(function(frame)
{
    return frame.message.type;
}), ["channel_chat_message", "channel_chat_file_metadata", "channel_chat_file_decrypted", "channel_chat_picture_decrypted"]);
assert.ok(outputs.some(function(message)
{
    return message.type === "data_processing_worker__file_receive_completed_from_server";
}), "The owner consumes raw completion while only its prepared terminal is forwarded to the page");
assert.equal(prepared[0].message.value, null);
assert.equal(prepared[0].message.decrypted_value, text);
assert.equal(prepared[1].message.file_header, null);
assert.equal(prepared[1].message.file_header_decrypted.name, file.name);

// The replacement page knows only the newest keys. The old ciphertext demonstrably cannot open.
context.g_current_channel_keys = context.keys__create_random_message_keys();
context.g_historic_keys_of_current_channel = [];
context.chat_file_keys_by_file_id = {};
assert.equal(context.keys__decrypt_channel_contents(encryptedText, false), null);
assert.equal(context.keys__decrypt_channel_contents(encryptedPicture, true), null);
assert.equal(context.chat_files__decrypt_channel_chat_file_header(42, encryptedHeader), null);
assert.equal(context.chat_files__decrypt_channel_chat_file_body(42, encryptedFile), null);
context.g_loopback_port = 1234;
context.dpw_local_session_id = "replacement";
context.dpw_forward_frames = false;
outputs.length = 0;
for (let index = 0; index < prepared.length; index++)
{
    prepared[index].local_delivery = {id: String(index), session_id: "replacement"};
    receive(prepared[index], true);
}
const applied = outputs.filter(function(message)
{
    return message.type !== "data_processing_worker__local_delivery_accepted" && message.type !== "data_processing_worker__connection_activity";
});
assert.deepEqual(applied.map(function(message)
{
    return message.type;
}), ["data_processing_worker__channel_chat_message", "data_processing_worker__channel_chat_file_metadata", "data_processing_worker__channel_chat_file", "data_processing_worker__channel_chat_picture"]);
assert.equal(applied[0].value.message.decrypted_value, text);
assert.equal(applied[1].value.message.file_header_decrypted.name, file.name);
assert.equal(applied[2].value.message.file.base64, file.base64);
assert.equal(applied[3].value.message.decrypted_base64_picture, picture);
console.log("PASS: actual AES channel text/header/file/picture survives page replacement after its old keys are unavailable.");

// The same prepared shapes and stable sender field are never accepted from the remote server.
context.g_loopback_port = 0;
context.dpw_forward_frames = true;
outputs.length = 0;
for (const frame of prepared)
{
    delete frame.local_delivery;
    receive(frame);
}
const currentEncryptedText = seal(text, false);
receive({local_sender: {client_id: 7, public_key: "forged"}, message: {type: "channel_chat_message", value: currentEncryptedText}});
assert.equal(outputs.length, 0);
receive({message: {type: "channel_chat_message", value: currentEncryptedText}});
assert.ok(outputs.some(function(message)
{
    return message.type === "decrypted_frame";
}), "A normal current encrypted channel message still passes this same remote path");
outputs.length = 0;
context.g_loopback_port = 1234;
context.dpw_forward_frames = false;
receive({local_sender: {client_id: 7, public_key: "forged"}, message: {type: "channel_chat_message", value: currentEncryptedText}}, true);
assert.equal(outputs.length, 0, "A sender snapshot without current local delivery scope is rejected");
console.log("PASS: remote prepared channel packets and forged retained-sender provenance are rejected before forwarding.");
