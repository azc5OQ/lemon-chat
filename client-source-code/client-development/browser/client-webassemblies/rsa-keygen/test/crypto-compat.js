// compare the current RSA implementation with the legacy libraries
// check key generation, encryption and decryption in both directions
// run: node crypto-compat.js

let fs = require("fs");
let vm = require("vm");
let node_crypto = require("crypto");

let development_path = __dirname + "/../../../";
let vendor_path = development_path + "main/scripts/vendor/";
let app_path = development_path + "main/scripts/app/";
let legacy_path = __dirname + "/legacy-reference/";

// read removed libraries from the reference directory and current ones from vendor
function read_reference(file)
{
    let legacy_file = legacy_path + file;
    return fs.readFileSync(fs.existsSync(legacy_file) ? legacy_file : (vendor_path + file), "utf8");
}

let wasm_base64 = fs.readFileSync(process.env.LEMON_RSA_WASM || (__dirname + "/../build/rsa_keygen.wasm")).toString("base64");

// provide the browser globals used by the vendor libraries
let sandbox = {
    navigator: { appName: "node", appVersion: "5" },
    console: console,
    atob: function (s)
    {
        return Buffer.from(s, "base64").toString("binary");
    },
    crypto: node_crypto.webcrypto,
    WebAssembly: WebAssembly
};
sandbox.window = sandbox;
sandbox.self = sandbox;
sandbox.global = sandbox;
vm.createContext(sandbox);

// close the AES factory here because this test does not load main.js
vm.runInContext(read_reference("aes-js.js") + "\n}));", sandbox, { filename: "aes-js.js" });

// load the legacy libraries in their original order
let old_source = ["biginteger.js", "jsbn.js", "sha256.js", "sha1.js", "md5.js", "rsa.js", "rsa-sign.js", "cryptico.js"] .map(read_reference) .join("\n");
vm.runInContext(old_source, sandbox, { filename: "vendor-stack.js" });

// embed the Wasm bytes in the current implementation
let new_source = fs.readFileSync(app_path + "crypto/rsa-crypto.js", "utf8") .replace("'@@WASM:wasm/rsa_keygen.wasm@@'", JSON.stringify(wasm_base64));
vm.runInContext(new_source, sandbox, { filename: "crypto/rsa-crypto.js" });

let old_crypto = sandbox.cryptico;
let new_crypto = sandbox.lemon_crypto;
let OldRSAKey = sandbox.RSAKey;

let checks = 0;
let failures = 0;

function expect(label, condition, detail)
{
    checks++;
    if (condition !== true)
    {
        failures++;
        console.log("FAIL  " + label + (detail ? ("\n      " + detail) : ""));
    }
}

function expect_equal(label, expected, actual)
{
    expect(label, expected === actual, "expected " + String(expected).slice(0, 72) + "\n      actual   " + String(actual).slice(0, 72));
}

// use a fixed seed so failures can be reproduced
let lcg = 0x2f6e2b1;
function next_byte()
{
    lcg = (Math.imul(lcg, 1103515245) + 12345) & 0x7fffffff;
    return (lcg >> 16) & 255;
}

// check envelope encoding conversions
for (let round = 0; round < 150; round++)
{
    let length = 1 + (next_byte() % 64);
    let byte_string = "";
    let hex_string = "";

    for (let i = 0; i < length; i++)
    {
        let b = next_byte();
        byte_string += String.fromCharCode(b);
        hex_string += b.toString(16).padStart(2, "0");
    }
    // check the padding of a hex string with an odd number of digits
    if ((round % 3) == 0)
    {
        hex_string = hex_string.slice(1);
    }

    expect_equal("b256to64 round " + round, old_crypto.b256to64(byte_string), new_crypto.b256to64(byte_string));
    expect_equal("b16to64 round " + round, old_crypto.b16to64(hex_string), new_crypto.b16to64(hex_string));

    let base64_string = old_crypto.b256to64(byte_string);
    expect_equal("b64to256 round " + round, old_crypto.b64to256(base64_string), new_crypto.b64to256(base64_string));
    expect_equal("b64to16 round " + round, old_crypto.b64to16(base64_string), new_crypto.b64to16(base64_string));
}

// check keys and PKCS1 encryption in both directions
let vector_files = ["vectors-512.json", "vectors-1024.json", "vectors-2048.json"];
let key_field_names = ["n", "e", "d", "p", "q", "dmp1", "dmq1", "coeff"];

