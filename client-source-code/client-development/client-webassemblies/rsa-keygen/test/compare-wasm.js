// differential test: the wasm keygen must reproduce the js golden vectors exactly,
// because a single differing bit means a different identity for the same passphrase
//   node compare-wasm.js [vectors-file]

let fs = require("fs");
let crypto = require("crypto");

let vectors_file = process.argv[2] || (__dirname + "/vectors-512.json");
let vectors = JSON.parse(fs.readFileSync(vectors_file, "utf8"));

let wasm_bytes = fs.readFileSync(__dirname + "/../build/rsa_keygen.wasm");

let field_names = ["n", "e", "d", "p", "q", "dmp1", "dmq1", "coeff"];

WebAssembly.instantiate(wasm_bytes, {}).then(function(result)
{
    let wasm = result.instance.exports;
    let memory_bytes = new Uint8Array(wasm.memory.buffer);
    let failures = 0;

    for (let i = 0; i < vectors.length; i++)
    {
        let vector = vectors[i];

        // the js side seeds with sha256.hex(passphrase); the vendored sha256 and node's
        // agree for these ascii passphrases
        let seed_hex = crypto.createHash("sha256").update(vector.passphrase, "utf8").digest("hex");

        let seed_ptr = wasm.rsa_keygen__get_seed_buffer();
        for (let j = 0; j < seed_hex.length; j++)
        {
            memory_bytes[seed_ptr + j] = seed_hex.charCodeAt(j);
        }

        let started = Date.now();
        let ok = wasm.rsa_keygen__generate(vector.bits, seed_hex.length);
        let took = Date.now() - started;

        if (ok != 1)
        {
            console.log("vector " + (i + 1) + ": generate returned " + ok);
            failures++;
            continue;
        }

        let all_match = true;
        for (let f = 0; f < field_names.length; f++)
        {
            let result_ptr = wasm.rsa_keygen__get_result(f);
            let hex = "";
            for (let b = result_ptr; memory_bytes[b] != 0; b++)
            {
                hex += String.fromCharCode(memory_bytes[b]);
            }

            if (hex !== vector[field_names[f]])
            {
                console.log("vector " + (i + 1) + " MISMATCH on " + field_names[f]);
                console.log("  js:   " + vector[field_names[f]].slice(0, 64) + "...");
                console.log("  wasm: " + hex.slice(0, 64) + "...");
                all_match = false;
            }
        }

        if (all_match)
        {
            console.log("vector " + (i + 1) + "/" + vectors.length + " MATCH (" + vector.bits
                + " bits, wasm " + took + "ms vs js " + vector.ms_js + "ms)");
        }
        else
        {
            failures++;
        }
    }

    console.log(failures == 0 ? "ALL VECTORS MATCH" : failures + " FAILURES");
    process.exit(failures == 0 ? 0 : 1);
});
