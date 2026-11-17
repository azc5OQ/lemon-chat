const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const crypto = require("node:crypto");
const assert = require("node:assert/strict");
const root = path.resolve(__dirname, "../../../browser/main");
const context = vm.createContext(
{
    Uint8Array, ArrayBuffer, DataView, Int32Array, Float32Array, WebAssembly, TextEncoder, TextDecoder,
    crypto: crypto.webcrypto,
    console,
    atob(value)
    {
        return Buffer.from(value, "base64").toString("binary");
    }
});
context.self = context;
context.global = context;
function load(relative, suffix = "")
{
    const source = fs.readFileSync(path.join(root, relative), "utf8").replace(/'@@WASM:([^@]+)@@'/g, function(match, binary)
    {
        return JSON.stringify(fs.readFileSync(path.join(root, binary)).toString("base64"));
    });
    vm.runInContext(source + suffix, context, {filename: relative});
}
load("scripts/vendor/aes-js.js", "\nreturn {}; }));");
load("scripts/vendor/sha256.js");
load("scripts/vendor/js-sha256.js");
load("scripts/app/crypto/connection-crypto.js");
load("scripts/app/crypto/keys.js");
load("scripts/app/audio/audio-transport.js");
load("scripts/app/audio/opus-runtime.js");
load("scripts/app/audio/audio-opus-glue.js");
context.create_opus_webassembly_instance();
context.opus_decoder_pool.push(new context.OpusDecoder(48000, 2));
context.opus_decoder_pool_free_indices.push(0);
context.g_metadata_keys = [];
context.g_media_server_key_layers_from_host = [];
context.g_session = {bytes_received: 0};
context.g_current_channel_id = 0;
context.g_local_client_id = 42;
context.g_client_list = [{client_id: 41, channel_id: 0}];
context.channel_tree__get_client_index_in_array_by_client_id = function(id)
{
    return id === 41 ? 0 : -1;
};
const key = Array.from(crypto.randomBytes(32));
const mac = Array.from(crypto.randomBytes(32));
context.g_media_keys_from_host = {key_bytes: key, mac_bytes: mac};
let received = null;
let pcm = null;
context.opus_decoder_direct_worklet_port =
{
    postMessage(message)
    {
        pcm = message.pcm;
    }
};
context.g_opus_decoder_worker =
{
    postMessage(message)
    {
        received = message;
        context.audio_opus_glue__opus_decoder_worker_onmessage({data: message});
    }
};
const fixtures = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
let energy = 0;
for (const fixture of fixtures)
{
    const counter = new Uint8Array(8);
    let value = BigInt(fixture.counter);
    for (let index = 7; index >= 0; index--)
    {
        counter[index] = Number(value & 255n);
        value >>= 8n;
    }
    const encrypted = new Uint8Array(Buffer.from(fixture.encrypted, "base64"));
    const expected = Buffer.from(fixture.plain, "base64");
    const opened = context.keys__media_apply_inner_layers(fixture.keys, fixture.sender, counter, encrypted, false);
    assert.deepEqual(Buffer.from(opened), expected, "browser channel decryption changed native Opus bytes");
    const rebuilt = context.keys__media_apply_inner_layers(fixture.keys, fixture.sender, counter, opened, true);
    assert.deepEqual(Buffer.from(rebuilt), Buffer.from(encrypted), "browser encryption differs from native encryption");
    const body = context.keys__media_build_body(fixture.sender, counter, encrypted);
    const sessionBlock = context.keys__media_session_block(counter, context.MEDIA_DIRECTION_FROM_SERVER, context.MEDIA_STREAM_AUDIO);
    const frame = new Uint8Array(9 + body.length + 8);
    frame[0] = 2;
    frame.set(counter, 1);
    frame.set(context.keys__media_session_crypt(context.g_media_keys_from_host, sessionBlock, body, true), 9);
    frame.set(context.keys___hmac_sha256(mac, frame.subarray(0, frame.length - 8)).slice(0, 8), frame.length - 8);
    context.g_current_channel_keys = fixture.keys;
    received = null;
    pcm = null;
    context.audio_transport__receive(frame.buffer);
    assert.ok(received, "browser transport did not deliver the native content");
    assert.equal(received.counter, fixture.counter, "worker counter lost precision");
    assert.deepEqual(Buffer.from(received.value).subarray(4), expected);
    assert.ok(pcm && pcm.length === 1920, "browser Wasm Opus decoder did not produce a stereo frame");
    for (const sample of pcm)
    {
        assert.ok(Number.isFinite(sample));
        energy += sample * sample;
    }
}
assert.ok(energy > 1, "browser decoded only silence");
context.opus_decoder_pool[0].destroy();
console.log("PASS: browser decrypts native Opus content at six full-width counter boundaries, preserves exact counter strings through its transport and worker, and decodes real stereo PCM with the browser Opus Wasm");
