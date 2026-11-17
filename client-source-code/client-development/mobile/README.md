# Android clients

- `android/`: Android Studio / Gradle application, with the WebView UI and embedded Node.js service.
- `nodejs-client/`: shared browser protocol code bundled for nodejs-mobile.
- `android-native/`: native Java views, protocol and audio, without Node.js or WebView.
- `../browser/`: shared browser sources; they remain outside the mobile folder.

Both Android apps remain available. The hybrid app keeps `com.lemonchat`, so upgrading its runtime preserves its existing identity and settings. The native app uses `com.lemonchat.nativeclient` and can be installed beside it. Its [README](android-native/README.md) describes its own build and feature coverage.

Open `android/` in Android Studio to build the hybrid app. Gradle prepares the pinned Node runtime, runs the browser and Node.js builders, and packages their outputs automatically.

```powershell
cd android
./gradlew.bat assembleDebug
```

The SDK is configured in `android/local.properties` or the usual Android SDK environment variables. Java 17 or newer is required. Gradle uses NDK 27.3.13750724 and keeps minimum Android API 26. Accept the SDK licenses before building so missing SDK components can be installed.

The embedded runtime is the full Node 24.22.1 mobile prerelease from `fogtape/nodejs-mobile`, with its archive, libraries, headers and dependency notices pinned by SHA256. It is a mobile fork, not the ordinary desktop executable. Updating desktop Node does not update the Android runtime. The pinned mobile build must be updated separately when its security patches become available.

The default APK includes ARM64 and ARMv7. For an ARM64-only APK use `-PandroidAbis=arm64-v8a`. For an x86_64 emulator use `-PandroidAbis=x86_64`. Unsupported architectures fail before packaging. An existing archive can be selected with `-PnodeRuntimeArchive=/path/to/archive.zip`, and its pinned checksum is still checked.

Native libraries are stripped and compressed in the APK without removing runtime features. Android extracts the selected architecture, so installed storage includes the APK plus that extracted library set. `-PcompressNativeLibraries=false` keeps them uncompressed for direct loading. Selecting one device architecture also avoids shipping the other architecture's library.

Build only the headless bundle with `python nodejs-client/build-node.py`.
The browser output remains `client-source-code/client.html`. Downloaded runtime files stay in the Android build directory. Old Node 18 files under `src/main/jniLibs` and `cpp/nodejs-mobile-include` are no longer build inputs.

See [communication and ownership](android/COMMUNICATION.md) for the Java, Node and page boundaries,
identity replacement, call actions, reconnect and acknowledged page delivery. The page still performs
content/media cryptography and capture; Node owns the remote chat-control session.

Run `node test/run.cjs` inside `nodejs-client/` for the transport and generated-bundle checks.
The maintained WebSocket dependency is vendored with its license and verified checksums, so building
the APK does not fetch a new dependency version. Pending message handoff is bounded memory storage;
it does not guarantee offline delivery after the Android process is killed.
