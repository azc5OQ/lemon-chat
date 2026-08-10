# client-development

Source workspace for the chat client. The shipped client is a single
`client.html` (~4.6 MB), but it is authored here as many small files and glued
back together by `build.py`.

## Layout

```
client-development/
├─ build.py            # glues src/ -> ../client.html (the shipped client)
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
   │  └─ app/          # our code (utils, encoding, audio glue, audio, sounds, messages, ui, main, ...)
   └─ wasm/            # *.wasm  -> real binaries (libopus, mp3_decoder)
```

The application splits across four files, all sharing one closure (see the
Editing section): `scripts/app/main.js` is the spine (startup, workers,
dispatchers, crypto helpers, app lifecycle), `scripts/app/messages.js` is the
protocol layer (`server_msg` handlers + `client_msg` builders), `scripts/app/ui.js`
is the `UI` object (rendering, themes, menus, dialogs), and `scripts/app/audio.js`
is the audio engine (worklets, playback/capture graphs). `styles/fonts.style` is
large because the fonts are base64-embedded.

## Building

Run any of:

```
build.bat        (Windows)
./build.sh       (Linux/macOS)
python build.py
```

Output is written straight to `../client.html` at the repo root — the shipped
client itself — deleted and regenerated every run. There is no separate
`client-build.html` step anymore; the build *is* the ship. The output path is
the `OUT` variable near the top of `build.py` if you want to change it.

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
- **The includes share one closure, not just one `<script>` tag.** A wrapper
  opens inside `scripts/vendor/aes-js.js` and closes at the very END of
  `scripts/app/main.js` (its final `}));`). Every file spliced between those two
  lives inside that closure and sees `main.js`'s variables (that is how
  `audio.js` works); a file placed outside that range silently sees none of
  them. Corollary: nothing in the app is a `window` global — `typeof X` in
  devtools says `undefined` for everything, which is normal, not breakage. Also
  never brace-balance-check one file alone; only the whole assembled script
  element parses.
- To replace a wasm binary: drop the new `.wasm` into `src/wasm/` and rebuild.

## Key configuration & state (`scripts/app/main.js`)

The most useful knobs live near the **top of `scripts/app/main.js`** (around
lines 922–1018). Start here when wiring the client to a server.

**Connection / autoconnect:**

| variable | meaning |
| --- | --- |
| `g_are_server_details_predefined` | `true` = use `g_autoconnect_details` below instead of prompting the user for host/port/keys. |
| `g_is_autoconnect_without_user_action_active` | `true` = connect on page load with no click. Caveat: browsers block audio until a user gesture, so if you open `client.html` straight from disk and the mic/AudioContext fails, set this to `false` (a single click on Connect is enough). Not an issue in Android WebView / Electron / embedded pages. |
| `g_is_reconnect_active` | `true` = keep retrying / reconnecting after a drop. |
| `g_autoconnect_details` | the predefined target: `{ host, port, keys: [...] }` (channel keys). |

**Core data structures (the live state of the app):**

| variable | meaning |
| --- | --- |
| `g_client_list[]` | connected clients |
| `g_channel_list[]` | channels |
| `g_icons[]`, `g_tags[]` | server icons and tags |
| `g_map_client_id_to_array_index` | `Map` from a client id to its index in `g_client_list` |

**Identity / crypto:** `g_dh_generator` / `g_dh_modulus` (2048-bit safe prime) for
Diffie-Hellman key exchange, and `g_my_rsa_key_object` / `g_rsa_public_key_string`
for the client's RSA identity.

Below these, the same block declares the rest of the per-session state
(`g_is_websocket_connected`, `local_username`, `current_channel_id`, …). A few
globals deliberately stay unprefixed: names that also appear as message-object
keys or as locals elsewhere (`local_username`, `current_channel_id`, `host`,
`port`, …) — renaming those was judged riskier than the inconsistency.

## Notes

- Many libraries were originally concatenated *inside* a single `<script>` tag.
  The split cuts them apart at their own headers; the build just concatenates,
  so the result is identical to the original byte-for-byte.
- `_split.py` is the one-time bootstrap that created `src/` from `../client.html`.
  It is kept for reference / re-splitting. Note that `build.py` now overwrites
  `../client.html` on every run, so it is no longer a pristine original — the
  untouched first version lives in git history. You normally do **not** run
  `_split.py` — use `build.py`.
- `client-webassemblies/` holds the *sources* for the two embedded `.wasm`
  binaries (libopus, minimp3), the `wasmstob64.py` converter, and a README on
  how to rebuild them. The build does **not** touch this folder — the binaries
  it actually embeds are the ones under `src/wasm/`. Only come here when you
  need to recompile a `.wasm` from scratch. See `client-webassemblies/README.md`.
