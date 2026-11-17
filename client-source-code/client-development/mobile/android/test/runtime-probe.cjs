"use strict";

const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const crypto = require("node:crypto");
const assert = require("node:assert/strict");
const {createRequire} = require("node:module");
const {once} = require("node:events");
const fixtures = process.argv[2];
const project = process.argv[3];
const resultFile = process.argv[4];
let checks = 0;

function equal(actual, expected)
{
    assert.equal(actual, expected);
    checks++;
}

async function verify()
{
    const pin = JSON.parse(fs.readFileSync(path.join(fixtures, "nodejs-mobile-runtime.json"), "utf8"));
    equal(process.version, "v" + pin.upstream_version);
    if (process.platform === "android")
    {
        equal(process.versions.mobile, pin.version);
    }
    const read = function(name)
    {
        return fs.readFileSync(path.join(fixtures, "browser/main", name), "utf8");
    };
    const messages = [];
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
        G_ALLOWED_RSA_KEY_BITS: [2048, 3072, 4096, 6144, 8192],
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
            messages.push(message);
        }
    });
    context.global = context;
    context.window = context;
    context.self = context;
    vm.runInContext(read("scripts/vendor/aes-js.js") + "\n}));", context);
    vm.runInContext(read("scripts/vendor/sha256.js"), context);
    vm.runInContext(read("scripts/vendor/js-sha256.js"), context);
    const wasm = fs.readFileSync(path.join(fixtures, "browser/main/wasm/rsa_keygen.wasm")).toString("base64");
    vm.runInContext(read("scripts/app/crypto/rsa-crypto.js").replace("'@@WASM:wasm/rsa_keygen.wasm@@'", JSON.stringify(wasm)), context);
    vm.runInContext(read("scripts/app/crypto/identity-file.js"), context);
    vm.runInContext(read("scripts/app/crypto/keys.js"), context);
    vm.runInContext(read("scripts/app/workers/workers.js"), context);
    const vector = JSON.parse(fs.readFileSync(path.join(fixtures, "browser/client-webassemblies/rsa-keygen/test/vectors-2048.json"), "utf8"))[0];
    const baseline = JSON.parse(fs.readFileSync(path.join(fixtures, "node18-identity-vectors.json"), "utf8"));
    const cases =
    [
        {passphrase: vector.passphrase, bits: vector.bits},
        {passphrase: "caf\u00e9 \u0130 \uD83D\uDE00 ".repeat(24), bits: 2048},
        {passphrase: "caf\u00e9 \u0130 \uD83D\uDE00 ".repeat(24), bits: 3072},
        {passphrase: "\uFEFF" + "identity\r\nline\n".repeat(16), bits: 2048}
    ];
    for (let index = 0; index < cases.length; index++)
    {
        const entry = cases[index];
        const exported = context.identity_file__serialize(entry.passphrase, entry.bits);
        const parsed = context.identity_file__parse(context.identity_file__decode(new TextEncoder().encode(exported)));
        equal(parsed.passphrase, entry.passphrase);
        equal(parsed.rsa_key_bits, entry.bits);
        const key = context.lemon_crypto.generateRSAKey(parsed.passphrase, parsed.rsa_key_bits);
        const fields = ["n", "e", "d", "p", "q", "dmp1", "dmq1", "coeff"];
        const values = fields.map(function(field)
        {
            return key[field].toString(16);
        });
        equal(crypto.createHash("sha256").update(values.join(":")).digest("hex"), baseline.cases[index].key_fingerprint);
        equal(crypto.createHash("sha256").update(exported).digest("hex"), baseline.cases[index].file_fingerprint);
        if (index === 0)
        {
            equal(context.lemon_crypto.publicKeyString(key), vector.public_key_string);
            context.g_my_rsa_key_object = key;
            context.g_rsa_public_key_string = context.lemon_crypto.publicKeyString(key);
        }
    }
    context.g_current_channel_keys = context.keys__create_random_message_keys();
    context.keys__decrypt_message_metadata = function(value)
    {
        return value;
    };
    const directText = JSON.stringify({type: "direct_chat_message", value: JSON.stringify({value: "Private café 😀"})});
    const keys = context.keys__create_random_message_keys();
    const sealed = context.keys__seal_direct_content(context.g_rsa_public_key_string, keys, context.keys__encrypt_direct_content(keys, directText));
    equal(JSON.stringify(sealed).includes("Private"), false);
    for (const type of ["direct_chat_message", "offline_chat_message"])
    {
        context.workers__mainthread__process_received_websocket_message_continue(
        {
            data: {value: JSON.stringify({message: {type, purpose: "user_message", value: JSON.stringify(sealed)}})}
        });
        equal(messages.at(-1).type, "data_processing_worker__" + type);
        equal(messages.at(-1).value.message.some_json.value, JSON.stringify({value: "Private café 😀"}));
    }
    const channelText = JSON.stringify({value: "Channel café 😀"});
    const channel = context.keys__seal_channel_content(context.g_current_channel_keys, context.keys__encrypt_with_aes_keys_and_convert_to_base64(context.g_current_channel_keys, channelText));
    equal(channel.includes("Channel"), false);
    context.workers__mainthread__process_received_websocket_message_continue(
    {
        data: {value: JSON.stringify({message: {type: "channel_chat_message", value: channel}})}
    });
    equal(messages.at(-1).type, "data_processing_worker__channel_chat_message");
    equal(messages.at(-1).value.message.decrypted_value, channelText);

    const builtin = globalThis.WebSocket;
    equal(typeof builtin, "function");
    const requireProject = createRequire(path.join(project, "load-bundle.js"));
    const loader = vm.createContext({globalThis, require: requireProject, module: {exports: {}}, __dirname: project});
    vm.runInContext(fs.readFileSync(path.join(project, "load-bundle.js"), "utf8"), loader);
    vm.runInContext("install_websocket_polyfill()", loader);
    const adapter = requireProject("./websocket-adapter.js");
    equal(globalThis.WebSocket, adapter.WebSocketClient);
    equal(globalThis.WebSocket === builtin, false);
    const WebSocketServer = requireProject("./vendor/ws").WebSocketServer;
    const server = new WebSocketServer({host: "127.0.0.1", port: 0});
    await once(server, "listening");
    let peer;
    let client;
    const deadline = setTimeout(function()
    {
        if (client)
        {
            client.close();
        }
        if (peer)
        {
            peer.terminate();
        }
        server.close();
    }, 5000);
    try
    {
        server.on("connection", function(connection)
        {
            peer = connection;
            peer.once("message", function(data)
            {
                peer.send(data.toString());
            });
        });
        client = new globalThis.WebSocket("ws://127.0.0.1:" + server.address().port);
        const packet = JSON.stringify({type: "runtime_probe", value: "café 😀"});
        const received = await new Promise(function(resolve, reject)
        {
            client.onopen = function()
            {
                client.send(packet);
            };
            client.onmessage = function(event)
            {
                resolve(event.data);
            };
            client.onerror = reject;
            client.onclose = function()
            {
                reject(new Error("Loopback closed before its reply"));
            };
        });
        equal(received, packet);
    }
    finally
    {
        clearTimeout(deadline);
        if (client)
        {
            client.close();
        }
        if (peer)
        {
            peer.terminate();
        }
        await new Promise(function(resolve)
        {
            server.close(resolve);
        });
    }
    return {checks, version: process.version, mobile: process.versions.mobile || null, platform: process.platform, architecture: process.arch};
}

verify().then(function(result)
{
    fs.writeFileSync(resultFile, JSON.stringify(result));
    console.log("PASS embedded runtime compatibility: " + checks + " checks");
}).catch(function(error)
{
    fs.writeFileSync(resultFile, JSON.stringify({error: error.stack || String(error)}));
    process.exitCode = 1;
});
