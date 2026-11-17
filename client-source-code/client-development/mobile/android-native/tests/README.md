# Reproducing the host checks

These checks let you verify identity compatibility, encrypted messages and Opus audio before installing the Android app. They build the actual JNI library for the development computer and run it inside a host JVM. They do not test Android audio devices, lifecycle or hardware-backed storage.

`ServerAddressChecks` adds 35 pure Java checks for the separate address and port form. It covers saved endpoint compatibility, IPv6, exact unchanged history-scope text, request paths and invalid input. `AutomaticTransportChecks` adds 43 checks using real plain and TLS listeners, including certificate and hostname rejection, upgrade refusal, cancellation during TLS setup and concurrent application sends during a ping-triggered close. These checks passed independently and run through the protocol test runner. The runner creates a temporary localhost certificate using the selected JDK's `keytool`; only the test JVM trusts that certificate.

The complete runner passed after automatic transport was added, including its existing 191 protocol, feature, call, tree and ordering checks and bounded retry checks. The actual Lemon server run exercised encrypted login, messages, attachments, idle return and reconnect through the automatic transport path. Physical Android verification of this latest transport change is separate from the storage results below.

`MusicStreamHostProbe` adds 20 passing checks for current-song metadata, initial playback, start/stop events, channel visibility and stale title removal. It is included in the protocol runner. `MusicTitleChecks` adds 14 Android view assertions for native marquees, recycled rows, member strips and bot click behavior. These view assertions compile but still need the next device run.

## Tools and inputs

The commands below were verified on Windows with a 64-bit JDK 21, Python 3.11, CMake, Ninja, the w64devkit GCC toolchain and Node 24. The JDK and native compiler must target the same architecture. The host build does not use the Android NDK.

Audio and protocol source compilation also needs `android.jar` from Android SDK platform 36. Browser comparison checks need the existing browser sources and built `rsa_keygen.wasm`, `connection_crypto.wasm` and `libopus.wasm` in `browser/main/wasm`. Generate missing browser binaries with the existing `browser/client-webassemblies/build.cjs` build and its documented toolchain. Node is used only by these compatibility checks. It is not packaged in the Android app.

Put these Maven artifacts in one external dependency folder:

