// differential test for the vendor removal: scripts/app/rsa-crypto.js replaces
// jsbn.js + rsa.js + rsa-sign.js + cryptico.js + biginteger.js, and must stay
// wire-compatible with them - a client on the new build has to read what a client on
// the old build wrote, and the other way round, or identities and messages break.
//
// both stacks are loaded into one browser-like vm context and compared directly.
//   node crypto-compat.js

let fs = require("fs");
let vm = require("vm");
let node_crypto = require("crypto");

let development_path = __dirname + "/../../../";
let vendor_path = development_path + "src/scripts/vendor/";
let app_path = development_path + "src/scripts/app/";
let legacy_path = __dirname + "/legacy-reference/";

// the replaced libraries live in legacy-reference now, the ones the client still ships
// stay in vendor; read from wherever the file actually is
function read_reference(file)
{
    let legacy_file = legacy_path + file;
    return fs.readFileSync(fs.existsSync(legacy_file) ? legacy_file : (vendor_path + file), "utf8");
}

let wasm_base64 = fs.readFileSync(__dirname + "/../build/rsa_keygen.wasm").toString("base64");

// a browser-ish global: the vendor files assign onto it (aes-js does root.aesjs = ...)
let sandbox = {
    navigator: { appName: "node", appVersion: "5" },
    console: console,
    atob: function (s) { return Buffer.from(s, "base64").toString("binary"); },
    crypto: node_crypto.webcrypto,
    WebAssembly: WebAssembly
};
sandbox.window = sandbox;
sandbox.self = sandbox;
sandbox.global = sandbox;
vm.createContext(sandbox);

// the vendored aes-js.js never closes the factory call it opens - in the build that
// call is closed by the last line of main.js, which is why the whole client ends up
// inside it. standalone it needs the same closer appended
vm.runInContext(read_reference("aes-js.js") + "\n}));", sandbox, { filename: "aes-js.js" });

// the stack being replaced, evaluated as one unit the way the build concatenated it
let old_source = ["biginteger.js", "jsbn.js", "sha256.js", "sha1.js", "md5.js", "rsa.js", "rsa-sign.js", "cryptico.js"]
    .map(read_reference)
    .join("\n");
vm.runInContext(old_source, sandbox, { filename: "vendor-stack.js" });

// the new implementation, with the build's wasm token filled in the way build.py does
let new_source = fs.readFileSync(app_path + "rsa-crypto.js", "utf8")
    .replace("'@@WASM:wasm/rsa_keygen.wasm@@'", JSON.stringify(wasm_base64));
vm.runInContext(new_source, sandbox, { filename: "rsa-crypto.js" });

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
    expect(label, expected === actual,
        "expected " + String(expected).slice(0, 72) + "\n      actual   " + String(actual).slice(0, 72));
}

// deterministic pseudo-random source, so a failure reproduces
let lcg = 0x2f6e2b1;
function next_byte()
{
    lcg = (Math.imul(lcg, 1103515245) + 12345) & 0x7fffffff;
    return (lcg >> 16) & 255;
}

// ---------------------------------------------------------------------------
// 1. the conversions the envelope is built out of
// ---------------------------------------------------------------------------
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
    // an odd-length hex input too: b16to64 pads it, and that padding must match
    if ((round % 3) == 0) { hex_string = hex_string.slice(1); }

    expect_equal("b256to64 round " + round, old_crypto.b256to64(byte_string), new_crypto.b256to64(byte_string));
    expect_equal("b16to64 round " + round, old_crypto.b16to64(hex_string), new_crypto.b16to64(hex_string));

    let base64_string = old_crypto.b256to64(byte_string);
    expect_equal("b64to256 round " + round, old_crypto.b64to256(base64_string), new_crypto.b64to256(base64_string));
    expect_equal("b64to16 round " + round, old_crypto.b64to16(base64_string), new_crypto.b64to16(base64_string));
}

