# client-development

Source workspace for the chat client. The shipped client is a single
`client.html` (~4.6 MB), but it is authored here as many small files and glued
back together by `build.py`.

## Layout

```
client-development/
├─ build.py            # glues src/ -> src/client-build.html
├─ build.bat           # Windows: runs build.py, then pauses
├─ build.sh            # Unix:    runs build.py
├─ _split.py           # one-time bootstrap (see below)
├─ client-webassemblies/   # Opus/minimp3 .wasm *sources* + how to rebuild them
│  ├─ README.md            # what the two modules are, build/removal steps
│  ├─ wasmstob64.py        # converts a built .wasm into the embedded base64
│  ├─ libopusjs/           # libopus source archive (.tar.gz)
│  └─ mp3-to-pcm/          # minimp3 source archive (.tar.gz)
└─ src/
   ├─ template.html    # the HTML shell: <head>, <body>, the <style>/<script>
   │                   # tags, and one /* @@INCLUDE: ... @@ */ marker per source
   ├─ styles/          # *.style  -> pure CSS (no <style> tags)
   ├─ scripts/
   │  ├─ vendor/       # third-party libs, one file each (aes-js, jsbn, rsa,
   │  │                # cryptico, js-sha256, libopus, minimp3, ...)
   │  └─ app/          # our code (utils, encoding, audio glue, main, ...)
   └─ wasm/            # *.wasm  -> real binaries (libopus, mp3_decoder)
```

`scripts/app/main.js` is the bulk of the application. `styles/fonts.style` is
large because the fonts are base64-embedded.

## Building

Run any of:

```
build.bat        (Windows)
./build.sh       (Linux/macOS)
python build.py
```

Output is written to `src/client-build.html` (deleted and regenerated every
run). To ship, copy it over the real `../client.html`. The output path is the
`OUT` variable near the top of `build.py` if you want to change it.

## How the glue works

`template.html` is the skeleton. Wherever a source file belongs, it has a marker
on its own line:

```html
<script type="text/javascript">
/* @@INCLUDE: scripts/vendor/aes-js.js @@ */
/* @@INCLUDE: scripts/app/main.js @@ */
</script>
```

`build.py` replaces each marker line with that file's exact bytes. The marker is
a `/* ... */` block comment, which is valid in both CSS and JS, so editors do
not flag it.

The two WebAssembly binaries are re-encoded to base64 at build time. These
markers are **not** in `template.html` — they live inside the two `.js` files,
mid-string-literal, because that is where the base64 sits in the library code:

- `scripts/vendor/libopus.emscripten.js` → `var libopus_webassembly_base64 = '@@WASM:wasm/libopus.wasm@@';`
- `scripts/app/minimp3-worker.js` → `let mp3_decoder_webassembly_base64 = "@@WASM:wasm/mp3_decoder.wasm@@";`

`build.py` expands the `@@INCLUDE` markers first, then swaps each
`@@WASM:...@@` for `base64(the .wasm file)`.

## Editing

- Edit any file under `src/`, then rebuild.
- Keep `.js`/`.style` files **pure** — no `<script>`/`<style>` tags. Those live
  only in `template.html`.
- To add a new source file: create it under `src/scripts/...`, then add a
  `/* @@INCLUDE: scripts/path/to/file.js @@ */` line in `template.html` at the
  spot it should load. Order matters — files are concatenated top to bottom.
- To replace a wasm binary: drop the new `.wasm` into `src/wasm/` and rebuild.

## Key configuration & state (`scripts/app/main.js`)

The most useful knobs live near the **top of `scripts/app/main.js`** (around
lines 922–1018). Start here when wiring the client to a server.

**Connection / autoconnect:**

| variable | meaning |
| --- | --- |
| `are_server_details_predefined` | `true` = use `autoconnect_details` below instead of prompting the user for host/port/keys. |
| `is_autoconnect_without_user_action_active` | `true` = connect on page load with no click. Caveat: browsers block audio until a user gesture, so if you open `client.html` straight from disk and the mic/AudioContext fails, set this to `false` (a single click on Connect is enough). Not an issue in Android WebView / Electron / embedded pages. |
| `is_reconnect_active` | `true` = keep retrying / reconnecting after a drop. |
| `autoconnect_details` | the predefined target: `{ host, port, keys: [...] }` (channel keys). |

**Core data structures (the live state of the app):**

| variable | meaning |
| --- | --- |
| `client_list[]` | connected clients |
| `channel_list[]` | channels |
| `icons[]`, `tags[]` | server icons and tags |
| `map_client_id_to_array_index` | `Map` from a client id to its index in `client_list` |

**Identity / crypto:** `dh_generator` / `dh_modulus` (2048-bit safe prime) for
Diffie-Hellman key exchange, and `my_rsa_key_object` / `rsa_public_key_string`
for the client's RSA identity.

Below these, the same block declares the rest of the per-session state
(`is_websocket_connected`, `local_username`, `current_channel_id`, …).

## Notes

- Many libraries were originally concatenated *inside* a single `<script>` tag.
  The split cuts them apart at their own headers; the build just concatenates,
  so the result is identical to the original byte-for-byte.
- `_split.py` is the one-time bootstrap that created `src/` from the original
  `../client.html`. It is kept for reference / re-splitting and reads the
  pristine `../client.html` (never modified). You normally do **not** run it —
  use `build.py`.
- `client-webassemblies/` holds the *sources* for the two embedded `.wasm`
  binaries (libopus, minimp3), the `wasmstob64.py` converter, and a README on
  how to rebuild them. The build does **not** touch this folder — the binaries
  it actually embeds are the ones under `src/wasm/`. Only come here when you
  need to recompile a `.wasm` from scratch. See `client-webassemblies/README.md`.
