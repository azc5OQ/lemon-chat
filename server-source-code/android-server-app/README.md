# Lemon Chat Android server

Host a server on your phone. Friends use the Lemon Chat Android client app on the same Wi-Fi.

## Build

Open this folder in Android Studio. You need SDK 36, Build Tools 35.0.0, NDK 27.2.12479018, CMake 3.22.1, JDK 17+ and Python 3.

```powershell
.\gradlew.bat :app:assembleDebug
```

On Linux/macOS, use `sh ./gradlew :app:assembleDebug`.

APK: `app/build/outputs/apk/debug/app-debug.apk`

Requires Android 8.0+ and a 64-bit ARM phone. Add `-PserverAbis=arm64-v8a,armeabi-v7a` when building to include 32-bit ARM phones.

## Use

1. Install the APK and connect to Wi-Fi.
2. Tap **Start server**. Set an admin password and an optional join password.
3. Wait for **Hosting**.
4. In the client app's Settings, enter the displayed IP and websocket port. Add the join password under **Server keys**, or leave the keys empty. Save and connect.

Keep the admin password private. The client asks you to change it when you first become admin. On the host phone, connect to `127.0.0.1` using the same port.

**Stop server** disconnects everyone. Android may stop background apps, so keep an eye on the hosting notification.

## Settings

**Advanced** contains the port, slots, audio connection and video switch. Stop the server before changing them. Settings are saved in the app's private `server_settings.json`.

Audio defaults to WebRTC. Choose WebSocket fallback if UDP is blocked, or WebSocket only to force it. Audio stays encrypted; a poor connection may add delay. Each audio WebSocket adds about 38 KiB of buffers and two threads. Video still uses WebRTC. Clients need the updated build.

Slots default to 16; the range is 1–100,000. On ARM64, each slot reserves about 10 KB, plus about 42 MiB of fixed tables. **100,000 slots need roughly 1 GiB before active connections and Android's own memory use.** The app shows an estimate. Of course, who would want to run a 100,000-slot server on a phone?

If something fails, open **View log**. Guest Wi-Fi may block connections between phones.

Build and process tests passed. A live session between Android phones still needs testing.