// ---------------------------------------------------------------------------
// 2. keys: same generation, same public key string, same pkcs#1 both directions
// ---------------------------------------------------------------------------
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

        // the identity as the server sees it
        expect_equal(tag + " publicKeyString", v.public_key_string, new_crypto.publicKeyString(new_key));
        expect_equal(tag + " publicKeyString vs old", old_crypto.publicKeyString(old_key), new_crypto.publicKeyString(new_key));

        // the keygen walk itself, through the new api
        let generated = new_crypto.generateRSAKey(v.passphrase, v.bits);
        key_field_names.forEach(function (name)
        {
            expect_equal(tag + " generateRSAKey " + name, v[name], generated[name].toString(16));
        });

        // pkcs#1 in both directions: whatever one stack encrypts, the other must read
        let short_message = "lemon" + vi + "-pkcs1-probe";

        let old_cipher = old_key.encrypt(short_message);
        expect_equal(tag + " old encrypt -> new decrypt", short_message, new_key.decrypt(old_cipher));

        let new_cipher = new_key.encrypt(short_message);
        expect_equal(tag + " new encrypt -> old decrypt", short_message, old_key.decrypt(new_cipher));

        // the server's rsa challenge is exactly this shape, base64 wrapped
        let challenge = "";
        for (let i = 0; i < 40; i++) { challenge += String.fromCharCode(33 + (next_byte() % 90)); }

        let challenge_cipher = old_crypto.b16to64(old_key.encrypt(challenge));
        expect_equal(tag + " challenge_decrypt", challenge, new_crypto.challenge_decrypt(challenge_cipher, new_key));

        let challenge_cipher_new = new_crypto.b16to64(new_key.encrypt(challenge));
        expect_equal(tag + " challenge_decrypt (old reads new)", challenge,
            old_crypto.challenge_decrypt(challenge_cipher_new, old_key));

        // ------------------------------------------------------------------
        // 3. the hybrid envelope. a 32 byte aes key utf-8 expands to as much as
        // 64 bytes inside the padding, so it only fits keys of 1024 bits and up -
        // which is what the app uses
        // ------------------------------------------------------------------
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

            // a garbage envelope must fail, not throw
            let bad = new_crypto.decrypt("Zm9v?YmFy", new_key);
            expect(tag + " garbage envelope reports failure", bad.status === "failure" || bad.plaintext === "");
        }

        // an invalid public key is reported, not thrown
        expect_equal(tag + " invalid public key", "Invalid public key", new_crypto.encrypt("x", "!!!not base64!!!").status);
    });
});

// ---------------------------------------------------------------------------
// 4. the diffie-hellman helper that replaces biginteger.js
// ---------------------------------------------------------------------------
{
    let dh_modulus_string = "32317006071311007300338913926423828248817941241140239112842009751400741706634354222619689417363569347117901737909704191754605873209195028853758986185622153212175412514901774520270235796078236248884246189477587641105928646099411723245426622522193230540919037680524235519125679715870117001058055877651038861847280257976054903569732561526167081339361799541336476559160368317896729073178384589680639671900977202194168647225871031411336429319536193471636533209717077448227988588565369208645296636077250268955505928362751121174096972998068410554359584866583291642136218231078990999448652468262416972035911852507045361090559";
    let modulus = BigInt(dh_modulus_string);
    let old_bigint = sandbox.bigInt;

    for (let round = 0; round < 3; round++)
    {
        let exponent_hex = "";
        for (let i = 0; i < 64; i++) { exponent_hex += next_byte().toString(16).padStart(2, "0"); }
        let exponent = BigInt("0x" + exponent_hex);

        let expected = old_bigint(2).modPow(old_bigint(exponent.toString()), old_bigint(dh_modulus_string)).toString();
        let actual = new_crypto.modpow(2n, exponent, modulus).toString();
        expect_equal("dh modpow round " + round, expected, actual);
    }

    // and the second half of the exchange: g^(ab) must agree from both sides
    let a = BigInt("0x" + "3f".repeat(64));
    let b = BigInt("0x" + "a7".repeat(64));
    let A = new_crypto.modpow(2n, a, modulus);
    let B = new_crypto.modpow(2n, b, modulus);
    expect_equal("dh shared secret agrees",
        new_crypto.modpow(A, b, modulus).toString(),
        new_crypto.modpow(B, a, modulus).toString());
}

console.log(checks + " checks, " + failures + " failures");
process.exit(failures == 0 ? 0 : 1);
