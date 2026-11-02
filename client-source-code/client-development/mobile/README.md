# Mobile client

- `android/`: Android Studio / Gradle application, with the WebView UI and embedded Node.js service.
- `nodejs-client/`: shared browser protocol code bundled for nodejs-mobile.
- `../browser/`: shared browser sources; they remain outside the mobile folder.

Open `android/` in Android Studio. Building its APK runs the browser and Node.js
builders and copies their outputs into the app's assets automatically.

```powershell
cd android
./gradlew.bat assembleDebug
```

The SDK is configured in `android/local.properties`; Java 17 or newer is required
by the Android build tools. The existing project uses JDK 21 and NDK 27.2.12479018.
If the embedded Node binaries are missing, run
`python nodejs-client/fetch-nodejs-mobile.py` from this directory first.

Build only the headless bundle with `python nodejs-client/build-node.py`.
The browser output remains `client-source-code/client.html`. The Node runtime
download still targets the adjacent `android/` project.
