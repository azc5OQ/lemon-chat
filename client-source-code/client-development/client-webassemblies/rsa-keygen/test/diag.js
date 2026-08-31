// pinpoints where a mismatched vector diverges, to tell a walk-length desync (low bits
// only) from a candidate-draw desync (high bits)
let fs = require("fs");
let crypto = require("crypto");

let vectors = JSON.parse(fs.readFileSync(__dirname + "/vectors-512.json", "utf8"));
let wasm_bytes = fs.readFileSync(__dirname + "/../build/rsa_keygen.wasm");

WebAssembly.instantiate(wasm_bytes, {}).then(function(result)
{
    let wasm = result.instance.exports;
    let mem = new Uint8Array(wasm.memory.buffer);
    let v = vectors[2]; // vector 3

    let seed_hex = crypto.createHash("sha256").update(v.passphrase, "utf8").digest("hex");
    let seed_ptr = wasm.rsa_keygen__get_seed_buffer();
    for (let j = 0; j < seed_hex.length; j++) { mem[seed_ptr + j] = seed_hex.charCodeAt(j); }
    wasm.rsa_keygen__generate(v.bits, seed_hex.length);

    ["p", "q"].forEach(function(field, idx)
    {
        let ptr = wasm.rsa_keygen__get_result(idx === 0 ? 3 : 4);
        let hex = "";
        for (let b = ptr; mem[b] != 0; b++) { hex += String.fromCharCode(mem[b]); }

        let js_hex = v[field];
        console.log(field + ": js len=" + js_hex.length + " wasm len=" + hex.length);
        // align from the most significant nibble
        let n = Math.max(js_hex.length, hex.length);
        let js_pad = js_hex.padStart(n, "0");
        let wa_pad = hex.padStart(n, "0");
        let first_diff = -1;
        for (let i = 0; i < n; i++) { if (js_pad[i] !== wa_pad[i]) { first_diff = i; break; } }
        console.log("  first differing hex digit from MSB: " + first_diff + " of " + n
            + " (bits from top ~" + (first_diff * 4) + ")");
        if (first_diff >= 0)
        {
            console.log("  js  : ..." + js_pad.slice(Math.max(0, first_diff - 4), first_diff + 8));
            console.log("  wasm: ..." + wa_pad.slice(Math.max(0, first_diff - 4), first_diff + 8));
        }
    });
});
