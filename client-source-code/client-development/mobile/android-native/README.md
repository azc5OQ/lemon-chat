# Lemon Native for Android

This is a separate native Android prototype for evaluating an alternative to the browser based Android client. The screens, connection owner and audio devices are written in Java. It contains no WebView, `client.html`, Node.js or JavaScript bridge.

The existing Android client remains in `client-source-code/client-development/mobile/android`. This project installs separately as **Lemon Native** and requires Android 8.0 or newer. Java sources use `com.lemonchat`. The installation ID remains `com.lemonchat.nativeclient` so updates preserve saved identities and settings.

## What you can try

Choose **Connect to a server** to open a four-step guide. Suppose a friend has given you the address `192.168.1.10` and port `1111`.

1. Enter the name your friends should see in the chat.
2. Enter `192.168.1.10` in **Server address** and `1111` in **Port**. Use the port your friend supplied if it is different.
3. If your friend supplied connection passwords, add them with their key types in the same order. Otherwise, choose **Next**. This step is for connection passwords, not your identity passphrase or the server's admin password.
4. Review your name, address, port and password count, then choose **Connect**. Back lets you correct an earlier answer. Cancel leaves the saved connection unchanged.

The app saves the connection only at the final step. Rotating the phone keeps your unfinished answers in memory. You do not enter a protocol prefix or choose TLS. The app detects the listener before sending login data. It uses TLS when available and reports certificate verification failures instead of ignoring them. Lemon's message encryption is used with either transport.

The app authenticates with its saved identity, loads channels and people, and joins the server's initial channel. Choose a channel to join it, or choose a person to open a private conversation.

Mobile 1 is the default. It and Mobile 2 share one workspace without page navigation. With chat visible, the side column shows members of your current channel. Tap the toolbar eye to hide chat and use the full channel tree. Tap it again to return to your conversation and draft. The separate people button opens a searchable list of all server members from the right. Select a person there to open a private conversation. The toolbar profile button opens your profile, and the options button includes local settings and theme selection.

Both mobile themes keep nested channels, audio indicators and real group icons. Hold a channel, or right click it, for an anchored **Join / View members** menu. Hold a member to read their groups and audio state. Native channel creation, editing, deletion, music bot management and server moderation are not implemented yet, so those browser menu actions are not shown.

Custom channel and group icons come from the server. The native renderer supports bounded PNG, JPEG, GIF and WebP images, with GIF shown as a still image. Custom SVG icons are not rendered yet. A group without a supported assigned image remains listed in member information.

Pocket, Mono Light, Mono Dark, Simple and Bluebell use **People, Chat, Channels, Me** navigation at the bottom. Pocket has quilted panels and chrome controls. Simple uses warm lemon colors. Bluebell has a toolbar button that moves its people strip between the top and the left. Mono Light and Mono Dark intentionally share a layout. The other themes have a separate light/dark choice, remembered for each theme.

People show the latest direct conversation preview when one exists. Channel cards include member circles and music bots. Tap the channel title in Chat to see everyone in that channel. Music bots appear in membership lists but cannot be opened as private conversations.

These are native adaptations of the named browser themes. Switching them rebuilds the visible views without reconnecting, changing identity or discarding the current conversation and drafts. Colors, dimensions and layout rules live in `NativeTheme.java`, with native drawables for patterned surfaces. They do not load arbitrary HTML or CSS, and adding a new theme still requires rebuilding the app. Aero and Hybrid are not included.

The microphone stays in the center above the message field. Hold it to speak in your joined channel. It remains available on People and Channels, so you do not have to open Chat to talk. The microphone never replaces Send. **Me → Local settings → Audio** controls its visibility, push to talk or hands free operation, keyboard shortcut and input device. Hands free still requires you to start recording explicitly.

Mobile 1 also has its own **Hide microphone in Mobile 1** setting. That choice does not hide the button in the other themes.

Voice uses Android `AudioRecord` and `AudioTrack`, with a native Opus codec. Audio can use the server's WebRTC DataChannel or WebSocket transport. Channel voice and music bot playback use their respective encryption keys.

