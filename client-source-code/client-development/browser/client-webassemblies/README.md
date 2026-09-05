# Client WebAssembly files

`client.html` uses three WebAssembly modules that are embedded directly as
Base64 strings. This way the client does not need to load any external
resources — everything lives inside the single `client.html` file.

The three modules are **minimp3**, **libopus** and **rsa-keygen**.

## minimp3 — MP3 streaming

Lets the client pick an MP3 file from disk (similar to selecting an image) and
stream it to the other clients in the channel.

This feature predates the music-bot feature. Now that music bots exist, it is
arguably redundant. The original idea was to let users easily grab an MP3 from
their system and send it to everyone in the channel — much like the old
TeamSpeak days when people streamed music through their microphone.

## libopus (Opus) — microphone & voice audio

Enables the client to use the microphone and to receive audio from other
clients. Received audio is decrypted with the channel keys into Opus, then
decoded into PCM and played back.

## rsa-keygen — identity keypair generation and runtime RSA

Since 2026-08-30 this module is the client's *only* RSA implementation. The
vendored `jsbn.js`, `rsa.js`, `rsa-sign.js`, `cryptico.js` and `biginteger.js`
were removed; `src/scripts/app/rsa-crypto.js` (the global `lemon_crypto`)
replaces all five using this wasm for the modular exponentiation, native
`BigInt` for Diffie-Hellman, `aes-js` for AES-CBC and `crypto.getRandomValues`
for key material. The five removed libraries are kept in
`rsa-keygen/test/legacy-reference/` as the oracle `test/crypto-compat.js`
compares against; they are not part of the client any more.

There is no JavaScript fallback left, so this module is now required.


Generates the RSA identity keypair from the passphrase ~100x faster than the
vendored JavaScript (jsbn/cryptico) path. It reproduces the JavaScript prime
walk **bit for bit** — same passphrase, same keypair — which the golden-vector
tests in `rsa-keygen/test/` prove at 512/1024/2048 bits.

The same module also carries a generic `modpow` export that the client's
`doPublic`/`doPrivate` (message-key encrypt/decrypt, challenge decrypt,
signatures) run through — ~4x faster than jsbn at 2048 bits, value-identical
(`rsa-keygen/test/rsa-ops.js`). The DH handshake stays on biginteger.js, which
uses native BigInt and is already fast.

If the module fails to instantiate, the client automatically falls back to the
JavaScript paths and produces identical values, just slower. Unlike the audio
modules it is built with plain clang (`rsa-keygen/build.bat`, Windows, no
emscripten needed).

## Identity randomness (assessment, 2026-08-29)

The keypair adds no randomness of its own — by design. `sha256(passphrase)`
seeds the whole prime walk, so a keypair is exactly as random as its
passphrase, and never more than 256 bits (the size of that hash).

- **User-entered passphrase:** whatever entropy the string has; the UI
  requires 199+ characters.
- **Random identity (the default):** `randomstring(200)` in `main.js` draws
  from `Math.random()`, not `crypto.getRandomValues`. The first identity in a
  worker comes from V8's xorshift128+ (OS-seeded, ~128-bit ceiling). That is
  hard to guess, but it is key material from a non-cryptographic PRNG — the DH
  code in `main.js` states itself that `Math.random` must never make key
  material, and uses `crypto.getRandomValues` instead.
- **Second and later random identities in the same worker (js path only):**
  `generateRSAKey` runs `Math.seedrandom`, which permanently replaces
  `Math.random` in that worker. Every later "random" passphrase was therefore
  a deterministic continuation of the previous keypair's stream — no fresh
  entropy at all. The wasm path seeds its own internal arc4 and never touches
  `Math.random`, so it does not have this problem; the js fallback still does.
- **AES message keys:** jsbn's SecureRandom pool, filled once at worker load
  from pre-hijack native `Math.random` plus a timestamp — the same ~128-bit
  grade as the first identity.

Recommended fix (not yet applied): draw `randomstring` from
`crypto.getRandomValues`, as the DH code already does. Existing identities are
unaffected — the passphrase-to-keypair mapping does not change.

## FAQ

### What is WebAssembly?

A file with a `.wasm` extension that modern browsers can load for tasks that
need near-native performance. It is commonly used for things like web-based
emulators (e.g. Flash emulators). Audio processing is hard to do efficiently in
pure JavaScript, so WebAssembly is used instead.

### Is it possible to remove them from `client.html`?

Yes — it is quite easy.

**To remove libopus:**

1. Open `client.html` in VS Code or a similar editor.
2. Locate the function `instantiate_libopus_webassembly_from_base64`.
3. Remove or comment out `libopus_webassembly_base64`.

This breaks audio sending and receiving, but text chat still works.

**To remove minimp3** (likely unnecessary anyway):

1. Locate the function `mp3_to_pcm_worker_onmessage`.
2. As with Opus, remove or comment out the WebAssembly variable.

**rsa-keygen cannot be removed.** It is the client's only RSA implementation
since the vendored JavaScript stack was deleted, so without it there is no
keypair, no login challenge and no message encryption.

### How large are they?

| Module     | Size in `client.html` (Base64-encoded) |
| ---------- | -------------------------------------- |
| libopus    | ~380 KB                                |
| minimp3    | ~30 KB                                 |
| rsa-keygen | ~31 KB                                 |

### How are these files built?

The build must be done on **Linux**, but the resulting files can be used
anywhere.

1. Download [Emscripten (emsdk)](https://emscripten.org/) and follow its setup
   instructions.
2. Extract the WebAssembly source archives shipped here and build them:
   - `libopusjs/libopus-workingbuild.tar.gz`
   - `mp3-to-pcm/minimp3-wasm.tar.gz`
3. The build produces a `.wasm` file.
4. Convert that `.wasm` into a Base64 string with the included
   [`wasmstob64.py`](wasmstob64.py) script, then paste it into `client.html`.

The archives of both projects the WebAssembly files were built from are
included in this directory.
