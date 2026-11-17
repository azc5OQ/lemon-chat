# Native bridge regression checks

When a connection is replaced or the Android activity closes, an older callback must not overwrite the new state. These checks run the production Java transport/settings classes and lifecycle/call/storage methods against small host-side Android fixtures. They use synthetic identities and temporary files; they do not read application preferences or contact a server.

Run from the repository root with Python 3 and a JDK (Java 8 or newer):

```text
python client-source-code/client-development/mobile/android/test/run.py
```

The runner finds the JDK through `JAVA_HOME`, then `javac` on `PATH`. Each check can also run individually.

- `state_transport_test.py`: concurrent first-run initialization, partial sound/microphone updates, failed commits, identity revision/operation checks, size preservation, UTF-8 limits, and real loopback sockets with a silent authentication candidate and connection replacement.
- `callback_test.py`: teardown before binding, absent/failing WebViews, queued settings after pause, and callbacks from a replaced document.
- `call_test.py`: activity reuse, current caller/capability, activity-first acceptance, and stale/duplicate/expired/wrong-session native actions.
- `publication_test.py`: successful MediaStore publication and insert/open/write/update/cleanup failures, including a zero-row publication update.
- `settings_action_test.py`: successful and failed native Save do not request Connect. Trusted Connect and Disconnect actions work independently of the page socket. Queued Connect cannot cross document replacement or request permission after its activity hides.
- `connection_wizard_test.py`: the final Connect waits for service binding and a resumed activity, Cancel does not connect, Advanced settings waits until resume, and first setup does not bypass identity recovery or replace a saved connection.
- `permission_chain_test.py`: first setup asks no unrelated permission, Connect offers notifications and an optional battery explanation, refusal allows connection, and rotation preserves prompt decisions. Microphone permission is requested by actual page capture.
- `foreground_service_test.py`: boot starts only messaging, hidden or untrusted pages cannot admit capture, recording permission and Android foreground admission are both required, and document retirement rejects stale microphone results. A retained page can restore its earlier WebView grant only after returning visibly.
- `runtime_assets_test.py`: production runtime extraction closes its input when destination creation fails, removes obsolete managed files, and preserves runtime data.
- `file_save_test.py`: missing/invalid native file data, duplicate names, partial-write and close failures, unavailable storage, and optional DownloadManager registration failure on the legacy path.

The state/transport suite also checks atomic logging preference commits, terminal runtime-failure
handling after queued status events, consumed-call replay cleanup, and retained forced-idle visibility.
The real socket cases retain one current Accept/Decline through a disconnected control channel,
retry after a lost result, ignore stale results and discard the action on replacement or original expiry.
Presence replays before the action so an older hidden state cannot undo an accepted call.
Connect/Disconnect tests half-close the native socket output to force a real production-writer failure
after enqueue. The latest intent must survive, retry with its original command ID and clear only on its
matching Boolean result; an older acknowledgment cannot erase a newer choice.
The callback suite includes task removal with an absent or throwing page: its native forced-idle
command and required superclass callback must still run.

The native recheck adds concurrent settings-publication ordering and retirement of cached incoming
calls after disconnection or terminal runtime failure. The runtime extraction suite verifies failed
destination-open cleanup, removal of obsolete managed code and preservation of unrelated runtime data.

For example, the socket test keeps one unauthenticated connection silent, authenticates another, then replaces that authenticated connection. It checks that the replacement receives 25 queued writes in order and that the older reader's cleanup cannot clear it. Without independent bounded authentication and connection ownership, the silent peer blocks login or an old cleanup removes the new writer.

The socket/UTF-8 checks use the real JDK. Android storage, views and activities are modeled. The settings JSON fixture stores object snapshots by tokens, so its checks cover field ownership and commit ordering, not Android JSON parser behavior. The lifecycle fixtures splice current production methods into the test class on every run.

