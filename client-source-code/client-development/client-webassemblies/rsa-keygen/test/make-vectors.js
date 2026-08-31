// generates the golden vectors for the wasm port: runs the REAL vendored js keygen
// (jsbn + seedrandom + cryptico) for a set of passphrases and records every key part.
// the wasm module must reproduce these numbers bit for bit, because an identity is
// exactly this deterministic prime walk
//   node make-vectors.js [bits] [outfile]

let fs = require("fs");

// the client no longer ships these; they live here as the reference the wasm is held to
let vendor_path = __dirname + "/../../../src/scripts/vendor/";
let legacy_path = __dirname + "/legacy-reference/";
let source = ["jsbn.js", "sha256.js", "rsa.js", "cryptico.js"]
    .map(function(f)
    {
        let legacy_file = legacy_path + f;
        return fs.readFileSync(fs.existsSync(legacy_file) ? legacy_file : (vendor_path + f), "utf8");
    })
    .join("\n");

// the vendor files expect a browser at load time; these shims satisfy them
let navigator = { appName: "node", appVersion: "5" };
let window = {};
let cryptico_ref = null;

eval(source + "\ncryptico_ref = cryptico;");

let bits = parseInt(process.argv[2]) || 2048;
let outfile = process.argv[3] || (__dirname + "/vectors-" + bits + ".json");

// a mix of realistic identity passphrases (the app uses 199+ char strings) and short ones
let passphrases = [
    "a".repeat(199),
    "lemon-chat test identity passphrase 0123456789 ".repeat(5),
    "sVqmEo2FZjaWyN8u1RfB7cXhKp4dTn5wLgQxYzC3vHbUkP9tMrDs6JeGA0iOl".repeat(4),
    "short one"
];

let count = parseInt(process.argv[4]) || passphrases.length;
let vectors = [];

for (let i = 0; i < count; i++)
{
    let started = Date.now();
    let key = cryptico_ref.generateRSAKey(passphrases[i], bits);
    let took = Date.now() - started;

    vectors.push({
        passphrase: passphrases[i],
        bits: bits,
        ms_js: took,
        n: key.n.toString(16),
        e: key.e.toString(16),
        d: key.d.toString(16),
        p: key.p.toString(16),
        q: key.q.toString(16),
        dmp1: key.dmp1.toString(16),
        dmq1: key.dmq1.toString(16),
        coeff: key.coeff.toString(16),
        public_key_string: cryptico_ref.publicKeyString(key)
    });

    console.log("vector " + (i + 1) + "/" + count + " done in " + took + "ms (" + bits + " bits)");

    // written after every key, because a long 2048 run once died on key 3 and lost everything
    fs.writeFileSync(outfile, JSON.stringify(vectors, null, 1));
}

console.log("wrote " + outfile);
