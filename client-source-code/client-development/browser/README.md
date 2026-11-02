# client-development

Source workspace for the chat client. The shipped client is a single
`client.html`, but it is authored here as many small files and assembled by
`build.cjs`. Choose minified or readable output when building; development sources stay readable.

## Layout

```
client-development/browser/
├─ build.cjs            # glues src/ -> ../../client.html (the shipped client, in client-source-code)
├─ build.bat           # Windows: runs build.cjs, then pauses
├─ build.sh            # Unix:    runs build.cjs
├─ client-webassemblies/   # Opus/minimp3 .wasm *sources* + how to rebuild them
│  ├─ README.md            # what the two modules are, build/removal steps
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

The application splits across topic files, all sharing one closure (see the
Editing section). The big three: `scripts/app/messages.js` is the protocol layer
(`server_msg` handlers + `client_msg` builders), `scripts/app/ui.js` is the `UI`
object (rendering, themes, menus, dialogs), and `scripts/app/audio.js` is the
audio engine (worklets, playback/capture graphs). `scripts/app/chat-files.js` is
the chat file feature (any file as an encrypted message: paperclip, drag & drop,
the file card with its progress ring, the download button, and the fast base64 /
crypto helpers its worker branches use). The rest of the page lives in one file
per topic, each with its own state at the top and its functions below:

| file | what it holds |
| --- | --- |
| `globals.js` | every name two or more other files use, one group per topic; `g_` = app-wide state, `G_` = app-wide constant, no prefix = private to the file that declares it (kept at that file's top) |
| `utils.js` | the helpers every file uses: storage, sleep, the log and the toast, base64 both ways, sha256, the touch-device check |
| `workers.js` | the two worker handlers, data processing and websocket (run inside those workers only) |
| `console-log.js` | the console prefixing installed in every context |
| `connection.js` | the driver that decides when to dial, the dial, the heartbeat, fast reconnect, the connect-page hold, server bookmarks |
| `keys.js` | the identity keypair, dh session keys, aes message bodies, channel keys and the maintainer wait timer |
| `channel-tree.js` | lookups and rendering for channels and clients, avatars |
| `chat.js` | composing and sending messages, pictures and files; the typing indicator; unread counts and seen receipts |
| `android-host.js` | the headless node runtime, the webview bridge implementations, deep idle |
| `layout.js` | the desktop grid layout engine |
| `voice.js` | the microphone, the webrtc datachannel, the music bot stream |
| `server-settings-tab.js` | the admin tab: field table, policy, icon uploads, country blocking |
| `dispatch.js` | `dispatch__mainthread_onmessage`, everything the data-processing worker posts back |
| `main.js` | startup: `main__window_onload` and the export seam for the node runtime |
| `android-bridge.js` | outside the closure: the `JavascriptJavaBridge__*` functions the java side calls by name |

Every top-level function carries its file's name as a prefix (`chat__send_chat_message` lives
in chat.js), the way the C server names its functions, so a call site tells you where to look.
The namespaced objects (`UI`, `server_msg`, `client_msg`, `lemon_crypto`, `webrtc`) are the
exception: the object is their prefix.

Every function and method carries a doc block in the server's layout, `@brief` (what it is,
then what it does), one `@param type name -> what` per parameter and `@return type what`
(`void` when nothing comes back). Add one to a new function; `lemonchat-study/tools/docinv.py`
lists any that lack it.

`styles/fonts.style` contains the embedded fonts. The existing fonts use base64
TrueType data. Inter, Manrope and Plus Jakarta Sans use base64 WOFF2 data to
reduce the size of `client.html`. All three retain their full character sets and
variable weights. Inter also retains its optical size axis.

These fonts come from the SIL Open Font License editions in the
[Google Fonts repository](https://github.com/google/fonts). Each font declaration
includes its source URL, source revision, original file hash, copyright notice
and complete license. The notices are included in the built client as well.
No font is downloaded when someone opens the client.

| Theme | Interface font | Usernames and headings | Timestamps |
| --- | --- | --- | --- |
| Default, Mobile, Termix, Simpledark | Inter | Inter | Inter |
| Ozone | Inter | Manrope | Inter |
| Pocket, Bluebell | Plus Jakarta Sans | Plus Jakarta Sans | Inter |
| Aero, Hybrid V1, Hybrid V2 | Existing theme fonts | Existing theme fonts | Existing theme fonts |

The theme's `--set-font` controls settings, dialogs, alerts, controls and lists.
`--chat-author-font` and `--chat-meta-font` control message names and timestamps.
Custom message font choices remain separate. To replace an embedded font,
update its base64 data, weight range and source notice in `fonts.style`, then
run the normal client build. The build itself needs no font conversion tools.

## Building

Install a maintained LTS version of [Node.js](https://nodejs.org/) and reopen
your terminal. A readable build needs only Node.js. To enable minification,
install [Terser](https://terser.org/docs/cli-usage/) yourself:

```sh
npm install --global terser
```

Run `build.bat` on Windows, `./build.sh` on Linux/macOS, or `node build.cjs`.
Choose **1. Minified** or **2. Unminified**. When Terser is unavailable, option 1
is shown as **disabled** and cannot be selected. The build never installs tools
or downloads packages. Node.js and Terser are not bundled with this project.

Both modes assemble `src/` into `../../client.html` and refresh an existing
`buildresult/client.html`. Source files stay readable and unchanged. Output
files are replaced only after assembly and validation succeed; a missing or
failed minifier leaves the previous output intact.

Minified output keeps the existing size savings: repeated CSS images are stored
once, TrueType fonts are losslessly packaged as WOFF, and CSS whitespace is
compacted. Terser 5 or newer removes JavaScript whitespace and ordinary comments.
Names, license notices, the worker patch target and the headless export seam
are preserved. Code compression and dead-code removal stay disabled.

For scripts and CI, choose explicitly:

```sh
node build.cjs --unminified
node build.cjs --minified
node build.cjs --minified --output preview/client.html
node build.cjs --unminified --no-server-copy
node build.cjs --check
node build.cjs --check-minifier
```

`--output` builds an isolated file without updating the shipped or server copy.
`--check` checks assembled JavaScript syntax and the worker patch target without
writing output. `--check-minifier` reports whether an installed Terser can run.
The build looks for a resolvable Terser package or the global npm installation.

Server build scripts use the same builder. Set
`LEMON_CLIENT_BUILD_MODE=unminified` or `minified` to skip the prompt;
command-line options take precedence. A build without a terminal must specify
its mode. Both Android Gradle projects default to unminified output. Use
`-PclientBuildMode=minified` after installing Terser to request minification.
Their separate native and headless build tools retain their own prerequisites.

`--no-minify-js` optimizes CSS/assets while keeping JavaScript readable.
`--minify-js` and `--no-optimize` remain aliases for `--minified` and
`--unminified`. The Windows wrapper pauses only when launched interactively
without arguments or an environment-selected mode and preserves the exit code.

## How the glue works

`template.html` is the skeleton. Wherever a source file belongs, it has a marker
on its own line:

```html
<script type="text/javascript">
/* @@INCLUDE: scripts/vendor/aes-js.js @@ */
/* @@INCLUDE: scripts/app/main.js @@ */
</script>
```

`build.cjs` replaces each marker line with that file's exact bytes. The marker is
a `/* ... */` block comment, which is valid in both CSS and JS, so editors do
not flag it.

The two WebAssembly binaries are re-encoded to base64 at build time. These
markers are **not** in `template.html` — they live inside the two `.js` files,
mid-string-literal, because that is where the base64 sits in the library code:

- `scripts/vendor/libopus.emscripten.js` → `var libopus_webassembly_base64 = '@@WASM:wasm/libopus.wasm@@';`
- `scripts/app/minimp3-worker.js` → `let mp3_decoder_webassembly_base64 = "@@WASM:wasm/mp3_decoder.wasm@@";`

`build.cjs` expands the `@@INCLUDE` markers first, then swaps each
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

## Key configuration & state (`scripts/app/globals.js`)

The most useful knobs live in the **"connection and session" group at the top of
`scripts/app/globals.js`**. Start here when wiring the client to a server.

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

- Libraries split across source files are concatenated in template order.
- `client-webassemblies/` holds WebAssembly sources and rebuild instructions.
  The browser build embeds binaries from `src/wasm/` automatically; no separate
  base64 conversion script is needed.

## Shared conversation-list features

`src/scripts/app/member-conversations.js` adds name/channel search, previews of
messages already retained in the client, their displayed send times, and optional
navigation from the member list into the selected chat. It uses the existing member
mirror and chat contexts, with no new server protocol or separate history store.
Previews update when messages arrive, change or are removed. A conversation without
retained messages shows its channel or presence instead of an invented preview.

Themes opt in with `--member-conversations: 1` on `:root`. To also reveal chat when
a member or channel is selected, set `--member-conversations-auto-open: 1`.
`styles/member-conversations.style` supplies the common controls and hides them when
the active theme does not opt in. Each theme can style the search, optional profile,
`.member-conversation-preview` and `.member-conversation-time` to suit its own layout.
No theme names are checked in the JavaScript module.

Mono Light and Mono Dark are registered as mobile themes. Both share
`styles/theme-mono.style`; `styles/theme-mono-dark.style` only changes the palette.
Use the half-light/half-dark theme control to select either palette. The bottom
People/Chat control switches between the full people list and the conversation;
a tap on a person opens their chat, while a hold retains the existing actions menu.
