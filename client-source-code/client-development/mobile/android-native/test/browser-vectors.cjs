const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const crypto = require("node:crypto");
const assert = require("node:assert/strict");
const root = path.resolve(__dirname, "../../../browser");
const read = function(relative)
{
    return fs.readFileSync(path.join(root, "main", relative), "utf8");
};
const context = vm.createContext(
{
    Uint8Array,
    ArrayBuffer,
    WebAssembly,
    TextEncoder,
    TextDecoder,
    crypto: crypto.webcrypto,
    G_ALLOWED_RSA_KEY_BITS: [2048, 3072, 4096, 6144, 8192],
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
        return new Uint8Array(Buffer.from(value, "base64"));
    },
    utils__substringByNullTerminator(value)
    {
        return value.split("\0")[0];
    }
});
context.self = context;
vm.runInContext(read("scripts/vendor/aes-js.js") + "\nreturn {}; }));", context);
vm.runInContext(read("scripts/vendor/sha256.js"), context);
vm.runInContext(read("scripts/vendor/js-sha256.js"), context);
vm.runInContext(read("scripts/app/crypto/rsa-crypto.js").replace("'@@WASM:wasm/rsa_keygen.wasm@@'", JSON.stringify(fs.readFileSync(path.join(root, "main/wasm/rsa_keygen.wasm")).toString("base64"))), context);
vm.runInContext(read("scripts/app/crypto/keys.js"), context);
vm.runInContext(read("scripts/app/chat/chat-files.js"), context);
const stored = JSON.parse(fs.readFileSync(path.join(root, "client-webassemblies/rsa-keygen/test/vectors-2048.json"), "utf8"))[0];
const cases =
[
    {name: "stored", passphrase: stored.passphrase, bits: 2048},
    {name: "unicode", passphrase: "caf\u00e9 \u0130 \ud83d\ude00 ".repeat(24), bits: 2048},
    {name: "unicode3072", passphrase: "caf\u00e9 \u0130 \ud83d\ude00 ".repeat(24), bits: 3072},
    {name: "newlines", passphrase: "\ufeff" + "identity\r\nline\n".repeat(16), bits: 2048},
    {name: "nul", passphrase: "prefix\0" + "identity".repeat(25), bits: 2048},
    {name: "surrogate", passphrase: "\ud800" + "identity".repeat(25), bits: 2048}
];
const b64 = function(value)
{
    return Buffer.from(value, "utf8").toString("base64");
};
const output = [];
const previous = process.argv[3] ? new Map(fs.readFileSync(process.argv[2], "utf8").trim().split("\n").map(line => [line.split("\t")[0], line.split("\t")])) : null;
const native = process.argv[3] ? new Map(fs.readFileSync(process.argv[3], "utf8").trim().split("\n").map(line => [line.split("\t")[0], line.split("\t")])) : null;
for (const entry of cases)
{
    const key = context.lemon_crypto.generateRSAKey(entry.passphrase, entry.bits);
    const publicKey = context.lemon_crypto.publicKeyString(key);
    if (entry.name === "stored")
    {
        assert.equal(publicKey, stored.public_key_string);
    }
    const keys = previous ? JSON.parse(Buffer.from(previous.get(entry.name)[5], "base64").toString("utf8")) : context.keys__create_random_message_keys();
    assert.equal(context.keys___channel_iv_key(keys).length, 32);
    const plaintext = JSON.stringify({type: "direct_chat_message", value: "Hello \ud83d\ude00 \u017e\n" + entry.name});
    const direct = context.keys__seal_direct_content(publicKey, keys, context.keys__encrypt_direct_content(keys, plaintext));
    const channel = context.keys__seal_channel_content(keys, context.keys__encrypt_with_aes_keys_and_convert_to_base64(keys, plaintext));
    const file = JSON.stringify({name: "test.txt", size: 5, mime: "text/plain", data: "SGVsbG8="});
    const picture = "data:image/png;base64,SGVsbG8=";
    const directFile = context.keys__seal_direct_content(publicKey, keys, context.chat_files__encrypt_direct_file_content(keys, file, 4096));
    const channelFile = context.keys__seal_channel_content(keys, context.chat_files__encrypt_string_with_aes_keys_fast(keys, file, 4096));
    const directPicture = context.keys__seal_direct_content(publicKey, keys, context.keys__encrypt_direct_content(keys, picture));
    const channelPicture = context.keys__seal_channel_content(keys, context.keys__encrypt_with_aes_keys_and_convert_to_base64(keys, picture));
    assert.ok(direct);
    assert.ok(channel);
    if (native)
    {
        const row = native.get(entry.name);
        const nativeDirect = context.keys__open_direct_content(JSON.parse(Buffer.from(row[1], "base64").toString("utf8")), key);
        assert.ok(nativeDirect);
        assert.equal(context.keys__decrypt_direct_content(nativeDirect.keys, nativeDirect.value), plaintext);
        const nativeChannel = context.keys__open_channel_content(keys, Buffer.from(row[2], "base64").toString("utf8"));
        assert.ok(nativeChannel);
        assert.equal(context.keys__decrypt_base64_contents_with_aes_keys(keys, nativeChannel), plaintext);
        const privateFile = context.keys__open_direct_content(JSON.parse(Buffer.from(row[3], "base64").toString("utf8")), key);
        assert.equal(context.chat_files__decrypt_direct_file_content(privateFile.keys, privateFile.value), file);
        const channelFileValue = context.keys__open_channel_content(keys, Buffer.from(row[4], "base64").toString("utf8"));
        assert.equal(context.chat_files__decrypt_base64_with_aes_keys_fast(keys, channelFileValue), file);
        const privatePicture = context.keys__open_direct_content(JSON.parse(Buffer.from(row[5], "base64").toString("utf8")), key);
        assert.equal(context.keys__decrypt_direct_content(privatePicture.keys, privatePicture.value), picture);
        const channelPictureValue = context.keys__open_channel_content(keys, Buffer.from(row[6], "base64").toString("utf8"));
        assert.equal(context.keys__decrypt_base64_contents_with_aes_keys(keys, channelPictureValue), picture);
    }
    output.push([entry.name, entry.bits, Buffer.from(entry.passphrase, "utf16le").toString("base64"), context.sha256.hex(entry.passphrase), publicKey, b64(JSON.stringify(keys)), b64(plaintext), b64(JSON.stringify(direct)), b64(channel), b64(JSON.stringify(directFile)), b64(channelFile), b64(JSON.stringify(directPicture)), b64(channelPicture)].join("\t"));
}
if (native)
{
    console.log("PASS: browser decrypts native direct and channel messages, pictures and authenticated files for all six identity cases");
}
else
{
    fs.writeFileSync(process.argv[2], output.join("\n") + "\n");
    console.log("PASS: browser identity, direct and channel fixtures generated for six passphrase cases");
}
