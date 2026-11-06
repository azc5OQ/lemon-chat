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

To turn edits in `src/` into a new `client.html`, the build script uses Node.js
to assemble files, embed WebAssembly and validate JavaScript. Node.js is needed
for this build step independently of the Android runtime. The finished page
runs in a browser without Node.js. Server builds skip this step when Node.js is
missing, and continue even if an attempted client build fails. They package the
existing `client.html` if available, with a warning that it may be outdated.
Neither Node.js nor a successful client rebuild is required to build the server.

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

### Example: rebuild after changing the client

Suppose you change a message label in `src/scripts/app/ui.js`. The server serves
`client.html`, so editing that source file alone does not update the page people
open. From `client-source-code/client-development/browser`, run:

```sh
node build.cjs --unminified
```

The builder reads `src/template.html`, inserts the files named by its include
markers, and replaces WebAssembly markers with base64 from the existing binary
files in `src/wasm`. It checks each assembled script's syntax and the worker
`THREAD_NAME` patch target. It does not compile the WebAssembly sources; use the
[WebAssembly guide](client-webassemblies/README.md) when those binaries need rebuilding.

In readable mode, the assembled HTML is written without CSS or JavaScript
minification. In minified mode, the builder optimizes CSS and embedded assets,
then runs Terser on the applicable inline scripts. It checks the resulting
JavaScript syntax, worker patch declaration and headless export seam before
writing. These checks do not replace testing the page in a browser.

The default result is `client-source-code/client.html`. If the project-root
`buildresult` directory already exists, the builder also updates
`buildresult/client.html`. Reload the page served by your server to see the new
label. Without rebuilding, the existing HTML still contains the old label.
The completion messages show the output path, mode, include and WebAssembly
counts, and the assembled and final byte sizes.

### Output files and failures

Use `--output preview/client.html` with a mode to try a build without replacing
the default page or server copy. Relative output paths are resolved from your
terminal's current directory. `--no-server-copy` updates the default page but
leaves `buildresult/client.html` alone. The builder creates missing parent
directories for the selected output; it does not create `buildresult` just to
make the automatic server copy.

Each output is written to a temporary file beside its destination and renamed
into place. Assembly, syntax or minification errors occur before output writes,
so those failures leave the previous output intact. The default page and server
copy are written separately: if writing the server copy fails, the default page
may already have been updated. Check the reported error and output paths before
assuming both copies match.

A standalone client build returns a nonzero exit code on failure. Common causes
are missing Node.js, missing input files, invalid JavaScript, unavailable Terser,
an invalid option or mode, and an unwritable output destination. For a missing
minifier, install Terser or select `--unminified`. For an unattended build, select
a mode explicitly; the builder does not silently choose one when it cannot prompt.
`--check` validates assembly without writing files or requiring Terser;
`--check-minifier` checks Terser separately without building the page.

### Building the server when the client cannot be rebuilt

Suppose the server machine has the C/C++ build tools but no Node.js. The Windows,
Linux and macOS server scripts warn, skip the client rebuild and continue to the
server build. If Node.js exists but the client build returns an error, they print
a warning and continue in the same way. This includes syntax, minifier, mode
selection and output-write errors. The client's standalone failure exit code is
preserved; the server scripts handle it as an optional build step.

During packaging, the scripts copy `client-source-code/client.html` if it exists.
After a skipped or failed rebuild, that file may be an older client and may not
match your edited sources. If it is absent, the scripts warn that the bundled
HTTP server has no page to serve and still continue. You can build the client on
another machine and place its `client.html` beside the server later. Native server
build failures still fail the server build normally.

To avoid the mode prompt during a server build, set the environment variable
before invoking the server script. In Windows Command Prompt:

```bat
set "LEMON_CLIENT_BUILD_MODE=unminified"
windows_build_script.bat
```

In PowerShell:

```powershell
$env:LEMON_CLIENT_BUILD_MODE = "unminified"
.\windows_build_script.bat
```

On Linux, from `server-source-code`:

```sh
LEMON_CLIENT_BUILD_MODE=unminified bash linux_build_script.sh
```

Use `macos_build_script.sh` on macOS. Set the value to `minified` when Terser is
installed and you want minification. If that request fails, the server scripts
warn and use the existing page; they do not silently rebuild in a different mode.

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