vector_files.forEach(function (file)
{
    let vectors = JSON.parse(fs.readFileSync(__dirname + "/" + file, "utf8"));

    vectors.forEach(function (v, vi)
    {
        let tag = file + " v" + (vi + 1);

        let old_key = new OldRSAKey();
        old_key.setPrivateEx(v.n, v.e, v.d, v.p, v.q, v.dmp1, v.dmq1, v.coeff);

        let new_key = new new_crypto.RSAKey();
        new_key.setPrivateEx(v.n, v.e, v.d, v.p, v.q, v.dmp1, v.dmq1, v.coeff);

        // compare the public key string sent to the server
        expect_equal(tag + " publicKeyString", v.public_key_string, new_crypto.publicKeyString(new_key));
        expect_equal(tag + " publicKeyString vs old", old_crypto.publicKeyString(old_key), new_crypto.publicKeyString(new_key));

        // generate the same key through the current API
        let generated = new_crypto.generateRSAKey(v.passphrase, v.bits);
        key_field_names.forEach(function (name)
        {
            expect_equal(tag + " generateRSAKey " + name, v[name], generated[name].toString(16));
        });

        // each implementation must decrypt the other's PKCS1 ciphertext
        let short_message = "lemon" + vi + "-pkcs1-probe";

        let old_cipher = old_key.encrypt(short_message);
        expect_equal(tag + " old encrypt -> new decrypt", short_message, new_key.decrypt(old_cipher));

        let new_cipher = new_key.encrypt(short_message);
        expect_equal(tag + " new encrypt -> old decrypt", short_message, old_key.decrypt(new_cipher));

        // check a Base64 encoded RSA challenge
        let challenge = "";
        for (let i = 0; i < 40; i++)
        {
            challenge += String.fromCharCode(33 + (next_byte() % 90));
        }

        let challenge_cipher = old_crypto.b16to64(old_key.encrypt(challenge));
        expect_equal(tag + " challenge_decrypt", challenge, new_crypto.challenge_decrypt(challenge_cipher, new_key));

        let challenge_cipher_new = new_crypto.b16to64(new_key.encrypt(challenge));
        expect_equal(tag + " challenge_decrypt (old reads new)", challenge, old_crypto.challenge_decrypt(challenge_cipher_new, old_key));

        // the hybrid envelope needs an RSA key of at least 1024 bits
        // UTF8 encoding can expand its AES key to 64 bytes before padding
        if (v.bits >= 1024)
        {
            let payloads = [
                JSON.stringify({ key: "0123456789abcdef", iv: "fedcba9876543210", n: vi }),
                "",
                "x",
                "a".repeat(500)
            ];

            payloads.forEach(function (payload, pi)
            {
                let old_envelope = old_crypto.encrypt(payload, v.public_key_string);
                expect_equal(tag + " p" + pi + " old envelope status", "success", old_envelope.status);
                let new_read = new_crypto.decrypt(old_envelope.cipher, new_key);
                expect_equal(tag + " p" + pi + " old envelope -> new", payload, new_read.plaintext);

                let new_envelope = new_crypto.encrypt(payload, v.public_key_string);
                expect_equal(tag + " p" + pi + " new envelope status", "success", new_envelope.status);
                let old_read = old_crypto.decrypt(new_envelope.cipher, old_key);
                expect_equal(tag + " p" + pi + " new envelope -> old", payload, old_read.plaintext);
            });

            // invalid envelopes must report failure without throwing
            let bad = new_crypto.decrypt("Zm9v?YmFy", new_key);
            expect(tag + " garbage envelope reports failure", bad.status === "failure" || bad.plaintext === "");
        }

        // invalid public keys must report failure without throwing
        expect_equal(tag + " invalid public key", "Invalid public key", new_crypto.encrypt("x", "!!!not base64!!!").status);
    });
});

// check the modular arithmetic used for Diffie Hellman
{
    let dh_modulus_string = "32317006071311007300338913926423828248817941241140239112842009751400741706634354222619689417363569347117901737909704191754605873209195028853758986185622153212175412514901774520270235796078236248884246189477587641105928646099411723245426622522193230540919037680524235519125679715870117001058055877651038861847280257976054903569732561526167081339361799541336476559160368317896729073178384589680639671900977202194168647225871031411336429319536193471636533209717077448227988588565369208645296636077250268955505928362751121174096972998068410554359584866583291642136218231078990999448652468262416972035911852507045361090559";
    let modulus = BigInt(dh_modulus_string);
    let old_bigint = sandbox.bigInt;

    for (let round = 0; round < 3; round++)
    {
        let exponent_hex = "";
        for (let i = 0; i < 64; i++)
        {
            exponent_hex += next_byte().toString(16).padStart(2, "0");
        }
        let exponent = BigInt("0x" + exponent_hex);

        let expected = old_bigint(2).modPow(old_bigint(exponent.toString()), old_bigint(dh_modulus_string)).toString();
        let actual = new_crypto.modpow(2n, exponent, modulus).toString();
        expect_equal("dh modpow round " + round, expected, actual);
    }

    // both sides must calculate the same shared value
    let a = BigInt("0x" + "3f".repeat(64));
    let b = BigInt("0x" + "a7".repeat(64));
    let A = new_crypto.modpow(2n, a, modulus);
    let B = new_crypto.modpow(2n, b, modulus);
    expect_equal("dh shared secret agrees", new_crypto.modpow(A, b, modulus).toString(), new_crypto.modpow(B, a, modulus).toString());
}

console.log(checks + " checks, " + failures + " failures");
process.exit(failures == 0 ? 0 : 1);