## Local settings and messages

Open the toolbar options in Mobile 1 or Mobile 2, or **Me** in the other themes, then choose **Local settings** to change appearance, chat, audio, files and pictures, identity defaults or diagnostics. Changes stay in a draft until Save. Cancel keeps the current settings. Saving display preferences does not reconnect or replace your identity.

If the whole interface feels too large, open **Appearance → UI size**. Move the slider between 70% and 130%. Releasing it previews that size in the settings window. **Save** applies it to the app, while **Cancel** keeps your previous size. **Reset UI size to 100%** restores the default. Your Android display and font settings stay unchanged. The separate **Chat size** control changes only message content.

The first launch asks whether to save private chat history on this device. **Don't save** is selected initially. To keep an encrypted copy, explicitly choose 1, 7, 30 or 90 days, or **No time limit**. Older messages may be removed sooner when storage is full. You can change that choice in **Me → Offline history**, also available from Local settings → Chat. **Clear saved history** removes the app's saved copy early. Files you saved outside the app are kept. Live conversations remain usable when saving is off.

Attach a picture or file from Android's document picker. The preview belongs to the conversation where you selected it. Each attachment is limited to 4 MiB, or the server's lower limit. Received files offer explicit Open and Save actions. Text beside a selected attachment remains a separate draft because the existing attachment protocol has no caption field.

Quoted replies, server emoji reactions, read receipts and conversation search have their corresponding settings. Reactions use the actual server emoji catalog. Local file blocking controls show when the connected server does not permit that choice. Avatar changes use the server's upload limits, and member information includes per-person playback volume and mute controls.

## Server administration

After connecting, open **Me > Administration**, or **Administration** in the toolbar menu. Enter this server's administrator password to load its General settings. Choose a section, change the needed controls and use **Save changes**. **Reload** fetches the current values and asks before discarding a draft. A saved result appears only after the server confirms the settings save.

The screen includes capacity, messaging, voice and video, attachment limits, connections, encryption and browser page settings. Connection passwords remain unchanged and are shown only as a count. Banner editing, blocked country lists, logging controls and snapshot repair are not included in this screen.

**Groups and permissions** opens server groups and channel roles. A delegated group manager can use the rights granted by the server without entering the full administrator password. Phones show a group selector and searchable permission categories. Tablets can keep the group list beside the settings. Edit one action or power, check the local draft, then choose **Apply changes**. Connected people can be assigned or removed through the group's membership view. Offline identity assignments are not included yet.

The existing server protocol confirms live server-group changes but does not acknowledge `save_groups`. The app therefore says **Changes accepted. Save requested**, rather than claiming a confirmed disk save. Channel role definitions have their own save result. Temporary channel memberships can disappear when that channel is removed.

Administrator passwords are not stored in settings or Android saved view state. Changing server or identity clears this administration session. If a mutation times out without a result, reconnect before making another change so a late reply cannot acknowledge a different edit.

## Background connection

Once connected, the foreground service owns the connection and chat data independently of the Activity. Closing the screen or swiping the app from Recents leaves that service running. A new screen reads its current channels, people, messages and avatar data rather than rebuilding the connection.

When you leave the app, the service stops all media and enters server idle mode when that mode is enabled. It waits 500 ms so rotating the screen does not unnecessarily interrupt the session. Removing the app from Recents requests idle immediately and keeps the chat connection running. Returning to the app leaves automatic idle mode. A deliberate **Go away** choice stays active until you choose **Return to channel**, and an incoming call prompt does not automatically accept or leave idle mode. Background idle takes priority over continuing a music stream.

Enable **Me → Connect automatically** after saving a working server connection to reconnect after startup and reboot. **Me → Background connection** shows the actual Android battery, background and notification access. Its buttons open the same battery-exemption flow used by the older Android app. Incoming calls use notifications and join the caller's visible channel when accepted. Answering a call does not start the microphone.

