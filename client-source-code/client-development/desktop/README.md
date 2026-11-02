# Native Lemon Chat desktop client

A C/CMake desktop client using Meek UI layouts and styles. Windows uses WinHTTP
for TCP WebSockets; Linux and macOS use libcurl with WebSocket support (`ws://`
and `wss://`). The opt-in game overlay currently renders inside a Windows D3D11
game's swap chain. Linux and macOS game injection still need their own adapters.

The main window has a compact channel tree, a separate conversation area and
audio controls below the tree. Classic and Slate keep a simple desktop style;
Amber adds warm dark surfaces, gold accents and pointer lighting. **Connections** and
**Settings** open their own panels without blocking the rest of the window.
The local member is bold black. On Windows, the client uses the installed
Segoe UI fonts. Linux uses DejaVu Sans and macOS uses Arial when available;
a bundled open-source font is the fallback. Meek UI draws the controls on all
three platforms; they are not Win32 widgets.

Connections accepts a domain name or IP address in **Hostname**, with a separate
**Port** field. The client tries WSS first and falls back once to WS on the same
host and port if the initial transport cannot connect. It does not downgrade a
connection after login starts. Saved addresses restore their hostname and port.
Audio follows the transport that connected successfully.

Use **Add key** to create individual server-key rows. Each row has an AES-256
type selector and **Remove** button; this is the key type currently supported by
the protocol. Rows retain their encryption order, accept up to eight keys and
2048 combined bytes, and are saved with the connection. Empty rows must be filled or
removed before connecting. **Show identity** reveals the identity for copying.

Choose **Save** to store connection details without connecting, or **Connect**
to save and connect. The hostname, port, username, identity, RSA key size and
encryption keys return on the next launch. They are stored in the local user's
`desktop.json` settings file (`%LOCALAPPDATA%\LemonChat\desktop.json` on Windows,
`~/.config/lemon-chat/desktop.json` on Linux, and
`~/Library/Application Support/LemonChat/desktop.json` on macOS). This file
contains the saved identity and encryption keys; it is not encrypted.
The Windows inputs support Ctrl+A/C/X/V. The multiline description also supports
caret navigation, mouse-drag and Shift selection, and pasting at the caret or
replacing selected text while preserving newlines and Unicode.

Server info, Profile, Audio and Settings reuse the browser client's icons.
Their SVG artwork is rasterized at twice the displayed size and embedded as
base64 PNG backgrounds in each theme, with labels kept visible. Dark themes
recolour the same shapes for contrast. No extra build tool is needed for them.

The channel tree uses circular audio indicators to the left of each name. A filled green circle means
someone is speaking; a hollow ring means they are quiet. A slash means the
microphone is disabled, and a cross means audio is disabled or locally muted.
The country flag anchors the right edge. Channel groups, server groups
and unread private-message badges sit to its left, in that order. Empty flag,
channel-role and unread slots keep badges aligned across nested channels.

Group badges use the server's icons and update when assignments change. Hover a
member to see group names in the tree caption. Up to three server-group icons
fit on a row; an extra count indicates more groups. Music bots are marked as
bots, and overflowing song titles scroll below their names. Titles respect the
server's channel visibility setting. Country flags reuse the browser client's
bundled SVG assets; no geolocation service is contacted.

Unread badges count received private text, picture and file messages. Opening
the private conversation clears its badge. Clicking the badge opens that
client's private conversation, just like clicking their name. The game overlay stays compact:
current channel, member names and the same circular audio states.

Right-click a member to open group assignments, local mute/volume, move and timed
mute submenus, or moderation actions. Group icons and names arrive through the
authenticated server catalog. A checked channel group is removed by clicking
it again. Unavailable actions remain disabled; kick and ban require confirmation.
Menus capture the selected client's lifetime and channel and close when those
change. Ignore and listening volume currently last for that connected identity's
session; they are not saved across reconnections.

Right-click a channel for create, delete, edit and join actions. This does not
join it automatically. Editing opens the existing unsaved form; deleting asks
for confirmation. Full browser menu parity is still pending for avatar controls,
idle-client calls and music-bot management.

