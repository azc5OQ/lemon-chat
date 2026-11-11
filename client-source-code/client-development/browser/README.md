# client-development

Source workspace for the chat client. The shipped client is a single
`client.html`, but it is authored here as many small files and assembled by
`build.cjs`. Choose minified or readable output when building; development sources stay readable.

## Layout

```text
client-development/browser/
    build.cjs                  # compiles/tests wasm, then assembles ../../client.html
    build.bat / build.sh       # build launchers
    client-webassemblies/      # independent WebAssembly source projects
        connection-crypto/     # symmetric ciphers and session key derivation
        rsa-keygen/            # identity and RSA operations
        libopusjs/             # Opus sources
        mp3-to-pcm/            # minimp3 sources
    main/
        template.html          # ordered source includes
        styles/                # CSS themes and layout
        wasm/                  # compiled modules embedded during page assembly
        scripts/
            vendor/            # third-party JavaScript
            app/
                main.js, globals.js, utils.js, console-log.js
                crypto/        # browser encryption wrappers and key handling
                connection/    # connection lifecycle, heartbeat and reconnect
                msg/           # protocol handlers and worker dispatch
                channel/       # channel tree, roles and client status
                chat/          # messages, attachments, replies and reactions
                audio/         # microphone, playback, music and notifications
                video/         # video transport and workers
                ui/            # controls, layout, menus and theme effects
                server/        # server settings, groups and moderation
                mobile/        # Android host and Java bridge
                workers/       # shared worker bootstrap and message handlers
```

When you want to change a cipher, open
`client-webassemblies/connection-crypto/main/crypto/`. That project owns the cipher
adapters and key-derivation code, and its `third-party/` directory owns the
Mbed TLS and LibTomCrypt dependencies it compiles. Its build does not read from
the desktop or server projects.

For browser-side encryption behavior, start in `main/scripts/app/crypto/`.
`connection-crypto.js` calls the compiled cipher module, `keys.js` manages keys,
and `rsa-crypto.js` wraps the separate RSA WebAssembly module. The JavaScript
wrappers and the C algorithm implementations have separate jobs and locations.

`main/template.html` still controls include order. Moving a file into a topic
folder does not change startup or worker initialization order. The Android Node
bridge has its own include list, updated to the same source paths.

The application splits across topic files, all sharing one closure (see the
Editing section). The big three: `scripts/app/msg/messages.js` is the protocol layer
(`server_msg` handlers + `client_msg` builders), `scripts/app/ui/ui.js` is the `UI`
object (rendering, themes, menus, dialogs), and `scripts/app/audio/audio.js` is the
audio engine (worklets, playback/capture graphs). `scripts/app/chat/chat-files.js` is
the chat file feature (any file as an encrypted message: paperclip, drag & drop,
the file card with its progress ring, the download button, and the fast base64 /
crypto helpers its worker branches use). The rest of the page lives in one file
per topic, each with its own state at the top and its functions below:

| file | what it holds |
| --- | --- |
| `globals.js` | every name two or more other files use, one group per topic; `g_` = app-wide state, `G_` = app-wide constant, no prefix = private to the file that declares it (kept at that file's top) |
| `utils.js` | the helpers every file uses: storage, sleep, the log and the toast, base64 both ways, sha256, the touch-device check |
| `workers/workers.js` | the two worker handlers, data processing and websocket (run inside those workers only) |
| `console-log.js` | the console prefixing installed in every context |
| `connection/connection.js` | the driver that decides when to dial, the dial, the heartbeat, fast reconnect, the connect-page hold, server bookmarks |
| `crypto/keys.js` | the identity keypair, dh session keys, aes message bodies, channel keys and the maintainer wait timer |
| `channel/channel-tree.js` | lookups and rendering for channels and clients, avatars |
| `chat/chat.js` | composing and sending messages, pictures and files; the typing indicator; unread counts and seen receipts |
| `mobile/android-host.js` | the headless node runtime, the webview bridge implementations, deep idle |
| `ui/layout.js` | the desktop grid layout engine |
| `audio/voice.js` | the microphone, the webrtc datachannel, the music bot stream |
| `server/server-settings-tab.js` | the admin tab: field table, policy, icon uploads, country blocking |
| `msg/dispatch.js` | `dispatch__mainthread_onmessage`, everything the data-processing worker posts back |
| `main.js` | startup: `main__window_onload` and the export seam for the node runtime |
| `mobile/android-bridge.js` | outside the closure: the `JavascriptJavaBridge__*` functions the java side calls by name |

Every top-level function carries its file's name as a prefix (`chat__send_chat_message` lives
in chat.js), the way the C server names its functions, so a call site tells you where to look.
The namespaced objects (`UI`, `server_msg`, `client_msg`, `lemon_crypto`, `webrtc`) are the
exception: the object is their prefix.

Every function and method carries a doc block in the server's layout, `@brief` (what it is,
then what it does), one `@param type name -> what` per parameter and `@return type what`
(`void` when nothing comes back). Add one to a new function; `lemonchat-study/tools/docinv.py`
lists any that lack it.