The service retries interrupted connections, uses a bounded CPU wake lease while connected and registers a persisted recovery job. Explicit Disconnect cancels recovery and suppresses automatic reconnection. Android can still stop the process, and a force stop is different from removing the task from Recents. Device-specific reliability and battery consumption require real-device measurement.

## Identity and encryption

The app creates a persistent 2048-bit identity on first use. To keep an existing identity, open **Me → Import identity** and choose its `.lmn` file. Version 1 files carry their RSA key size. For a legacy raw file, the import sheet asks for the original size. Disconnect before replacing the identity.

**Me → Export identity** uses Android's file picker. Exported files contain the secret passphrase and must be kept private. The app's saved identity and optional server passwords are encrypted using Android Keystore.

The native library reuses the desktop client's protocol encryption and the deterministic RSA generator. Java reproduces the browser's historical passphrase hashing exactly. The same passphrase and size therefore reproduce the browser identity, including the tested Unicode and line break cases. See [identity files](../../../../docs/identity-files.md) for the shared import and export format.

Static server keys can be entered as ordered cipher and password rows in the connection sheet. This prototype currently supports up to eight static server keys, matching the shared desktop crypto implementation. Its six supported cipher types are AES, ChaCha20, Camellia, Twofish, Serpent and Trivium.

## Saved private conversations

If you want private conversations to survive an app restart, choose a duration in **Me → Offline history**. Saving is off until you explicitly choose it. **Don't save** keeps the current conversations in memory without loading or saving an archive. Existing prototype files remain unopened until you choose whether to keep or delete them.

Saved messages and attachment chunks use a separate, randomly generated Android Keystore AES-256 key. Each encryption gets a fresh GCM IV. Message text, display names and delivery state are encrypted, and authentication binds each record to its server, local identity and record identifier. The key stays the same between ordinary writes. **Clear saved history** revokes it, so later saved messages use a new random history key. Your identity and saved server keys are kept.

Saved private conversations remain readable while disconnected. People who are no longer online remain accessible through their saved conversation. The current interface enables sending only when you are connected and that person is online. It can receive private messages queued by the server while you were away. Changing server or identity loads that combination's separate history.

For example, choosing **7 days** keeps a message for seven days from the time this device receives it. Editing the message or updating its delivery state does not restart that period. Expired records are excluded on load and removed by background maintenance. Android can defer that maintenance when the app is not running. **No time limit** removes the age limit, but all choices still use these storage limits:

| Storage | Limit |
| --- | --- |
| Saved private records across all servers and identities | 2,000 messages and 8 MiB of encoded encrypted records |
| Saved encrypted attachment chunks | 64 MiB across all servers and identities |
| Main database file | 96 MiB |
| Visible message cache across all conversations | 500 messages, 2 MiB of metadata and 8 MiB of attachments |
| Pending saves and their callbacks | 128 messages and 10 MiB of queued data |

Older records are removed first. A storage failure leaves the received message in the bounded memory cache and shows a warning that it could not be saved. An interrupted outgoing send restores as **Unconfirmed**. App backup is disabled, and exporting an identity does not export conversation history.

**Clear saved history**, or choosing **Don't save**, closes the archive, revokes its history key, overwrites the accessible database and its journal files with random bytes, flushes them and deletes them. Temporary attachment copies opened through this app are cleared too. Current in-memory conversations stay visible, and earlier queued saves cannot put them back on disk. A failed deletion is reported and must finish before saving resumes. Files you explicitly saved elsewhere are outside this operation.

Older prototype archives used the identity vault's key. After you enable saving, their messages are migrated to the separate history key before the old accessible files are overwritten and removed. Their retention starts at migration because they did not record a local receipt time. The old identity key must remain for your identity and server settings, so deleting the new history key cannot revoke an inaccessible copy of that older archive. Flash wear leveling, backups and copies already read by another app also prevent a guarantee of physical erasure of every copy.

Channel history lasts for the connection only. There is no complete historical synchronization or history backup. The existing server also removes an offline queue entry before sending it and has no receipt acknowledgement tied to local storage. A connection failure during that transfer or process termination before saving can still lose that queued message.

