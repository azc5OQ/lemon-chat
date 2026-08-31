// differential test for the runtime rsa ops: the wasm modpow (and the crt combine the
// client glue builds on it) must produce exactly what jsbn's doPublic/doPrivate produce,
// for every golden key and a spread of deterministic messages plus edge values.
//   node rsa-ops.js
let fs = require("fs");

// jsbn/rsa are the reference the wasm ops are held to; the client itself no longer ships them
let vendor_path = __dirname + "/../../../src/scripts/vendor/";
let legacy_path = __dirname + "/legacy-reference/";
let source = ["jsbn.js", "rsa.js"]
    .map(function(f)
    {
        let legacy_file = legacy_path + f;
        return fs.readFileSync(fs.existsSync(legacy_file) ? legacy_file : (vendor_path + f), "utf8");
    })
    .join("\n");

let navigator = { appName: "node", appVersion: "5" };
let window = {};
let probe = null;
eval(source + "\nprobe = { BigInteger: BigInteger, RSAKey: RSAKey, parseBigInt: parseBigInt };");

let wasm_bytes = fs.readFileSync(__dirname + "/../build/rsa_keygen.wasm");

// deterministic message stream, so failures reproduce
let lcg_state = 0x12345678;
function lcg_hex_below(n_hex)
{
    let out = "";
    for (let i = 0; i < n_hex.length - 1; i++)
    {
        lcg_state = (Math.imul(lcg_state, 1103515245) + 12345) & 0x7fffffff;
        out += (lcg_state & 15).toString(16);
    }
    return out.replace(/^0+/, "") || "1";
}

WebAssembly.instantiate(wasm_bytes, {}).then(function(result)
{
    let wasm = result.instance.exports;

    function modpow_hex(base_hex, exp_hex, mod_hex)
    {
        let mem = new Uint8Array(wasm.memory.buffer);
        let ops = [base_hex, exp_hex, mod_hex];
        for (let i = 0; i < 3; i++)
        {
            let ptr = wasm.rsa_keygen__get_modpow_buffer(i);
            for (let j = 0; j < ops[i].length; j++) { mem[ptr + j] = ops[i].charCodeAt(j); }
            mem[ptr + ops[i].length] = 0;
        }
        if (wasm.rsa_keygen__modpow() != 1) { throw new Error("modpow rejected"); }
        mem = new Uint8Array(wasm.memory.buffer);
        let ptr = wasm.rsa_keygen__get_modpow_result();
        let hex = "";
        for (let b = ptr; mem[b] != 0; b++) { hex += String.fromCharCode(mem[b]); }
        return hex;
    }

    // the crt combine exactly as the client glue does it
    function wasm_do_private(key, x)
    {
        let xp = probe.parseBigInt(modpow_hex(x.mod(key.p).toString(16), key.dmp1.toString(16), key.p.toString(16)), 16);
        let xq = probe.parseBigInt(modpow_hex(x.mod(key.q).toString(16), key.dmq1.toString(16), key.q.toString(16)), 16);
        while (xp.compareTo(xq) < 0) { xp = xp.add(key.p); }
        return xp.subtract(xq).multiply(key.coeff).mod(key.p).multiply(key.q).add(xq);
    }

    let checks = 0;
    let failures = 0;
    function expect_equal(label, a_hex, b_hex)
    {
        checks++;
        if (a_hex !== b_hex)
        {
            failures++;
            console.log("MISMATCH " + label);
            console.log("  js:   " + a_hex.slice(0, 64) + "...");
            console.log("  wasm: " + b_hex.slice(0, 64) + "...");
        }
    }

    ["vectors-512.json", "vectors-1024.json", "vectors-2048.json"].forEach(function(file)
    {
        let vectors = JSON.parse(fs.readFileSync(__dirname + "/" + file, "utf8"));
        vectors.forEach(function(v, vi)
        {
            let key = new probe.RSAKey();
            key.setPrivateEx(v.n, v.e, v.d, v.p, v.q, v.dmp1, v.dmq1, v.coeff);

            let messages = [];
            for (let m = 0; m < 8; m++) { messages.push(lcg_hex_below(v.n)); }
            messages.push("0");
            messages.push("1");
            messages.push(key.n.subtract(probe.BigInteger.ONE).toString(16));

            messages.forEach(function(m_hex, mi)
            {
                let x = probe.parseBigInt(m_hex, 16);
                let tag = file + " v" + (vi + 1) + " m" + mi;

                let js_pub = key.doPublic(x);
                expect_equal(tag + " doPublic", js_pub.toString(16), modpow_hex(m_hex, "3", v.n));

                let js_priv = key.doPrivate(js_pub);
                expect_equal(tag + " doPrivate", js_priv.toString(16), wasm_do_private(key, js_pub).toString(16));
            });

            // no-crt private path and a base larger than the modulus
            let c = key.doPublic(probe.parseBigInt(messages[0], 16));
            expect_equal(file + " v" + (vi + 1) + " modPow(d)",
                c.modPow(key.d, key.n).toString(16),
                modpow_hex(c.toString(16), v.d, v.n));
            expect_equal(file + " v" + (vi + 1) + " base>n",
                probe.parseBigInt(v.n + v.p, 16).mod(key.n).toString(16),
                modpow_hex(v.n + v.p, "1", v.n));
        });
    });

    console.log(checks + " checks, " + failures + " failures");
    process.exit(failures == 0 ? 0 : 1);
});