`styles/fonts.style` contains all 18 embedded font faces as Base64 WOFF2. This
keeps `client.html` smaller without removing fonts or characters. Conversion
preserves glyph outlines, spacing, hinting and font metadata. Inter, Manrope
and Plus Jakarta Sans retain their variable weights; Inter also retains its
optical size axis.

Inter, Manrope and Plus Jakarta Sans come from the SIL Open Font License editions in the
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
run the normal client build. Convert a replacement font to WOFF2 before embedding
it; FontTools with Brotli support can do this without subsetting characters.
The build itself needs no font conversion tools and embeds the existing WOFF2
bytes unchanged.

## Building

Suppose you change a chat label in `main/scripts/app/ui/ui.js`, or change a cipher
in `client-webassemblies/connection-crypto/main/crypto`. Users open `client.html`,
so those edits become visible only after rebuilding that page. The normal build
now compiles **all four WebAssembly modules from source**, tests them, and then
embeds the resulting bytes as Base64 alongside the JavaScript, CSS and assets.

### Why Zig is part of a browser build

The audio and crypto modules use C compiled to WebAssembly for speed. The build
uses **`zig cc`**, Zig's C compiler based on Clang/LLVM. The sources remain
ordinary C.

Zig includes the compiler, linker and C-library support needed to build all four
modules with one portable toolchain. It replaces the old Opus setup that required
WSL, Emscripten and CMake. The build uses **Zig 0.15.2**, the version verified
with its compatibility tests.

Node.js runs the build scripts and tests and assembles the HTML. Neither tool
is needed to run the finished page. The
[WebAssembly README](client-webassemblies/README.md) describes the modules,
their source locations and build commands.

### Start the build and obtain tools

From this directory, run `build.bat` on Windows or `sh build.sh` on Linux/macOS.
The wrappers look for Node.js 20 or newer. If it is missing, they offer to
download portable Node.js 24.14.0. The Node builder then looks for **Zig 0.15.2**,
the C compiler used for all four modules, and offers to download it if missing.
Choose minified or readable output; Terser is optional and can also be downloaded
when you want minification. Declining Terser still permits a readable build.

If the required Zig version is already on PATH or in `browser/.build-tools`,
there is nothing to download. The builder prints `Using existing Zig` with its
version and location. An explicit `ZIG` environment variable selects that
executable; an invalid value produces an error instead of replacing your choice.
Server builds use `--no-download-tools`, so they never show the download prompt.

Before compilation starts, the builder lists all four `.wasm` files and their
purpose. Each module has a `[WASM build 1/4]` heading and a completion message
showing how many modules remain. Tests have their own `[WASM test 1/4]` headings,
including separate progress messages for the longer RSA checks. Assembly and
minification are announced afterward, so test output is not mistaken for more
compilation.

Downloads stay in `browser/.build-tools`, an ignored directory separate from
source projects. Nothing is installed system-wide and no shell profile is
changed. Node and Zig archives are checked against pinned SHA-256 hashes before
extraction. Terser is installed locally through npm with lifecycle scripts
disabled and a saved package lock. The downloaders need `curl` and `tar`; the Unix
Node bootstrap also needs `sha256sum` or macOS's `shasum`. Current Windows includes
PowerShell, curl and tar. You may instead install these tools yourself. Set `ZIG`
to the **compiler executable**, not its directory, when it is outside PATH.

There is no Emscripten SDK, WSL, CMake, Ninja, Python, TypeScript or separate
wasm-opt requirement for this browser build. Zig includes its C compiler, linker
and the C-library pieces used by Opus and connection crypto. Node.js runs the
builder and tests; it is unrelated to the Android bridge here. The finished
HTML runs in a browser without Node.js or Zig.

### What one normal build does

For example, run `build.bat --unminified` after editing the chat label:

1. Locate the required tools, offering downloads when running interactively.
2. Compile Opus/Speex, minimp3, RSA and connection crypto into a temporary
   directory below `client-webassemblies/build`. Each project uses only its own
   sources and third-party libraries. Every module must have zero host imports.
3. Run the codec, cipher and RSA tests. Opus encodes a tone and decodes its own
   packets, including packet-loss recovery and resampling. MP3 decoding must give
   audible output and the same samples when a file is decoded again. RSA must
   reproduce every recorded identity vector exactly, and cipher outputs must match
   their reference vectors and OpenSSL counterparts.
4. Only after every test passes, publish the complete set into `main/wasm`.
   Compiler or test failure keeps the previous set. Builds use a lock so two
   processes cannot publish competing sets. After an interrupted build, remove
   `client-webassemblies/build/build.lock` only after confirming its builder has
   stopped, then rebuild.
5. Read `main/template.html`, insert its ordered source includes, and replace each
   WebAssembly marker with Base64 from the newly compiled modules. Check script
   syntax and the worker `THREAD_NAME` patch target.