`gradlew.bat :app:testDebugUnitTest` also runs the connection draft and typed-password tests against the real JSON library. They check every supported cipher, legacy AES normalization, ordered layers, invalid addresses and ports, the fresh port 1111 default, and the connection-only patch that leaves identity and other settings unchanged. Draft retention across rotation and the displayed wizard still require an Android UI check.

Compile against the real Android API and build both configured native ABIs separately:

```text
cd client-source-code/client-development/mobile/android
gradlew.bat :app:compileDebugJavaWithJavac :app:externalNativeBuildDebug
```

Use `./gradlew` on Unix. Real WebView origin/CSP behavior, permission prompts, service recreation, notification display, provider behavior and runtime pressure still require Android device tests. A successful host check or APK build does not replace those tests.

`HybridWindowInsetsTest` checks API 35 window padding with the actual Android inset classes. It covers system bars, cutouts, keyboard height, repeated installation without accumulated padding, and the unchanged layout on older Android versions. Run it through the ordinary AndroidJUnitRunner in the isolated verification installation.

After `:app:assembleDebug`, run `python test/verify_apk.py` from the Android project directory. It checks the exact runtime asset set, compares every packaged runtime file and the page to the generated source outputs, verifies the vendored WebSocket checksums/license, rejects the obsolete `mini-ws.js` and any packaged device capability, and checks both configured native ABIs. An optional first argument selects another APK.

For a selected architecture, pass the same list to `verify_apk.py --abis arm64-v8a`. Use `--runtime-dir` when the Gradle build directory was redirected. The verifier compares every allocated Node ELF section with the pinned SDK after stripping, including dynamic exports, and checks 16 KiB alignment for 64-bit libraries. It also verifies the packaged Node dependency notice.

`python test/runtime_prepare_test.py --archive /path/to/verified-sdk.zip` runs the real Gradle preparation task with a fresh output directory, then repeats it to check immutable cache reuse. `--work-dir` selects temporary storage and `--abi` selects a supported architecture. This catches Gradle creating the install destination before the downloader can publish it.

`EmbeddedRuntimeTest` executes the packaged Android library through the production JNI entry point. It checks RSA Wasm, the `.lmn` key size and four deterministic identity fingerprints recorded on Node 18, real encrypted channel/private/offline content, the production WebSocket override and a real loopback packet exchange. Its data stays in a temporary cache directory. The test manifest selects a separate process so a scheduled background service cannot start Node before the test does:

```text
gradlew.bat -PandroidAbis=x86_64 assembleDebug assembleDebugAndroidTest
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb install -r app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk
adb shell am instrument -w -e class com.lemonchat.EmbeddedRuntimeTest,com.example.lemonchat.ExampleInstrumentedTest com.lemonchat.test/androidx.test.runner.AndroidJUnitRunner
adb shell am instrument -w -e class com.lemonchat.ServiceRuntimeTest com.lemonchat.test/com.lemonchat.ServiceRuntimeTestRunner
```

The second command runs the real production service in its normal process. It waits for Node's UI port to arrive through the authenticated Java bridge. It does not invoke Node a second time or measure handshake latency during startup key generation. Use a test installation because the service follows its normal startup behavior and saved connection settings.

Use an ARM ABI instead on a matching device. A desktop Node pass does not execute the embedded Android runtime, and an x86_64 emulator does not establish ARM hardware or 16 KiB device behavior.

For a phone that may already contain Lemon Chat, build a separate verification installation. It uses `com.lemonchat.hybridverification` and the label `Lemon Hybrid Verification`, leaving the normal application's identity and settings untouched:

```text
gradlew.bat -PverificationBuild=true -PandroidAbis=arm64-v8a assembleDebug assembleDebugAndroidTest
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb install -r app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk
adb shell am instrument -w -e expectedArchitecture arm64 -e class com.lemonchat.EmbeddedRuntimeTest,com.example.lemonchat.ExampleInstrumentedTest com.lemonchat.hybridverification.test/androidx.test.runner.AndroidJUnitRunner
adb shell am instrument -w -e class com.lemonchat.ServiceRuntimeTest com.lemonchat.hybridverification.test/com.lemonchat.ServiceRuntimeTestRunner
```

