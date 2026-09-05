# what the vectors are

a "vector" (test vector, golden vector) is one recorded pair of *fixed input -> expected
correct output*. the term comes from crypto standards: NIST ships test vectors for AES,
SHA-256 etc. so implementers can check their code against the officially correct answers.
it has nothing to do with geometric vectors or C++ `std::vector`.

here, one vector = one json object:

- **input:** a passphrase and a bit size (e.g. `"short one"` at 512 bits)
- **expected output:** every key component (`n`, `e`, `d`, `p`, `q`, `dmp1`, `dmq1`,
  `coeff`) that the vendored js keygen (jsbn + seedrandom + cryptico) produces for it,
  plus `public_key_string` and how long the js took

golden vectors are the right tool for this module because a lemon-chat identity is a
*deterministic* function of the passphrase: there is exactly one correct keypair per
input, so the wasm can be checked byte-for-byte against the recorded js answers, and a
single wrong bit anywhere in the prime walk shows up as a mismatch. this is how the
2026-08-29 `mont_mul` reduction-width bug was caught.

## files

- `vectors-512.json`, `vectors-1024.json` (and `vectors-2048.json` once generated):
  4 passphrases each — three ~200+ char identity-style strings and one short one
- `make-vectors.js <bits>` regenerates a file by running the REAL vendored js keygen.
  slow on purpose (minutes per key at 2048); writes after every key so a crash loses
  nothing. only regenerate to add sizes/passphrases — never to make a failing wasm pass,
  because the js output is the identity format that existing users already depend on
- `compare-wasm.js <file>` feeds the same seeds to `build/rsa_keygen.wasm` and demands
  exact equality on all fields
- `diag.js` localizes a mismatch: low-digits-only difference = walk-length desync
  (a primality verdict differed), high-digit difference = candidate-draw desync
  (the Math.random() stream shifted earlier)
- `validate-key.js <bits>` covers sizes with no golden vectors (the js reference takes
  hours above 2048): generates in the wasm and BigInt-checks every RSA invariant. note
  jsbn only forces each prime's top bit, so n can be one bit short of the request —
  the js path has the same property, so this is not a wasm defect
- `rsa-ops.js` covers the runtime side: the wasm `rsa_keygen__modpow` export (and the
  crt combine the client glue builds on it) must equal jsbn's doPublic/doPrivate for
  every golden key, deterministic messages, and the edge values 0, 1, n-1, base>n
- `crypto-compat.js` covers the vendor removal: `scripts/app/rsa-crypto.js` replaced
  jsbn.js, rsa.js, rsa-sign.js, cryptico.js and biginteger.js, and has to stay
  wire-compatible with them or a new client cannot read what an old one wrote. it loads
  both stacks into one vm and compares the base64/hex conversions, the public key
  string, the keygen fields, pkcs#1 and the message envelope in BOTH directions
- `legacy-reference/` holds the replaced libraries (jsbn, rsa, rsa-sign, cryptico,
  biginteger, plus the sha1 and md5 they alone consumed). they are no longer part of
  the client, they are kept only as the oracle the tests above compare against - do not
  wire them back into `src/`
