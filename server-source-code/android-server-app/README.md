# Lemon Chat Android server

Host a server on your phone. Connect over your local network, or enable Tailscale Funnel to give visitors a public HTTPS address.

## Build

Open this folder in Android Studio. You need SDK 36, Build Tools 35.0.0, NDK 27.2.12479018, CMake 3.22.1, JDK 17+, Python 3 and Go 1.27.1 or later. Python and Go must be in PATH. The first build downloads the pinned Go dependencies.

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

Audio defaults to WebRTC. Choose WebSocket fallback if UDP is blocked, or WebSocket only to force it. Audio stays encrypted. Funnel mode selects WebSocket for both audio and video because Funnel does not forward UDP.

Slots default to 16; the range is 1–100,000. On ARM64, each slot reserves about 10 KB, plus about 42 MiB of fixed tables. **100,000 slots need roughly 1 GiB before active connections and Android's own memory use.** The app shows an estimate. Of course, who would want to run a 100,000-slot server on a phone?

If something fails, open **View log**. Guest Wi-Fi may block connections between phones.

## Public access through Tailscale Funnel

- Stop the server and open **Settings**. Enable **Public access through Tailscale Funnel** and save.
- Start the server. Tap **Sign in to Tailscale** if the app asks. Authorize the device in your browser, then return to Lemon Chat.
- Your Tailscale account must have MagicDNS, HTTPS certificates and Funnel permission enabled. The **Tailscale setup help** button opens the official instructions.
- Wait for **Hosting publicly**, then copy the HTTPS address. Visitors can open it without installing Tailscale.
- In the web client, use the displayed hostname and **WSS port 8443**. Enter the join password under **Server keys** if you set one. The public page does not contain that password.
- **Stop server** closes the public routes too. Your Tailscale sign-in stays in the app's private storage for the next start. To revoke it, remove the device in the Tailscale admin console.

HTTPS on port 443 serves the bundled `client.html`. WSS on port 8443 forwards chat, audio and video to the server's WebSocket port. The HTTP listener uses port 8080, or 8081 if WebSocket already uses 8080. LAN access remains available.

The app includes a separate Tailscale process using `tsnet`. It does not need root, a VPN permission or the separate Tailscale Android app. Android may still stop hosting in the background. Funnel also has bandwidth limits, so it is not a replacement for a fast VPS when streaming to many people.

Funnel connections currently appear as `127.0.0.1` to this Android server. Multiple connections from that address are enabled automatically. Do not use IP bans to distinguish visitors in this mode.

## Android certificate support

The build pins Tailscale to `v1.104.1`. That version excludes its certificate API on Android, which prevents `tsnet` Funnel from obtaining a certificate. `funnel/build.py` applies the two build-constraint changes proposed in [upstream PR 21720](https://github.com/tailscale/tailscale/pull/21720) to a separate build copy. It leaves the downloaded dependency unchanged and checks the expected version and source before applying the change.

The ARM64 debug APK builds, and local HTTP and WebSocket forwarding checks pass. This integration still needs a live Android sign-in and public-access check. A successful build alone does not establish that a particular phone or account can host through Funnel.