Drag the divider to resize the channel list. Private tabs have individual close
buttons and scroll arrows; closing another tab preserves the current draft.
Floating panels can be moved by their title text and fit inside smaller windows.

## Change the theme

Open **Settings → Appearance, chat and attachments**. The picker offers
**Classic**, **Slate** and **Amber**. Choose a theme, then **Save changes**. Switching keeps your chat draft,
connection and open conversations. Classic and Slate are restrained light and
dark options. Amber uses the Lemon Loader's warm brown/gold palette, rounded
panels, gradients, blurred popups and animated wheel selectors. Panel and input
borders catch a warm highlight near the pointer, like the loader. Those effects
are confined to Amber; selecting it does not change the game overlay.

To make your own theme, copy `assets/themes/classic.css` beside the executable
to `assets/themes/my-theme.css`, edit the copy, choose **Custom stylesheet**, and
enter `my-theme.css` in the filename field. Choose **Save changes** again after editing to reload it. No C rebuild or
change to `assets/main.ui` is needed. The filename may contain letters, digits,
hyphens and underscores, followed by `.css`.

Use the supplied stylesheet as an example. These are Meek stylesheets, not
arbitrary browser CSS: `font-color`, `pad` and `size` are Meek properties;
borders, gradients and keyframes are supported. Context menus read their font,
colours, width and `item-height` from the stylesheet too. Chat scaling controls
conversation dimensions independently of the channel list. The game overlay
keeps its own compact stylesheet in `src/overlay/assets`.

Theme files must be smaller than 64 KiB. Read or parse failures leave the current
theme in place; an unavailable saved theme falls back to Classic at startup.
Keep the total rule count, including comma-separated selectors and the client's
chat-scale rules, within Meek's default 256-rule limit. Chat-size changes update
existing rules without consuming more slots. A theme that cannot fit those rules
is rejected and the last valid theme is restored. See the
[Meek UI notes](../../../proposals/MEEK-UI-ISSUES.md) for current limitations.

## Try the overlay

Suppose you are playing a D3D11 game and want to see who is speaking without
leaving fullscreen. Open Lemon Chat's **Settings**, enter the game's executable name in **Game
executable**, tick **Enable game overlay**, and choose **Save settings**. For
example, enter `game.exe`, not its directory. The setting starts disabled.

When that executable runs, the client checks its architecture and loaded graphics
runtime, then loads the matching overlay DLL. It only watches the selected
executable in your Windows session. If several copies are running, it waits
instead of choosing one arbitrarily. D3D9, D3D12, OpenGL and Vulkan are detected
but their overlay adapters are not implemented in this build.

Choose **Preview channel and speakers** to send clearly marked sample members
to the native view and game overlay. One member speaks at a time, so you can see
the indicator change. Preview does not connect to a server or change permissions.
When connected, the overlay shows the current channel and its members with audio
indicators. It has a transparent background and shows up to eight rows, depending
on the game window height. Speaking members come first when space is limited;
the footer shows how many others are in the channel. The standalone window keeps
its full channel and member lists.

Untick the setting and save to stop drawing. Closing the desktop client also
hides the overlay; if updates stop unexpectedly, it hides after two seconds.
The hooked DLL stays loaded until the game exits, avoiding unloading code while
a game thread could still be executing it. It does not intercept keyboard or
mouse input. The client and game must have matching 32/64-bit architectures.

Enabling the checkbox alone does not apply or save the change. Local settings
are stored in the platform's user configuration directory:

- Windows: `%LOCALAPPDATA%/LemonChat/desktop.json`.
- Linux: `$XDG_CONFIG_HOME/lemon-chat/desktop.json`, falling back to
  `$HOME/.config/lemon-chat/desktop.json`.
- macOS: `$HOME/Library/Application Support/LemonChat/desktop.json`.

## Build

Builds use relative source paths in diagnostics and debug information, including
dependency libraries. Normal runs log warnings and errors without input tracing.
The Windows desktop executable uses the GUI subsystem and does not open a console.

Use a C11 compiler and CMake 3.22 or newer. Run `build.bat` on Windows or
`sh build.sh` on Linux/macOS from the desktop directory. No IDE is required.
The scripts detect the logical CPU count and use that many compiler jobs. They put the executable, shared libraries,
layouts and fonts together in the build directory's `package` folder.