## Project structure

| Directory | Responsibility |
| --- | --- |
| `app/src/main/java/.../ui` | Native screens, shared colors, icons and settings sheets |
| `app/src/main/java/.../service` | Connection lifetime, observer delivery and background notification |
| `app/src/main/java/.../protocol` | Serialized session state, bounded WebSocket transport and message handling |
| `app/src/main/java/.../model` | Immutable channel, member, message and session snapshots |
| `app/src/main/java/.../crypto` | Identity files and the narrow native crypto interface |
| `app/src/main/java/.../audio` | Microphone, playback, Opus and authenticated voice transport |
| `app/src/main/java/.../storage` | Keystore protected identity and private conversation storage |
| `app/src/main/cpp` | Small JNI adapters and build definitions for existing crypto and Opus sources |

The activity does not own the socket. The service owns one session, and that session processes connection changes on one worker. Old socket callbacks are checked against the connection they belong to. UI observers receive immutable snapshots on the main thread.

[Mumla](https://gitlab.com/quite/mumla) and its [Humla backend](https://gitlab.com/quite/humla) were references for separating audio capture, playback and the native codec boundary. No Mumla or Humla source was copied. The client uses Lemon Chat's own protocol.

## Build

Clone the complete repository, then open `client-source-code/client-development/mobile/android-native`. The project needs the shared crypto, Opus and transport sources elsewhere in that checkout. You do not need Android Studio to build an APK.

### Guided APK builder

On Windows, **double-click `build-windows.bat`**. Choose a number for **Testing APK**, **Release APK**, **Check installed build tools only**, or **Exit**. For a build, the next menu asks which architecture to include. There are no command-line arguments to remember. The window stays open when the build finishes or encounters an error.

For example, choose **1** for a testing APK, then **2** for ARMv8 / ARM64. The builder checks your tools and offers to install anything missing. After the build, it prints the full path to `build-output/arm64-v8a/debug/lemon-native-arm64-v8a-debug.apk`. This APK is signed with your local debug key and can be installed on a matching Android device. Choose **All supported architectures** if you want one APK for all three targets.

On Linux or macOS, open a terminal in this project directory and run:

```sh
bash build-linux.sh   # Linux
bash build-mac.sh     # macOS
```

The shell builder also provides numbered architecture and build-type menus. Both scripts offer these targets:

| Choice | Android ABI | Device CPU |
| --- | --- | --- |
| All supported architectures | All three below | One universal APK |
| ARMv8 / ARM64 | `arm64-v8a` | 64-bit ARM |
| ARMv7 | `armeabi-v7a` | 32-bit ARMv7 |
| Intel / AMD 64-bit | `x86_64` | Compatible devices and emulators |

**ARMv6 and 32-bit Intel are not supported by this project.** ARMv7 is not interchangeable with ARMv6. The app requires Android 8.0 or newer. Building on a Windows Intel/AMD computer can produce ARM APKs because the Android NDK includes a cross compiler.

The scripts find an existing JDK 17/21 and Android SDK, including the SDK recorded in `local.properties`. A JRE alone cannot compile the app. If needed, they offer an Eclipse Temurin JDK 21 download, Android command-line tools, SDK platform 36, Build Tools 35.0.0, NDK 27.2.12479018 and CMake 3.22.1. Downloaded JDK and command-line tool archives are checked against SHA256 checksums. SDK package installation uses Google's SDK manager, which presents the license terms for you to accept or decline.

Tools install in a user-owned directory. Windows defaults to `%LOCALAPPDATA%\LemonChat\build-tools`. Linux defaults to `$XDG_DATA_HOME/lemon-android-tools`, or `~/.local/share/lemon-android-tools`. macOS defaults to `~/Library/Application Support/Lemon Android Tools`. Set `LEMON_BUILD_HOME` before running a script if you want its tools and caches on another drive. An existing SDK selected through `local.properties` or the Android environment variables keeps its location. Allow several GB of free space for the SDK, NDK, dependencies and build outputs.

Each script sets Java and Android environment variables for its build. Windows separately offers to save those variables and tool PATH entries for future terminals. The shell builder offers an `env.sh` file you can source later. On Linux and macOS, missing Python 3 and curl can be installed through a supported package manager after confirmation. If Homebrew is missing on macOS, the script offers its official installer. These system package installers can request an administrator password. Apple Silicon may also need Rosetta for the pinned Android host tools.

**Release APK** produces an unsigned APK. It needs your own signing key before installation or distribution. The builder does not create or change a release signing identity. Final APKs are copied into `build-output/<architecture>/<debug-or-release>/`, with separate build caches for architecture selections.

Windows builds require 64-bit Intel/AMD Windows and its included PowerShell. The shell script supports Linux x86_64 and macOS Intel/Apple Silicon. Its optional `--help` explains automation and read-only prerequisite checks.

### Android Studio or Gradle

You can also open this directory in Android Studio, let it sync and run the **app** configuration. Use Android Studio's embedded Gradle JDK, or JDK 17/21. The IDE must support Android Gradle Plugin `8.11.1`.

For a deeply nested checkout on Windows, use `git -c core.longpaths=true clone <repository-url>`. Some existing vendored server test paths need Git's long-path support during checkout, before Gradle starts.

The checked-in wrapper downloads Gradle `8.13` and verifies its checksum. Gradle resolves the Java dependencies from Maven repositories and builds the bundled crypto and Opus sources through CMake. No desktop, browser or separate native dependency build is required.

Android Studio and Gradle install missing SDK platform `36`, Build Tools `35.0.0`, NDK `27.2.12479018` and CMake `3.22.1`. Allow downloads and accept the Android SDK license prompts. Automatic NDK/CMake installation requires accepted licenses, as described in the [Android build documentation](https://developer.android.com/studio/projects/install-ndk). The project contains no machine-specific SDK, compiler or library paths. Android Studio manages its local SDK location in the ignored `local.properties` file.

For a command-line build, use JDK 17/21 and set `ANDROID_HOME` to an installed Android SDK with accepted licenses. Run from this directory on Windows:

```text
gradlew.bat :app:assembleDebug
```

Or on Linux or macOS:

```sh
./gradlew :app:assembleDebug
```

The default APK includes `arm64-v8a`, `armeabi-v7a` and `x86_64`, so it also runs on the usual Android Studio emulator without changing the build settings. It is produced at `app/build/outputs/apk/debug/app-debug.apk`.

Normal Android Studio builds need no custom properties. To deliberately limit the APK to selected architectures, use `-PnativeAbis=arm64-v8a,armeabi-v7a` or `-PnativeAbis=x86_64`. `-PnativeBuildRoot=<absolute-output-directory>` is available when you want build outputs elsewhere. Use a separate output directory for each ABI selection.

## Current scope

The prototype implements login, password protected channel joins, channel and member browsing, encrypted channel and private messages and attachments, quoted replies, server emoji reactions, read receipts, avatars, queued offline private messages, saved recent private conversations, identity import and export, calls, notifications, automatic startup, channel voice, music bot playback and local music streaming. Audio supports WebSocket and native WebRTC DataChannel.

Rotation preserves up to 24 conversation drafts within a combined 2 MiB text budget, the pending send and identity import or export work. These unfinished operations are retained in memory and do not survive Android terminating the whole process.

Message editing, video, full music bot controls and composing messages to offline recipients are not implemented in this interface yet. Browser cursor effects and arbitrary CSS controls do not apply to native views. The seven included theme adaptations are described above.

## Verification

[tests/README.md](tests/README.md) contains reproducible host build and test commands. The host checks use the real JNI crypto and Opus code, compare native messages with the actual browser implementation, and exercise protocol and audio through an isolated server with disposable identities.

Real phone microphone quality, echo cancellation, Bluetooth and headset routing, phone call interruptions, sustained screen-off reception, battery behavior and manufacturer background restrictions still require device testing. Emulator interface and lifecycle checks cannot establish those physical-device results.