The verification build uses the same runtime and app code. Its Java namespace stays unchanged because the JNI entry points and component class names use that namespace. Only the installed package, label and instrumentation targets change. Its service can keep running after the service check, so stop or remove this verification installation when testing is finished.

## Real connection and background check

`LiveClientTest` opens the actual native settings form, enters an address and port separately, saves it, and connects through the production Node service. It sends a message through the real WebView composer and waits for a server message ID. It then moves the activity into the background, removes its own task, and reopens the page to check that the same authenticated server session survived.

Use a disposable server with no join key and at least two other connected clients. The test refuses every installation except `com.lemonchat.hybridverification`. For example, if the host server listens on port 51908, forward phone port 29111 to it:

```text
adb reverse tcp:29111 tcp:51908
adb shell pm grant com.lemonchat.hybridverification android.permission.POST_NOTIFICATIONS
adb shell am instrument -w -e class com.lemonchat.LiveClientTest -e serverPort 29111 com.lemonchat.hybridverification.test/com.lemonchat.ServiceRuntimeTestRunner
```

The phone must be unlocked. Granting notification permission is necessary on Android 13 and later. This focused test skips the initial permission questionnaire. It does not test permission prompts, microphone capture, long screen-off operation or battery use. Task removal uses Android's own `finishAndRemoveTask` API and waits for the service's task-removal path. A manual Recents gesture remains a separate visual check.

Add `-e incoming true` to exercise real background message and call notifications. When the output prints `HYBRID_BACKGROUND_READY username=...`, send that synthetic user a private message from another client. After `HYBRID_CALL_READY`, call the same idle user. Each phase waits at most 45 seconds. The test checks the received call capability and declines it through the production service. It does not fabricate a notification or claim an accepted voice call.

`HybridIncomingPeer.java` can supply those two real events. Compile it against the native client's existing host protocol classes and dependencies, then run `com.lemonchat.protocol.HybridIncomingPeer` with the local `ws://127.0.0.1:<port>/` endpoint and the instrumentation output file. Start it before instrumentation, using an absent or empty log. It creates a synthetic identity in memory, waits for the printed phases, confirms the private message acknowledgement, and keeps the calling peer connected until the phone finishes. It refuses remote endpoints and a changed recipient identity. The helper uses the real encrypted protocol and does not control the phone.

Four phone screenshots are saved in the verification app's cache. A successful result prints their directory. The test restores its original connection settings and preserves its identity, then disconnects and closes its activity. Notification permission is intentionally managed outside instrumentation because revoking it can terminate the test process. Restore that permission afterward if it was granted only for this test. Both the live instrumentation and peer helper compile against their actual production classes, and the ARM64 verification test APK builds successfully. Their live results are not yet verified.

Run `node --test test/webview-policy.test.cjs` from the Android project for the source policy check and
a real Chromium check of blocked frames/objects and working inline scripts, Blob workers and Wasm.
The browser case needs Node 20 or later and Playwright; it reports a test skip when those tools are
unavailable. `LEMON_PLAYWRIGHT_PATH` can select the `playwright-core` module and `LEMON_BROWSER_PATH`
can select the browser executable. This desktop check does not establish installed WebView behavior.

On Windows, run `python test/build_freshness_test.py` after a successful `:app:assembleDebug` to check
the content-based build guard. It advances one generated Wasm input's timestamp without changing its
bytes, verifies that Gradle keeps the Node bundle up to date and that the fingerprint still passes,
then restores the original timestamp. This covers a normal browser rebuild that emits identical
Wasm bytes; a timestamp-only check incorrectly rejects that valid bundle.