With MinGW/w64devkit and CMake on PATH:

```powershell
.\build.bat
.\build\package\lemon-chat-desktop.exe
```

The batch file also recognizes the server script's usual w64devkit location,
`%USERPROFILE%\Downloads\w64devkit\bin`. Set `MINGWPATH` if yours is elsewhere.
Visual Studio Build Tools can be used from their developer command prompt.
Ninja is preferred for a new build; an existing directory keeps its generator.

If Windows build tools are missing, the script lists them and asks whether to
download them. It supplies portable w64devkit, CMake and Ninja; `--test` also
offers Node.js when missing. Downloads come from the upstream release sites and
must match the pinned SHA-256 checksums before extraction. They stay under
`%LOCALAPPDATA%\LemonChat\BuildTools`, without an administrator install or a
permanent PATH change. Existing compatible tools are reused.
Use `--download-tools` to accept those downloads explicitly, or `--no-download`
to require tools already installed. Redirected input never approves a download.

Before an existing Windows build starts, `build.bat` asks:
`Clean all build files before starting? [Y/n]`. Press Enter or Y to remove the
build directory, including its CMake cache and packaged output, then rebuild.
Choose N to keep an incremental build. Source files are preserved.
Use `--clean` or `--keep-build` to choose without a prompt. Redirected input keeps
existing files unless `--clean` is supplied. Automatic cleanup is limited to
`build`, `build-*` or `build_*` directories directly inside this desktop project;
use `--keep-build` for an external build directory.

Use `build.bat Debug` or `sh build.sh Debug` for an unoptimized build. The default,
`RelWithDebInfo`, enables optimization and retains debug symbols. Add `--test` to
run the small desktop checks before packaging; normal builds do not run tests.
Use `--help` for options. Node.js is optional for the protocol checks and is not
required to build the native client. Set `CMAKE_BUILD_PARALLEL_LEVEL` to change
the job count instead of using the detected CPU count, or `LEMON_DESKTOP_BUILD_DIR`
to choose another build directory. For example, set `CMAKE_BUILD_PARALLEL_LEVEL=2`
to keep compilation to two jobs while you use the computer.

Meek UI, cJSON, tiny-AES, Opus, Mbed TLS, miniaudio and libfvad sources are included in `third-party`;
no separate checkout or download is needed. Set `MEEK_UI_SOURCE_DIR` only to
deliberately use another compatible checkout.

