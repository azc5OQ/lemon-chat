# Lemon Chat Android server

Host a server on your phone and connect over your local network. **Or try more advanced options** opens a choice of public-access guidance, other connection methods and startup settings.

## Build

Open this folder in Android Studio. You need SDK 36, Build Tools 35.0.0, NDK 27.2.12479018, CMake 3.22.1, JDK 17+, Python 3, Node.js, Zig 0.15.2 and Go 1.27.1 or later. Python, Node.js and Go must be in PATH. Zig can be in PATH or selected through `ZIG`. The first build downloads the pinned Go dependencies. The bundled browser page is built from the current browser source, including its four WebAssembly modules.

```powershell
.\gradlew.bat :app:assembleDebug
```

On Linux/macOS, use `sh ./gradlew :app:assembleDebug`.

APK: `app/build/outputs/apk/debug/app-debug.apk`

For device checks beside an existing installation, add `-PverificationBuild=true`. This creates the separate debug app **Lemon Server Checks** with its own private settings and data. Normal builds keep `com.lemonchat.server`. Use `-PserverBuildRoot=D:/lemon-server-build` to put Gradle and native outputs on another drive.

For first-run wizard checks without disturbing that verification server, also add `-PverificationSuffix=setupchecks`. This produces **Lemon Server Setup Checks** in a second isolated package. `funnelchecks` creates **Lemon Funnel Checks** for a separate synthetic public-access fixture. Only these three suffixes are accepted, and only for verification builds. Installing a verification APK does not enable public access.

Requires Android 8.0+ and a 64-bit ARM phone. Add `-PserverAbis=arm64-v8a,armeabi-v7a` when building to include 32-bit ARM phones.

## Device checks

The isolated checks app also has a public-access guide test. Run its instrumentation with `-e suite guide` on an unlocked device. It checks the actual four-step screens, Back and rotation while leaving hosting preferences unchanged. It never starts the server, signs into Tailscale or publishes an address.

`-e suite funnel-startup` separately runs the installed helper with fresh private state and temporary loopback listeners. It contacts Tailscale only to obtain sign-in instructions, validates the login hostname without logging the transient link, and stops before account authorization. It removes its own temporary state afterward. Passing this test proves helper startup and receipt of sign-in instructions, not working HTTPS certificates or public connections.

Build `:app:assembleDebug :app:assembleDebugAndroidTest -PverificationBuild=true` and install both APKs only into the isolated verification package. Copy disposable settings and their referenced music snapshot into that app's private files before running the test. The runner refuses the ordinary application ID and refuses to take over a server that is already hosting.

```text
adb shell am instrument -w -r -e hold_seconds 180 com.lemonchat.server.checks.test/com.lemonchat.server.ServerChecks
```

The test starts the real foreground service, stops it, restarts it and prints `ANDROID_SERVER_READY`. During the bounded hold, a separate client can verify restored channels, music bots and encrypted audio. The test stops its server afterward. These service checks can run independently of the visible screen. They do not verify the settings UI or public Funnel access.

The host packet regression compiles the shared server writers and checks root and ordinary channel parents after JSON serialization. From this folder, run:

```sh
cc -std=c11 -O1 -ffunction-sections -fdata-sections native/tests/channel-parent-wire-test.c ../main/msg/server/server_msg_channel.c ../third-party/dave-g-json/cJSON.c -Wl,--gc-sections -o /tmp/channel-parent-wire-test
/tmp/channel-parent-wire-test
```

On Windows, use GCC and choose an output path for the executable.

## Use

1. Install the APK and connect to Wi-Fi, or enable a hotspot for your friends.
2. On the first launch, follow the five setup steps. Choose an admin password, decide whether friends need a join password and its type, and choose the maximum number of connected clients.
3. Review your choices and tap **Start server**.
4. Wait for **Hosting**.
5. In the client app's Settings, enter the displayed IP and websocket port. Add the join password with the same type under **Server keys**, or leave the keys empty. Save and connect.

Each setup screen shows **Step X of 5**, with **Back** and **Next** controls. Nothing is written until you choose **Start server**. The defaults are 16 client slots, port 1111, WebRTC audio and video off. Port, audio transport and video choices are available under **Advanced settings** on the review screen.

Step 4 offers 8, 16, 32 and 64 slots. Enter another value, such as 500, under **Custom slot count**. The review uses the number you entered, within the server's 1–100,000 range.

If you require a join password, choose its **Password type** in step 3. AES-256 is selected by default. ChaCha20, Camellia-256, Twofish-256, Serpent-256 and Trivium-80 are also available. Share both the password and its type with your friends. For example, a password created as Twofish-256 must also use Twofish-256 in the client's Server keys. Entering the same text as AES-256 will not connect. Trivium-80 is a fixed-IV compatibility option. The admin password in step 2 is separate and does not use this selector.

Existing or imported configurations skip first setup. The wizard also refuses to replace settings that appear while it is open. Rotating the phone keeps the unfinished draft in memory. Closing setup discards it, and passwords are not put in saved Android screen state.