| Artifact | Version used by the host checks |
| --- | --- |
| [org.java-websocket:Java-WebSocket](https://repo.maven.apache.org/maven2/org/java-websocket/Java-WebSocket/1.6.0/Java-WebSocket-1.6.0.jar) | 1.6.0 |
| [org.slf4j:slf4j-api](https://repo.maven.apache.org/maven2/org/slf4j/slf4j-api/2.0.16/slf4j-api-2.0.16.jar) | 2.0.16 |
| [org.slf4j:slf4j-nop](https://repo.maven.apache.org/maven2/org/slf4j/slf4j-nop/2.0.16/slf4j-nop-2.0.16.jar) | 2.0.16 |
| [org.json:json](https://repo.maven.apache.org/maven2/org/json/json/20250517/json-20250517.jar) | 20250517 |

The real JSON jar must precede `android.jar` at runtime because Android's SDK jar contains throwing stubs. The Python runners set that order. The JSON jar is a host testing dependency and is not added to the Android APK.

## Build the host JNI library

Open PowerShell in this `android-native` directory. Edit the tool and output paths for your computer. Keep generated files outside the repository.

```powershell
$nativeProject = (Get-Location).Path
$checkRoot = "D:/lemon-native-checks"
$jdk = "D:/Java/jdk-21.0.12+8"
$sdk = "D:/androidsdk"
$toolchain = "C:/tools/w64devkit/bin"
$node = "C:/tools/node/node.exe"
$dependencies = "$checkRoot/dependencies"
$hostBuild = "$checkRoot/host"
$androidJar = "$sdk/platforms/android-36/android.jar"
$server = "C:/path/to/lemon-chat/buildresult/chat-server.exe"
$env:JAVA_HOME = $jdk
$env:PATH = "$toolchain;$env:PATH"
cmake -S "$nativeProject/tests/host" -B $hostBuild -G Ninja "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" "-DCMAKE_C_COMPILER=$toolchain/cc.exe" "-DCMAKE_CXX_COMPILER=$toolchain/c++.exe" "-DCMAKE_MAKE_PROGRAM=$toolchain/ninja.exe"
cmake --build $hostBuild --parallel 4
```

The result is `$hostBuild/lemonnative.dll`. It includes the same `crypto.cmake`, `audio.cmake`, `transport.cmake`, JNI wrappers and bundled crypto, Opus and WebRTC sources as the Android build. Windows links BCrypt and the required Windows socket libraries. Unix builds use the existing platform branches and keep the platform library filename, but those host platforms have not been verified here. CMake 4 needs the quoted minimum-policy option for the bundled dependency projects.

Use a new build folder after moving the repository or changing native compiler or JDK architecture. An old CMake cache contains absolute source and tool paths.

## Identity and encrypted content

```powershell
python tests/run-crypto-tests.py --jdk $jdk --dependencies $dependencies --native-library $hostBuild --node $node --output "$checkRoot/crypto"
```

This generates six fixtures using the actual browser RSA Wasm and crypto code, runs `test/CryptoSelfTest.java` against the native library, then asks the browser code to decrypt the native results. It checks identity digests, public keys, `.lmn` files, channel/private content, authenticated files and rejected inputs. The verified result is 130 native checks plus both browser comparison passes. Output goes to `crypto-tests.log` and generated TSV fixtures in the selected output directory.

## Opus, browser compatibility and server audio

```powershell
python tests/run-audio-tests.py --jdk $jdk --android-jar $androidJar --dependencies $dependencies --native-library $hostBuild --server $server --node $node --output "$checkRoot/audio"
```

This checks the real native codec, stereo music encoding, six resampling rates and continuity across decoder chunks. It decrypts native encrypted Opus using the actual browser code and decodes it with browser Opus Wasm, then sends encrypted audio between two native peers through a disposable server. The live checks cover WebSocket, WebRTC and a mixed WebRTC sender with a WebSocket receiver. WebRTC uses the server's announced STUN port. Counter boundaries include values beyond JavaScript's exact Number range. Omitting `--node` explicitly skips the browser comparison only.

The live probe also compares independently copied media keys with the original chat context. It holds the chat lock while media encryption runs, closes the chat context while keeping media keys alive, and checks invalid handles, bounded allocation, retired handles and tampered frames. The copied keys must be closed separately when their audio connection is retired.

The runner also performs twelve audio worker checks. It fills the ordinary queue, verifies that teardown still fits, drains work in order, waits for the inactive thread to retire, restarts it and checks that shutdown rejects new jobs.

The server executable must come from this repository and have its normal runtime libraries available beside it or on `PATH`. The runner uses a fresh temporary settings directory and free local port. It stops its own server on exit and removes that temporary directory. It does not change an existing server's settings. Results are in `audio-tests.log` and `server.log`.

Native codec checks and synthetic browser envelopes verify codec and packet compatibility. Live server checks exercise the wire protocol. Physical microphone quality, routing and battery use require separate device tests.

## Protocol checks

```powershell
python tests/run-protocol-tests.py --jdk $jdk --android-jar $androidJar --dependencies $dependencies --native-library $hostBuild --server $server --output "$checkRoot/protocol"
```

This runs the existing protocol checks with an isolated server. Omit `--server` to run only the checks that do not require a connection. The separate `--demo` option intentionally keeps a demonstration fixture running and is not part of the automated verification sequence.

## Android build and instrumentation

The baseline verification on 2026-11-15, before optional history and theme additions, built the debug APK, test APK and minified release APK for ARM64, ARMv7 and x86_64. Android lint passed with zero errors and 30 warnings. The native libraries passed Android API 26 import checks, platform-only dependency checks and JNI export checks. The 64-bit libraries and APK passed 16 KiB alignment checks. ARMv7 uses 4 KiB ELF alignment.

To build an isolated debug verification app and its checks in the normal project build folders:

```powershell
.\gradlew.bat :app:assembleDebug :app:assembleDebugAndroidTest :app:assembleRelease :app:lintDebug -PnativeVerificationInstall=true --no-daemon --max-workers=2
```

The property gives debug builds the application ID `com.lemonchat.nativeclient.checks` and label **Lemon Native Checks**. Namespace and JNI names stay unchanged. Normal builds and release builds keep their original application ID. This separate installation also separates Android Keystore keys, preferences, databases and attachment caches from the user's app. The suite temporarily changes its target app's preferences and starts its service, so run it against this verification installation on a real phone.

Connect a test device or start an emulator, then substitute its authorized ADB serial below. The instrumentation uses the actual Android Keystore, SQLite, image decoder and MP3 decoder. It does not replace those implementations with host stubs.

Use a disposable test installation for this suite. The UI and service fixtures temporarily change preferences, replace visible session state, finish the Activity and stop its service. They restore saved preferences, but cannot restore an existing unsaved draft or live in-memory conversation. On a personal phone, use a separate validation application ID or test the normal app manually. Do not run the suite against an installation containing conversations you want to keep.

```powershell
$adb = "$sdk/platform-tools/adb.exe"
$serial = 'emulator-5580'
& $adb -s $serial install -r app/build/outputs/apk/debug/app-debug.apk
& $adb -s $serial install -r app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk
& $adb -s $serial shell am instrument -w -r com.lemonchat.nativeclient.checks.test/com.lemonchat.storage.StorageInstrumentation
```

The earlier Android 15 emulator baseline passed **140 checks**, listed below. The expanded storage and theme checks are tracked separately.

The latest optional-history and theme changes have passed complete debug APK, Android test APK, minified release APK and lint builds.

| Area | Passed |
| --- | --- |
| Keystore and encrypted archive | 45 |
| Image decoding and bounds | 13 |
| Audio decoding, settings, callback retirement and counters | 13 |
| Local settings | 17 |
| Native chat rendering and avatar revisions | 24 |
| Background wake lease and recovery eligibility | 24 |
| Installed call task configuration | 4 |

On 2026-11-15, the expanded storage suite passed **139 assertions on a Moto G84 running Android 14** in the isolated verification installation:

| Physical storage area | Passed |
| --- | --- |
| Keystore and encrypted archive | 45 |
| History consent, expiry, migration and erasure | 54 |
| Failed preference commits and rollback | 19 |
| Service history ordering and clear races | 21 |

These include overlapping archive coverage from the emulator baseline, so the two totals should not be added as distinct checks. The physical run ended with `INSTRUMENTATION_CODE: -1`; its local log is `D:/lemon-native-evaluation/native-moto-storage.log`. To run only this storage suite:

```powershell
& $adb -s $serial shell am instrument -w -r -e suite storage com.lemonchat.nativeclient.checks.test/com.lemonchat.storage.StorageInstrumentation
```

Successful output ends with `INSTRUMENTATION_CODE: -1`. Storage unit checks use separate preference files and a separate database. The additional UI and service fixtures require the disposable installation described above. Temporary media records and attachment files are removed after their checks. A storage-only run does not verify theme rendering, first-run dialogs, opened attachments or call notification actions. Run the corresponding suites for those checks.

The four call configuration checks inspect the installed manifest. They do not prove that Answer or Decline returns to the right app. Actual notification navigation, screen-off call delivery, Doze, manufacturer restrictions, microphone quality, Bluetooth and physical-device battery use remain separate device checks.

No downloaded jars, generated fixtures, native binaries, test server data or build caches need to be committed.

### Mobile message rows

The chat-feature host probe covers image-only and file-only quote descriptions, caption preservation, encrypted quote JSON and emoji catalogs. `MessageRowChecks.verify(Instrumentation)` runs 37 Android layout assertions. It checks incoming and outgoing avatars, an explicit avatar opt-out, sender/time headers in Mobile 1 and Mobile 2, quote spacing and width at a narrow measured size, a valid PNG attachment and unchanged footers in the other five themes.

`EmojiImageChecks.verify(Instrumentation)` runs seven Android assertions for a valid custom GIF above the former base64 limit, cached thumbnails after encoded-source eviction, deletion and connection replacement. `ServerAddressChecks` runs 35 host assertions with the fresh-form port set to 1111.

### Group and permission editing

Run the permission UI host checks with a JDK and the same `org.json` jar used by the protocol tests:

```text
python tests/run-permission-ui-tests.py --jdk <jdk-directory> --json-jar <json-jar> --output <temporary-output-directory>
```

The script compares the native catalog with the actual C action/power arrays, channel-role support, default-action rules and browser labels. It then runs 35 checks for isolated editing drafts, single-field changes, numeric bounds, invalid input, advanced flags, source-value preservation and serialization. Channel actions retain explicit Enable or Disable values rather than a default the server would normalize. No server or native library is required.

`ThemeChecks` invokes `GroupAdminUiChecks.verify(Activity)` and `AdministrationChecks.verify(MainActivity)` on the Android main thread inside its isolated activity fixture. The checks create unstarted service objects with immutable test snapshots, so they make no network requests and change no preferences. They measure phone and tablet panes, keyboard-safe editor controls, search, unsent edits, insufficient editing power, administrator-only flags, omitted values and stale targets.

## Native administration checks

`AdminHostChecks` runs with the protocol host suite. It covers immutable permission definitions, numeric and flag validation, partial settings bodies, delegated group access, forced password change, actual acknowledgement matching, stale group and client identity rejection, queued edit invalidation and connection reset. The optional credentials-file argument runs against a disposable administrator-enabled server. Never point that mutation probe at a server containing settings or groups you need to preserve.

The isolated host run passed **53 checks**, including administrator login, General settings load/save/reload and restoration, server-group creation, permission editing, renaming and deletion, plus channel-role creation and deletion. It verifies the actual server catalog counts of 58 actions and 26 powers, and channel counts of 11 and 12. The separate group draft suite passed **35 checks** and compares its labels/default rules with current source catalogs.

`AdministrationChecks` and `GroupAdminUiChecks` run inside the existing isolated `ThemeChecks` activity fixture. The General checks cover password view-state exclusion and clearing, delegated access, compact Save, field validation, draft preservation, forced replacement and connection replacement. Main and Android test Java compilation passes. These new view checks have not yet executed on a device.


The administration checkpoint also passes the ARM64 verification app/test builds, normal three-ABI minified release and release lint. Native API 26 imports and 16 KiB APK/ELF alignment pass. These build results do not substitute for the pending administration view checks on a device.