Keep the shared libraries beside the executable. The client
resolves its assets beside itself, independent of the launch working directory.
Direct CMake commands remain supported for custom build options:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel 2
cmake --install build --prefix build/package
```

### Linux

Install a C compiler, CMake, Ninja, X11/EGL/GLES development packages and libcurl
development files. The libcurl build must support both `ws` and `wss` and be
version 7.86 or newer. Some older distro builds omit WebSocket support; the
client reports that explicitly instead of accepting a connection it cannot make.

```sh
sh build.sh
./build-linux/package/lemon-chat-desktop
```

The native window uses Meek UI's X11/EGL backend, including XWayland on Wayland
desktops. A separate native Wayland host is not wired into this client yet.
The Linux build, core/codec checks, fragmented WebSocket roundtrip, encrypted
session fixture and hidden native-window check have been run in Ubuntu under WSL. The Xvfb window check
uses software rendering; it does not establish hardware acceleration performance.

### macOS

Install Xcode command-line tools, CMake, Ninja and a libcurl build with WebSocket
support. For example, when using Homebrew curl:

```sh
CMAKE_PREFIX_PATH="$(brew --prefix curl)" sh build.sh
./build-macos/package/lemon-chat-desktop
```

This selects Meek UI's existing Cocoa/OpenGL backend. Application code stays C;
the vendored Cocoa adapter is Objective-C. This build path has not yet been
compiled or run on a Mac. App-bundle packaging and signing are also pending.

Set `LEMON_DESKTOP_GUI=OFF` to build the portable core without the native window.
Set `LEMON_DESKTOP_OVERLAY=OFF` to omit the Windows game module; it defaults off
on Linux and macOS. Video, offline messaging and the browser
client's full settings and moderation UI are still pending.

## Connect and chat

Current desktop support is:

| Feature | Status |
| --- | --- |
| Channel text | Send and receive encrypted plain text |
| Direct text | Send and receive encrypted plain text with online clients |
| Channel/direct pictures | Encrypted send/receive, bounded previews, direct-offer acceptance and Save as |
| Channel/direct files | Encrypted send/receive, progress, cancellation and Save as |
| Local chat/media preferences | Image width/height limits, chat scaling, separate receiving controls for channel/direct pictures/files |
| Audio preferences | Device selection, volume, gain, push-to-talk key, voice activation and bitrate |

Channel editing includes required message/picture powers. Those are server
permission requirements, not local chat/media preferences. Offline messaging
and rich message features still need implementation.

### Pictures and files

To send a picture to your channel, choose **Picture** below the conversation,
select the image, then choose **Send**. **File** sends other file types. From a
private conversation, the recipient first sees an offer and can accept or decline
it. Selecting another conversation while the picker is open does not change the
original destination. A disconnected or replaced recipient cannot inherit it.

Each transfer shows its progress and a Cancel button while active. To keep a
received attachment, choose **Save as**, select a folder and enter a new filename.
The client never opens received files automatically or overwrites an existing
file. A failed write can leave a partial file at the destination you selected.

Open **Settings > Chat and attachments** to limit picture width and height, scale
the conversation from 75% to 150%, or block incoming pictures/files separately
for channels and private conversations. Choose **Save changes** to apply them.
The channel tree keeps its size. Blocking a type cancels its unfinished incoming
transfers; files already received remain available to save.

The current native limits are 20 MiB per file and 4 MiB per picture, further
restricted by the server's limits. Previews support PNG, JPEG, BMP, GIF and WebP;
animated formats show their first frame. SVG and unsupported image formats can
be sent as files. Images above 8192 pixels on either axis or 16 million pixels
are rejected before decoding. Preview pixels are reduced to at most 1024 pixels
per axis; the saved file retains the original bytes.

Up to sixteen transfer records are retained. Original files and preview pixels
share a 64 MiB cache, with a separate 32 MiB texture limit. Older completed files
can be released when the cache fills, so save anything you want to keep. Reconnecting
clears the previous connection's attachment cache. These are retained-cache limits;
encryption and decoding also need bounded temporary buffers.

Open **Connections**, enter a domain name or IP address in **Hostname**, then enter
the **Port** separately. The client tries WSS first, then WS on the same port if the secure
connection cannot be established. A failed login does not trigger fallback.
To use your existing identity, enter its identity string and matching RSA
key size. Leaving the identity blank creates one. The client uses the same key
calculation and public-key protocol as the browser. It saves your identity,
address, username and key size in the local settings file; server encryption keys
are used for the current connection only.

If your server requires encryption keys, choose **Add key** for each key and enter
them in the server's order. Each row has a type selector and **Remove** button.
AES-256 is currently the supported type, matching the browser. The form accepts
up to eight keys and 2048 bytes in total, including the separators between keys.
Empty rows must be filled or removed before connecting.

Use Ctrl+A, Ctrl+C, Ctrl+X and Ctrl+V in editable fields on Windows. Choose
**Show identity** before copying the identity; masked fields still accept paste.
The channel description supports caret navigation, mouse-drag and Shift selection,
copy, cut and paste. Failed clipboard operations preserve the draft and selection.

To keep several server addresses, open **Bookmarks**, choose **Add**, enter a
name and address, then choose **Save changes**. Select an existing row to rename
it, change its address or remove it. **Use address** fills the Connections panel;
choose **Connect** there when ready. Bookmarks retain your current identity and
do not store server encryption keys. Selecting a different address clears keys
left in the connection form. Up to sixteen named addresses are saved locally.

Open **Server info** to see the server's public feature switches, slot usage and
uptime. The panel refreshes every five seconds while open and leaves chat usable.
Values the server does not publish say **Not shared**, rather than appearing to
be disabled or zero. These are server-wide switches; your groups can restrict
what you are allowed to use. Refresh requests pause while you are idle.

The background session performs identity authentication and the existing
encrypted channel-key exchange. Right-click a channel and choose **Join**, then supply its password
if requested, then send text from the composer once channel keys are ready.
The native view follows joins, departures, name changes and audio states sent by
the server. Text uses the browser's existing encrypted message wrapper, so a
message sent from either client is readable in the other.

Left-clicking another member opens their private chat and marks their row.
Clicking a channel, yourself or a music bot only marks that row. Hovering gives the full
row a visible color change, and the marked row stays highlighted afterward.
Right-click a channel, another member or yourself for the corresponding menu.
Unavailable actions remain visible but disabled. Groups without assigned icons
have no badge; audio states use shaded colored circles beside the name.

Click a member to open a private tab. You stay in
your voice channel while chatting privately. Each tab keeps its own draft, and
the X beside the tabs closes the selected private conversation. Messages from a
disconnected client stay in recent history, but sending to that old conversation
is disabled. A reused client number cannot redirect it to someone else.

If you need to correct a text you sent, hover over that message and click its pencil icon.
Change the text and choose **Save changes**; **Cancel** keeps the original.
The correction replaces the existing message and marks it as edited, while your
composer draft stays untouched. Channel edits require the original channel's
current encryption keys. Private edits require the same connected recipient and
use fresh message keys. Edits stay encrypted and refer to the original random
message ID and author public key; an older edit or a different author cannot
replace the text. Messages from an earlier connection cannot be edited.

Choose **Mute locally** in a member's panel to stop hearing that member in this
connection. Other listeners are unaffected. Choose **Unmute locally** to hear
them again. These controls do not change anyone's server permissions.

To change your name after connecting, open **Profile**, enter the new name and
choose **Save changes**. The saved connection name updates after the server
accepts it. Choose **Go idle** to leave active channel chat and audio, then
**Return** to rejoin. While idle, right-click a channel and choose **Join** to request that destination.

Right-clicking a member offers **Poke**, **Move to channel**, **Kick from channel**,
**Mute for everyone** and **Stop stream**. Unavailable actions remain disabled.
The server supplies the permission hints and checks each request again. A channel
kick asks for confirmation, moves the member to root and keeps them connected;
it stays disabled in root. A timed mute accepts 0 to remove it. These forms leave
the rest of the chat interactive. If the target moves or disconnects while a form
is open, the action is rejected rather than applied to a different member.

Open **Channels** to select a channel without joining it. Its name, description,
audio/video options, member limit and required powers are editable according to
the rights returned by the server. Choose **Save changes** to apply a draft.
The current password stays in place unless you tick **Replace password**;
an empty replacement removes it. If another editor changes the channel while
your form is open, select it again before saving so you can review the new values.

**+ Add child channel** creates a channel inside the selected permanent channel.
The server decides whether the new channel is temporary from your permissions.
**Delete channel** asks for confirmation and remains disabled for root.
Descriptions are editable up to the application's 999-byte UTF-8 limit.

The view is bounded to 1,024 clients, 256 channels, sixteen open private tabs and
128 recent text messages. Received text and composer drafts can contain up to
2,047 UTF-8 bytes. Each conversation retains its complete draft when switching
tabs. Meek owns the growing text buffers; control limits match the application's
receiving buffers. Byte limits never split a typed or pasted UTF-8 character.
These limits keep server input from growing memory without a bound. Identity
calculation runs off the UI thread; closing during that calculation waits for it
to finish before releasing its buffers.

## Voice activation and push-to-talk

When you join a channel, Lemon Chat connects its separate audio WebSocket and
plays the channel through your selected output device, initially the system
default. To speak, choose
**Mic off** to enable the microphone, then hold Q. Release Q to stop sending.
The button stays in the same style while you speak; members' audio indicators
show who is talking. The microphone starts disabled each time the app opens.

Member rows use circular audio indicators in both the standalone window and
game overlay. Speaking fills the circle green; quiet, disabled and muted states
use a center dot, slash or cross. No indicator depends on a font glyph or the
word "speaking". The preview includes the disabled and muted states too.

Open the speaker icon in the toolbar, or **Local settings → Microphone, push to
talk and audio**. **Mode** and **Push-to-talk key** are at the top. Select a mode
and a letter, number, Space or F1–F12, then choose **Save changes**. Q is the
default for new settings; an existing saved key is preserved. The key is read
by the session worker even while a game, browser or another app has focus,
including when Lemon Chat is minimized. It is not consumed, so the focused app
still receives it. Pick a key your game does not use.

Windows uses global key state and Linux uses X11 key state. Native Wayland
applications may prevent XWayland from seeing it. The macOS adapter uses Quartz
key state with ANSI physical key positions; macOS may require Input Monitoring
permission. The macOS adapter remains untested on a Mac.

To use a headset while other applications use your speakers, select it under
**Speakers / headphones** in Audio. Select its microphone under **Microphone**,
then choose **Save changes**. Selecting a different entry alone leaves the
current devices in use. **System default** lets the audio backend choose the
device. The microphone still starts disabled when the app opens.

After plugging in a device, choose **Refresh devices**. Enumeration runs on its
own worker so the chat and codec worker can continue. The list shows up to
32 inputs and 32 outputs. Choices are saved by the audio backend's device ID,
not its display name. Those IDs can change when the OS or driver changes.
If a saved device cannot be found or opened, it stays unavailable; the client
does not silently switch to another microphone or output. Reconnect it or select
a replacement and save. Hardware switching remains unverified on real devices.

If you want to speak without holding a key, select **Voice activation** under **Mode**
and choose **Save changes**. Then enable the microphone. The client listens
locally and sends when the WebRTC detector recognizes speech above your minimum
level. For example, if quiet words are missed at -45 dB, lower the minimum toward
-50 dB. If room noise keeps opening the microphone, raise it toward -35 dB.
Watch the input meter while speaking, then save the level that suits your room.
The meter follows the applied settings; unsaved slider changes are drafts.

The client retains 60 ms of audio locally so detection does not immediately lose
the start of a word. It also holds the gate for 200 ms after the detector stops
reporting speech, preserving quiet endings. Transmission still waits for the
server's microphone permission; old frames expire if that response is delayed.
This detects speech, not a particular speaker, so nearby voices or speaker output
can activate it. Select **Push to talk** and save to return to key-controlled transmission.

Choose **Deafen** to stop listening and close microphone capture too. Choose
**Listen** to resume. Microphone permission and talk-power checks still belong
to the server: audio is sent only after it acknowledges the active microphone.
Music-bot playback uses the existing bot keys supplied for your current channel.

To adjust the bandwidth used by your microphone, change **Voice bitrate** in Audio
and save. The range is 32 to 128 kbit/s, with 64 kbit/s as the default. A higher
value gives the codec more room for detail; it also uses more bandwidth while
you speak. It does not remove microphone noise or change incoming audio quality.

Audio uses Opus at 48 kHz with 20 ms microphone frames, a default 64 kbit/s VBR target,
full-band capability and encoder complexity 8. The WebRTC detector examines its
own downsampled copy; it does not lower the quality of the PCM sent to Opus.
When several speakers overlap, the mixer uses Opus soft clipping to keep summed
peaks within the output range without abruptly cutting them flat.
A worker owns device transitions, encoding and up to sixteen independent speaker decoders. Device
callbacks only move bounded PCM. Old queued audio is discarded on channel
changes, and network backpressure drops live frames instead of growing an
unbounded queue. The native client uses TCP WebSockets only. Echo cancellation
and noise suppression are not implemented.

## Libraries and process boundaries

| Component | Purpose |
| --- | --- |
| `meek_ui.dll` | Desktop window, controls, layouts and styling |
| `opus.dll` | Opus microphone encoding and per-speaker decoding |
| `lemon-audio.dll` | miniaudio capture and playback devices |
| `lemon-vad.dll` | libfvad's WebRTC speech detector, used only inside the desktop process |
| `lemon-aes.dll` | Existing tiny-AES implementation |
| `lemon-json.dll` | Existing cJSON implementation |
| `lemon-crypto.dll` | Mbed TLS primitives for the identity/session layer |
| `lemon-overlay-d3d11.dll` | Private embedded renderer and D3D11 hooks inside the selected game |

Shared libraries separate binaries and debugging symbols. They do not inherently
reduce the program's CPU or memory use. The overlay is kept small in scope:
networking, audio processing, keys and authentication stay in the desktop process.

The client publishes a versioned, bounded snapshot containing the current
channel, member names and speaking/mute flags. The game reads it through a local
memory mapping protected by a nonblocking mutex read. A busy writer never makes
the render thread wait. Invalid, disabled or stale snapshots are not drawn.

DLL loading and hook installation happen outside `DllMain`. Meek UI saves and
restores the D3D11 pipeline around rendering; the ResizeBuffers hook releases its
back-buffer view before forwarding a resize. The implementation follows the
[Windows DLL initialization guidance](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices).

## Source layout

Application C code follows the server's C conventions.
Dependency sources retain their upstream style.
Confirmed GUI integration issues, limitations and local workarounds are tracked
in [Meek UI proposals](../../../proposals/MEEK-UI-ISSUES.md).
Application integers use the server clib's `int64`, flags use `boole`, and unsigned
counters or bit operations use `uint64`. Native API output parameters retain their
required types; wire and overlay records retain their defined field widths.

- `src/clib/clib.c`, `clib.h`: an unchanged copy of the server's application clib.
  String writes use buffer-plus-capacity values. Byte operations, parsing and
  formatting use this copy, without replacing calls inside third-party libraries.
  Allocation and operating-system services still use the host interfaces.
- `src/main.c`: creates the window and connection objects, loads saved settings,
  copies received data for the window, and releases resources when it closes.
- `src/ui/ui_window.c`: connection form, channel/client selection, chat tabs and
  overlay preferences. It registers window actions before loading `main.ui` and
  keeps the controls updated from the copies supplied by `main.c`.
- `src/ui/`: channel tree, members, conversations, floating settings panels,
  explicit channel drafts and input/layout adapters for Meek UI.
- `src/session/`: worker-owned connection state, command queues and bounded
  snapshots shared with the GUI. Headers and implementation stay together.
- `src/msg/client/`, `src/msg/server/`: outgoing and incoming connection, channel
  and chat messages, following the server's message-directory organization.
- `src/crypto/`: existing identity calculation, metadata/channel encryption,
  key exchange and authenticated media framing.
- `src/audio/`: bounded PCM queues, Opus codecs, mixing, native devices and the
  audio socket lifecycle coordinated by the session worker.
- `src/attachments/`: bounded transfer ownership, worker-side file access,
  browser-compatible encryption and image validation/decoding.
- `src/platform/`: Windows, macOS and Linux adapters for threads, mutexes,
  clocks, font/asset paths and push-to-talk keys.
- `src/settings/`: local preference validation and JSON serialization, with
  platform-specific file access.
- `src/overlay/overlay_controller_win32.c`: selected-game discovery, renderer detection,
  same-architecture DLL loading and lifecycle control on a worker thread.
- `src/overlay/overlay_data.*`, `overlay_ipc_win32.c`: bounded display snapshot and IPC.
- `src/overlay/overlay_d3d11.c`: game-side render hooks and Meek UI scene.
- `src/overlay/assets/`, `src/overlay/checks/`: overlay layouts and owned test fixtures.
- `src/network/`: URL parsing, JSON envelopes, bounded message queues, cancellable
  WebSocket transports and the command-line connection probe.
- `assets/`: independently editable native window `.ui`/`.style` files.
- `third-party/`: local dependency sources and their CMake targets.

## Worker ownership

The UI sends commands to a session worker and receives copied snapshots. Separate
transport workers own the chat and audio sockets; an audio worker owns the codecs
and devices. A separate worker enumerates device choices when requested, without
opening capture or playback streams. On Windows, the overlay controller has its own worker. Short mutex
sections protect the queues and snapshot copies. Attachments have their own worker
for file access, encryption and image decoding, with an independent RSA context.
The protocol worker queues bounded chunks without waiting for network capacity.
Audio callbacks use try-locks so
they do not wait on another worker; the game-side display uses a separate
cross-process mutex. These paths do not need the server rwlock's recursive or
read-to-write upgrade behavior.

If a server stops reading, outgoing messages stay in a bounded queue and socket
writes do not block the session worker. Windows uses asynchronous WinHTTP I/O;
Linux and macOS use libcurl's nonblocking WebSocket operations. Unsent voice
packets expire after 100 ms instead of accumulating delayed speech. The audio
worker also stops transmitting if its push-to-talk decision is not refreshed
within 200 ms. Reliable chat and connection messages keep their order.

## Focused checks

When built with `BUILD_TESTING=ON` and the local `checks/startup_check.c` and `.h`
files present, `--smoke --demo` renders 112 hidden desktop frames, checks that the
settings panel leaves the channel tree accessible, and writes `desktop-smoke.bmp`,
`desktop-settings-smoke.bmp`, `desktop-connection-smoke.bmp` and
`desktop-audio-smoke.bmp`, `desktop-audio-devices-smoke.bmp`,
`desktop-actions-smoke.bmp`, `desktop-channels-smoke.bmp`
and `desktop-channel-powers-smoke.bmp` on Windows/Linux, followed by
`desktop-bookmarks-smoke.bmp` and `desktop-server-info-smoke.bmp`. The channel captures use a local
sample channel to check form clipping and scrolling; they send no requests.
The final captures are `desktop-chat-settings-smoke.bmp`,
`desktop-attachment-picker-smoke.bmp`, `desktop-chat-scale-smoke.bmp` and
`desktop-tree-smoke.bmp`. The tree check verifies name width, badge alignment
across channel depths, and unread counts before and after opening a conversation.
The scale check also verifies that the channel sidebar retains its width.
The Audio capture checks that a missing saved device stays selected and that the
device controls remain reachable by scrolling. It does not enumerate hardware.
The existing macOS backend does not provide screenshot readback. The check ignores saved
overlay settings and never attaches to a game. `lemon-overlay-check` starts its
own hidden D3D11 fixture, verifies opt-in
injection, rendering, resize, state restoration, disable/re-enable and heartbeat
expiry, then closes the fixture. It writes `overlay-smoke.bmp` for inspection.

The other checks cover bounded data, URL/protocol handling, the AES known-answer
vector and an Opus encode/decode roundtrip. Node.js is optional and runs the local
WebSocket and encrypted-session fixtures. The latter checks identity compatibility,
login, channel-key delivery in both directions, browser text wrappers, private
chat, password-protected channel joins, authenticated audio attachment, empty
binary keepalives and rejection of replayed or tampered media. It also covers
rename, poke, channel moderation, idle/return, channel
creation/editing/deletion, password preservation and rejection of an outdated draft.
Independent Node crypto vectors check the media AES/HMAC format. Generated PCM checks exercise
the audio worker, Opus, duplicate rejection, stale voice expiry, push-to-talk
expiry/recovery and mute/reset boundaries without
opening a microphone or speakers. Hardware audio and macOS remain unverified.
The WebSocket fixture includes one bounded stalled-peer cancellation check.
The activity fixture uses upstream speech PCM and silence to check detection,
pre-roll, the permission gate, microphone-off and stale-decision expiry, without
opening audio devices. Settings checks use memory-backed storage and never read
or overwrite a user's saved identity. Device-ID checks cover bounded encoding,
backend namespaces, unused native padding and missing-device resolution without
opening audio hardware. Invalid saved device fields remain unavailable instead
of becoming the system default.
These checks never run a client stress test.

Checks in `checks/` and `src/overlay/checks/` are deliberately ignored by Git.
A checkout without them still builds the application; `--test` explains that the
local checks need to be restored. Attachment checks use generated files, independent
Node crypto vectors, invalid images and a loopback transfer fixture. The build-tool
check mocks downloads and verifies checksum rejection without installing tools.
Tree protocol checks cover live group assignment, hidden country flags, song
visibility and stop events, received versus sent message counts, and bounded
vector icon decoding.

On Linux/macOS with GCC or Clang, configure a separate headless check build with
`-DLEMON_DESKTOP_GUI=OFF -DLEMON_DESKTOP_OVERLAY=OFF -DBUILD_TESTING=ON
-DLEMON_DESKTOP_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug` to instrument application
code for address and undefined-behavior checks. Run CTest with
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1`.
See [MEMORY-OWNERSHIP.md](MEMORY-OWNERSHIP.md) for buffer ownership and the limits
of these checks.
