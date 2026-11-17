// generate reference RSA keys with the legacy JavaScript implementation
// save every key component for comparison with Wasm
// run: node make-vectors.js [bits] [outfile]

let fs = require("fs");

// load the legacy key generator used as the reference
let vendor_path = __dirname + "/../../../main/scripts/vendor/";
let legacy_path = __dirname + "/legacy-reference/";
let source = ["jsbn.js", "sha256.js", "rsa.js", "cryptico.js"]
    .map(function(f)
    {
        let legacy_file = legacy_path + f;
        return fs.readFileSync(fs.existsSync(legacy_file) ? legacy_file : (vendor_path + f), "utf8");
    })
    .join("\n");

// provide the browser globals required by the legacy libraries
let navigator = { appName: "node", appVersion: "5" };
let window = {};
let cryptico_ref = null;

eval(source + "\ncryptico_ref = cryptico;");

let bits = parseInt(process.argv[2]) || 2048;
let outfile = process.argv[3] || (__dirname + "/vectors-" + bits + ".json");

// include long identity passphrases and short test inputs
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

    // save each completed key so an interrupted run keeps its progress
    fs.writeFileSync(outfile, JSON.stringify(vectors, null, 1));
}

console.log("wrote " + outfile);