Keep the admin password private. In the client, tap the lock icon in the upper toolbar to sign in as administrator. The client asks you to change the password when you first become admin. On the host phone, connect to `127.0.0.1` using the same port.

**Stop server** disconnects everyone. Android may stop background apps, so keep an eye on the hosting notification.

The **Instructions** page explains joining from another device, connecting from the hosting phone and advanced options. It keeps these steps separate from the main server controls.

## Settings

**Advanced** contains the port, slots, audio connection, video switch and optional Tailscale public access. Stop the server before changing them. Settings are saved in the app's private `server_settings.json`.

The Android app accepts settings files up to 32 MiB to bound Java memory use. Larger imported files are refused without overwriting them. Music snapshots are separate files and do not count toward this limit.

If saved music exists, change the join password through a connected client's server settings while hosting. The server can then save the music with the new password. The Android startup form refuses that password change because editing only the settings file would leave the saved songs encrypted with the old password.

Audio defaults to WebRTC. Choose WebSocket fallback if UDP is blocked, or WebSocket only to force it. Audio stays encrypted. Funnel mode selects WebSocket for both audio and video because Funnel does not forward UDP.

Slots default to 16; the range is 1–100,000. The app asks the installed native executable for array sizes and includes the saved channel capacity in its estimate. For example, importing settings with 500 client slots and 300 channel entries reserves memory for both arrays, even before anyone connects. Active connections, stored music and Android need additional memory, so start with a small slot count on a phone.

If something fails, open **View log**. The log records the native process exit code and whether a stop was requested. Guest Wi-Fi may block connections between phones.

## Public access through Tailscale Funnel

Suppose your friends are outside your Wi-Fi network and cannot reach the local address. Open **Or try more advanced options**, then **Public address with Tailscale**. The optional guide has four steps and leaves hosting unchanged until you explicitly start it.

1. Read what public access means. Anyone can reach the public address, while your server keys and permissions still apply.
2. Prepare your Tailscale account. The guide links its DNS and Access controls pages for MagicDNS, HTTPS certificates and Funnel permission. Ask the account administrator if you cannot change these. See the official [Funnel requirements](https://tailscale.com/docs/features/tailscale-funnel).
3. If your local server is running, explicitly stop it first. Choose **Enable public access and start**, confirm, then use **Sign in to Tailscale** if requested. Return from the browser to see the live connection status. An existing sign-in can skip the browser step.
4. Continue to the address only when the service reports that its public listeners are ready. Copy or share the HTTPS address. In a Lemon Chat app, use the same hostname and port **8443**. Enter every configured server key separately. The shared address does not contain those keys or your admin password.

Closing the guide leaves an explicitly started server running. **Stop public hosting** disconnects everyone and disables public access for future starts. The normal **Stop server** control closes the routes but keeps the hosting choice. Tailscale sign-in stays in private app storage. Remove the device in the Tailscale admin console to revoke that sign-in.

Private VPNs and router forwarding are separate alternatives under **Other ways to connect**. Local hosting needs no Tailscale account. The guide does not configure a VPN or router for you.

HTTPS on port 443 serves the bundled `client.html`. WSS on port 8443 forwards chat, audio and video to the server's WebSocket port. The HTTP listener uses port 8080, or 8081 if WebSocket already uses 8080. LAN access remains available.

The app includes a separate Tailscale process using `tsnet`. It does not need root, a VPN permission or the separate Tailscale Android app. Android may still stop hosting in the background. Funnel also has bandwidth limits, so it is not a replacement for a fast VPS when streaming to many people.

Funnel connections currently appear as `127.0.0.1` to this Android server. Multiple connections from that address are enabled automatically. Do not use IP bans to distinguish visitors in this mode.

## Android certificate support

The build pins Tailscale to `v1.104.1`. That version excludes its certificate API on Android, which prevents `tsnet` Funnel from obtaining a certificate. `funnel/build.py` applies the two build-constraint changes proposed in [upstream PR 21720](https://github.com/tailscale/tailscale/pull/21720) to a separate build copy. It leaves the downloaded dependency unchanged and checks the expected version and source before applying the change.

The helper also opts into Tailscale's [Android executable support](https://github.com/tailscale/tailscale/blob/v1.104.1/feature/androidbin/androidbin.go). Android blocks the normal interface lookup for app processes. The upstream integration supplies route-based interface discovery without root or extra permissions. This fallback does not provide a complete inventory of every network interface. This cgo build keeps Android's bionic DNS resolver and its existing TLS certificate handling.

Upstream assumes Android cgo builds have a Java interface getter. This standalone helper has none, so the same isolated build copy also enables the upstream fallback's registration for cgo. The builder checks the exact pinned source before changing its build constraint.

On an Android 13 NLS-MT93 phone, the rebuilt ARM64 helper starts under the app UID and returns a real Tailscale sign-in link. The test then stops it before authorization and removes its temporary state. This integration still needs account sign-in, certificate issuance and a public-access check from another network. Helper startup alone does not establish that a particular phone or account can host through Funnel.
