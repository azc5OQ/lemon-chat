// mathematical validation for sizes with no golden vectors (the js reference takes
// hours above 2048): generates a key in the wasm and BigInt-checks every RSA invariant.
// proves the output is a VALID keypair of the requested size; walk-equality with the js
// is proven separately by compare-wasm.js at the sizes that have vectors.
//   node validate-key.js [bits] [passphrase]
let fs = require("fs");
let crypto = require("crypto");

let bits = parseInt(process.argv[2]) || 4096;
let passphrase = process.argv[3] || "validate-key test passphrase 0123456789 ".repeat(5);

let wasm_bytes = fs.readFileSync(__dirname + "/../build/rsa_keygen.wasm");

function modpow(b, e, m)
{
    let r = 1n;
    b %= m;
    while (e > 0n)
    {
        if (e & 1n) { r = (r * b) % m; }
        b = (b * b) % m;
        e >>= 1n;
    }
    return r;
}

// miller-rabin with 20 pseudo-random bases; error probability < 4^-20
function is_probable_prime(n)
{
    if (n < 2n || (n & 1n) == 0n) { return false; }
    let d = n - 1n, s = 0n;
    while ((d & 1n) == 0n) { d >>= 1n; s++; }
    for (let i = 0; i < 20; i++)
    {
        let a = 2n + BigInt("0x" + crypto.randomBytes(16).toString("hex")) % (n - 3n);
        let x = modpow(a, d, n);
        if (x == 1n || x == n - 1n) { continue; }
        let ok = false;
        for (let j = 0n; j < s - 1n; j++)
        {
            x = (x * x) % n;
            if (x == n - 1n) { ok = true; break; }
        }
        if (!ok) { return false; }
    }
    return true;
}

WebAssembly.instantiate(wasm_bytes, {}).then(function(result)
{
    let wasm = result.instance.exports;
    let mem = new Uint8Array(wasm.memory.buffer);

    let seed_hex = crypto.createHash("sha256").update(passphrase, "utf8").digest("hex");
    let seed_ptr = wasm.rsa_keygen__get_seed_buffer();
    for (let j = 0; j < seed_hex.length; j++) { mem[seed_ptr + j] = seed_hex.charCodeAt(j); }

    let started = Date.now();
    let ok = wasm.rsa_keygen__generate(bits, seed_hex.length);
    let took = Date.now() - started;

    if (ok != 1) { console.log("generate returned " + ok); process.exit(1); }

    let names = ["n", "e", "d", "p", "q", "dmp1", "dmq1", "coeff"];
    let k = {};
    for (let f = 0; f < names.length; f++)
    {
        let ptr = wasm.rsa_keygen__get_result(f);
        let hex = "";
        for (let b = ptr; mem[b] != 0; b++) { hex += String.fromCharCode(mem[b]); }
        k[names[f]] = BigInt("0x" + hex);
    }

    let phi = (k.p - 1n) * (k.q - 1n);
    // jsbn's RSAGenerate only forces each prime's top bit, so n can land one bit short
    // of the request; both the js and the wasm share that property
    let n_bits = k.n.toString(2).length;
    let checks = [
        ["n bitlength " + n_bits + " in {" + bits + "," + (bits - 1) + "}", n_bits == bits || n_bits == bits - 1],
        ["n == p*q",              k.n == k.p * k.q],
        ["p > q",                 k.p > k.q],
        ["p prime",               is_probable_prime(k.p)],
        ["q prime",               is_probable_prime(k.q)],
        ["e == 3",                k.e == 3n],
        ["3*d == 1 mod phi",      (3n * k.d) % phi == 1n],
        ["dmp1 == d mod p-1",     k.dmp1 == k.d % (k.p - 1n)],
        ["dmq1 == d mod q-1",     k.dmq1 == k.d % (k.q - 1n)],
        ["q*coeff == 1 mod p",    (k.q * k.coeff) % k.p == 1n],
        ["encrypt/decrypt",       modpow(modpow(1234567890123456789n, 3n, k.n), k.d, k.n) == 1234567890123456789n]
    ];

    let failures = 0;
    for (let c = 0; c < checks.length; c++)
    {
        if (!checks[c][1]) { failures++; }
        console.log((checks[c][1] ? "  ok   " : "  FAIL ") + checks[c][0]);
    }
    console.log(bits + " bits in " + took + "ms: " + (failures == 0 ? "VALID KEYPAIR" : failures + " FAILED CHECKS"));
    process.exit(failures == 0 ? 0 : 1);
});
