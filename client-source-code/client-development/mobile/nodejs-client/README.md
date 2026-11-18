# why Node.js for the Android client

At first the idea was to wrap client.html that web client uses in
Android's WebView, customize and tweak it until it just worked, with functionality such as calls
routed through the JS-Java bridge.

The downside was that client.html had to run constantly in the background, and so did the WebView.
Keeping a WebView alive in the background required an ugly hack: attaching it to an invisible 0x0
window, which produced a permanent "lemon chat is displaying over other apps" alert in the
notifications. Surprisingly, client.html stayed connected even over long periods - but the alert
was always there.

The user also had to grant the draw-over-other-apps permission, which made setup more complicated.

Moving the core websocket functionality into Java would have split the codebase further and made
everything even more complicated than it already was. So the choice fell on a JavaScript
environment without HTML - Node.js for Android - which can run in the background without needing
to be "displayed over other apps" and without the WebView, while reusing the existing client's
JavaScript files.

When the app actually needs to render something, the Node.js environment hands the data to it. On
Android it is therefore NOT client.html that speaks to the server over the websocket - it is the
Node.js environment. client.html connects to a loopback websocket served by Node and receives the
same protocol replayed; Node holds the one real connection, permanently.

# what is in this directory

- `build-node.py` + `node-template.js` - glue the client sources into `bundle.js` (same @@INCLUDE
  mechanism as client.html's build.cjs). Run after changing any client source.
- `dom-shim.js` - headless stand-ins for the browser globals, plus the node platform flags.
- `load-bundle.js` - loads the bundle the way a browser would (vm, not require).
- `websocket-adapter.js` + `vendor/ws/` - pinned ws 8.23.0 client and server using its pure JavaScript
  implementation. The sources, MIT license and verified checksums are included; APK builds work offline.
- `transport-limits.js` - explicit control, authentication, frame and queue capacities.
- `android-main.js` - entry point inside the android service.
- `android-bridge-client.js` - JSON-lines TCP bridge to the java side (settings in, events out).
- `loopback-ui-server.js` - the loopback websocket the ui connects to instead of the real server.
- `loopback-transfer.js` - scoped download admission, chunk window, acknowledgements and expiry.
- `fetch-nodejs-mobile.py` + `nodejs-mobile-runtime.json` - download and verify the pinned Node 24 mobile runtime, its matching headers, and license notice
- `build-windows.bat` - wraps build-node.py.
- `test/` - transport and shared bridge regressions. Run `node test/run.cjs` from this directory.
  The browser FileReader test requires Playwright and Node 20 or newer; Node 18 reports that browser-only
  harness as skipped while running the application runtime tests.

## Reattaching the page

When Android closes a page while a message arrives, Node retains the message in a bounded memory queue.
The next authenticated page first receives the current identity, membership, policy and key state. It
then receives pending content and acknowledges successful processing. A socket write alone does not
remove a message from that queue. A replaced page cannot issue commands or acknowledge its successor's
messages. Opening the page does not reconnect a session that the user deliberately disconnected.

Retained content keeps the original sender's public identity, including after its numeric slot is
reused. Channel text, headers and bodies are prepared by Node before retention, so replay does not
depend on a channel key that has since rotated. File and picture headers remain with their pending
body. If page processing fails, that content is kept for a replacement page; dependent edits/deletes
wait for the original's acceptance while unrelated content continues.

Reading a conversation is a separate action from accepting its content. The page includes the last
delivery ID it processed for that conversation. Node clears its unread count only when that ID is the
latest acknowledged delivery in the current session. For example, if Maya sends another message while
the page's earlier read request is travelling to Node, that earlier request cannot clear the new message.

This queue is not disk storage. Process death loses it; replacing the remote session expires old session
entries. Reaching the capacity stops remote intake with an error instead of silently accepting more.
For example, a file received while the page is closed is kept as prepared content in Node. When the page
returns, a large content frame first asks the page to reserve space. Node then sends at most four
unacknowledged chunks, each containing 48 KiB of raw bytes encoded as at most 64 KiB of base64. The page
acknowledges each chunk; after reconstructing and processing the original frame, it separately acknowledges
the content delivery. Accepting the last chunk alone does not remove the pending content.

Local transfer admission allows at most eight transfers, each no larger than 48 MiB plus 64 KiB of frame
metadata, and a combined 64 MiB plus eight metadata allowances. A transfer expires after two minutes.
Page replacement cancels its transfers; rejected content is retained for the next page, and the current
file or picture card receives a terminal failure instead of remaining stuck on receiving.

Content crypto still constructs complete base64 frames, and the receiver reconstructs complete JSON.
These chunk limits bound the local hop, not the application's total heap. The lower-level 144 MiB
WebSocket budget can carry the server's 100 MiB file representation, but the existing file receiver still
has its separate 48 MiB encoded-file limit. This is not a claim of 100 MiB Android file support or safe
heap usage on every device. Device memory and ABI testing remain separate from desktop transport tests.

## Building and checking dependencies

The Android Gradle build prepares the pinned full Node 24.22.1 mobile runtime automatically. Its 112.9 MB archive is cached outside the checkout. Only the selected device architectures go into the APK. Existing Node 18 files under the old source folders are not used or deleted.

This runtime comes from the separate `fogtape/nodejs-mobile` fork and is marked as a prerelease. It uses Node 24.22.1 and does not include the complete [Node 24.22.3 security update](https://nodejs.org/en/blog/release/v24.22.3), published on 11 November 2026. That update fixes HTTP, HTTP/2, TLS and other runtime issues. The client uses Node HTTP and, for secure WebSockets, HTTPS/TLS, so the patch gap remains relevant even though chat messages have their own encryption.

Replacing the JavaScript bundle or Java bridge cannot supply fixes to native Node code and compiled dependencies. Closing this gap requires rebuilt mobile runtime libraries with matching headers, new verified hashes, and compatibility checks. The selected prerelease is not described as fully patched or production ready.

For an offline build, pass Gradle `-PnodeRuntimeArchive=/path/to/nodejs-mobile-android-24.22.1-0.zip`. The same checksum is required. `LEMON_NODE_RUNTIME_CACHE` selects another archive cache directory. A changed or incomplete generated runtime is rejected instead of mixing its libraries and headers with another version.

Run the downloader checks with `python -B -m unittest discover -s tests -p test_fetch_nodejs_mobile.py`. These do not download the actual runtime. `python fetch-nodejs-mobile.py --metadata` prints the pin, and `--check --output-dir PATH --abis arm64-v8a,armeabi-v7a` verifies an existing installation without writing or downloading.

Run `python build-node.py` to generate the Node bundle. The builder verifies each vendored ws source
against `vendor/ws-lock.json`. `python build-node.py --list-inputs` prints the exact template, include,
Wasm and dependency paths used by Gradle's incremental build check.

Transport tests cover arbitrary UTF-8 splits, invalid control commands, reconnect state, WebSocket
authentication bounds, binary upgrade leftovers, moderator close codes, page replacement, acknowledged
delivery, a complete 100 MiB representation through the adapter, and the full bundle's remote upload
backpressure. The chunk tests exercise real sender/receiver integration, byte-ACK versus content-ACK
ordering, Unicode and binary fidelity, bounded admission, expiry and terminal failures. Fresh-bundle
event tests cover ignore state, unread counts, actual notification attribution and reset completion.