6. For minified output, optimize CSS/assets and run Terser on inline JavaScript.
   Names, license notices, the worker patch target and headless export seam stay
   intact; code compression and dead-code removal stay disabled.
7. Replace `../../client.html` and, when present, the project-root
   `buildresult/client.html`. HTML replacement is atomic per output file. A
   compile, test, syntax or minifier failure leaves the previous HTML intact.

Reload the served page to see the changed label. Editing sources without
rebuilding leaves the existing HTML unchanged. The completion output reports
module sizes, test results, include counts and the final file size. The compiler
may reuse its internal object cache, but the source compilation step is always
invoked; normal builds do not silently reuse the shipped wasm files.

### Unattended builds and checks

```sh
# Allow missing tools to be downloaded without an interactive prompt:
sh build.sh --unminified --download-tools
# Windows equivalent: build.bat --unminified --download-tools

# With tools already installed; no network access or download prompts:
node build.cjs --minified --no-download-tools
node build.cjs --unminified --no-download-tools --output preview/client.html
node build.cjs --unminified --no-server-copy

# Syntax-only checks of existing modules; no compilation/downloads/output:
node build.cjs --check
node build.cjs --check-minifier
```

Without `--download-tools`, an unattended build never downloads anything. Missing
required tools cause a clear failure. `--no-download-tools` also disables prompts
in an interactive terminal. `--output` writes a separate HTML file without
updating either default HTML destination; a normal build still compiles and
publishes tested modules into `main/wasm`. `--no-server-copy` suppresses the
`buildresult` copy. `LEMON_CLIENT_BUILD_MODE=minified` or `unminified` chooses a
mode without prompting; command-line mode takes precedence.

`--no-minify-js` optimizes CSS/assets while leaving JavaScript readable.
`--minify-js` and `--no-optimize` remain aliases for `--minified` and
`--unminified`. On Windows the wrapper pauses only with no arguments and no
mode environment variable, and preserves the build's exit code.

### Server and Android builds

The server scripts call this same builder with downloads disabled. A missing
Node.js, missing Zig, failed module test or any other client build failure is a
warning: **the server still builds**. Its package uses the existing `client.html`
when available and warns if that page is missing or may be outdated. Neither
Node.js, Zig nor a successful browser build is a server requirement.

Both Android Gradle projects also invoke the browser builder and therefore need
its tools when rebuilding this page; their native and headless build tools keep
their own requirements. Their default mode is unminified; use
`-PclientBuildMode=minified` when Terser is available. The Android Node bundle's
separate generator consumes the browser modules already built by this process.

See [the WebAssembly projects](client-webassemblies/README.md) for their source
layout, individual build commands, tests and dependency versions.

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
markers are **not** in `template.html` â€” they live inside the two `.js` files,
mid-string-literal, because that is where the base64 sits in the library code:

- `scripts/vendor/libopus.emscripten.js` â†’ `var libopus_webassembly_base64 = '@@WASM:wasm/libopus.wasm@@';`
- `scripts/app/audio/minimp3-worker.js` â†’ `let mp3_decoder_webassembly_base64 = "@@WASM:wasm/mp3_decoder.wasm@@";`

`build.cjs` expands the `@@INCLUDE` markers first, then swaps each
`@@WASM:...@@` for `base64(the .wasm file)`.

## Editing

- Edit any file under `main/`, then rebuild.
- Keep `.js`/`.style` files **pure** â€” no `<script>`/`<style>` tags. Those live
  only in `template.html`.
- To add a new source file: create it under `main/scripts/...`, then add a
  `/* @@INCLUDE: scripts/path/to/file.js @@ */` line in `template.html` at the
  spot it should load. Order matters â€” files are concatenated top to bottom.
- **The includes share one closure, not just one `<script>` tag.** A wrapper
  opens inside `scripts/vendor/aes-js.js` and closes at the very END of
  `scripts/app/main.js` (its final `}));`). Every file spliced between those two
  lives inside that closure and sees `main.js`'s variables (that is how
  `audio/audio.js` works); a file placed outside that range silently sees none of
  them. Corollary: nothing in the app is a `window` global â€” `typeof X` in
  devtools says `undefined` for everything, which is normal, not breakage. Also
  never brace-balance-check one file alone; only the whole assembled script
  element parses.
- To replace a wasm binary: drop the new `.wasm` into `main/wasm/` and rebuild.

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
(`g_is_websocket_connected`, `local_username`, `current_channel_id`, â€¦). A few
globals deliberately stay unprefixed: names that also appear as message-object
keys or as locals elsewhere (`local_username`, `current_channel_id`, `host`,
`port`, â€¦) â€” renaming those was judged riskier than the inconsistency.

## Notes

- Libraries split across source files are concatenated in template order.
- `client-webassemblies/` holds WebAssembly sources and rebuild instructions.
  The browser build embeds binaries from `main/wasm/` automatically; no separate
  base64 conversion script is needed.

## Shared conversation-list features

`main/scripts/app/chat/member-conversations.js` adds name/channel search, previews of
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
