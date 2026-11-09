# Checked prebuilt browser modules

If you cannot compile the WebAssembly projects, this directory supplies a checked
module set that works with the current browser JavaScript. It is maintained
separately from normal source builds. Normal builds and the tool downloader never
overwrite it.

From the browser directory, run:

```sh
node build.cjs --unminified --prebuilt-wasm --output preview/prebuilt-modules.html
```

The builder verifies `sha256.json`, runs `check.cjs`, and then embeds these modules.
The checks exercise the actual browser RSA operations and secret clearing, MP3
decoding and input limits, all six connection ciphers, and the matching Opus
loader's audio and allocation cleanup. A missing export or failed regression
stops the build before writing the page. To run only these checks from this
directory, use `node check.cjs`.

This option skips C compilation and leaves `main/wasm` untouched. Use the normal
build command again to compile current sources. This set is not an untouched
historical snapshot: security fixes must also reach the explicit prebuilt path.

## Module provenance

On 2026-11-07, `rsa_keygen.wasm` and `mp3_decoder.wasm` were copied byte for byte
from the tested `main/wasm` outputs. RSA now exports the mandatory
`rsa_keygen__clear_sensitive` function used by the current client. MP3 includes
the 64 MiB encoded-input limit and checked buffer-size arithmetic. The previous
RSA and MP3 modules must not be restored as a fallback for these sources.

`connection_crypto.wasm` is unchanged. `libopus.wasm` remains the earlier Opus
1.5.2 binary and requires `libopus.emscripten.js`; it cannot use the current
source-built module's runtime. That matching loader now frees partial resampler
allocations and preserves existing buffers if a replacement allocation fails.
Keep the Opus binary and its loader together.

`sha256.json` records the current checked files. For provenance, the replaced
files previously had these SHA-256 values:

| File | Previous SHA-256 |
| --- | --- |
| `rsa_keygen.wasm` | `399c92211ea2df898a36348ac25e8dcfe48237a8486d7dc3a00780292873a5fd` |
| `mp3_decoder.wasm` | `f077b7e027926cdf267273337d61cdb81290c7f08ba67a6bb7c9d207d47f6cff` |
| `libopus.emscripten.js` | `db6a76bcedaa11737889cdb7c323fb93c3d265861d7a7ba367e17c09dd2b403f` |
