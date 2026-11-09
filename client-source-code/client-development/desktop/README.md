# Lemon Chat desktop client

A native Lemon Chat client written in C. It works as a normal desktop program
and can optionally show a small voice overlay inside supported games.

## Features

- channel and private text messages
- chat fonts, colors and text sizes, with fonts that can be replaced while running
- channel and private pictures and files
- voice chat with push-to-talk or voice activation
- channel tree with users, groups, country flags and speaking indicators
- channel and client context menus
- saved connections and bookmarks
- Classic, Slate, Amber, Ivory and Midnight themes
- optional Windows game overlay

Windows, Linux and macOS are supported as desktop applications. The game overlay
currently supports Windows games using Direct3D 11.

## Build on Windows

Run:

```bat
build.bat
```

The script detects the number of processor cores and uses that many build jobs.
If required build tools are missing, it can download verified portable copies.
It also asks whether an existing build directory should be cleaned.

The finished program is placed at:

```text
build\package\lemon-chat-desktop.exe
```

Keep the DLL files, `assets` and `fonts` beside the executable.

Useful options:

```bat
build.bat Debug
build.bat Release
build.bat --clean
build.bat --keep-build
build.bat --no-download
```

## Build on Linux or macOS

Run:

```sh
sh build.sh
```

Linux needs a C compiler, CMake, libcurl with WebSocket support, pkg-config and
the X11, EGL and GLES development files. macOS needs the Xcode command-line tools,
CMake and a libcurl build with WebSocket support.

The finished package is placed in `build-linux/package` or
`build-macos/package`.

## Connect to a server

Open **Connections**, enter the server hostname or IP address and its port, add
the server encryption keys, then choose **Connect**. The client tries WSS first
and falls back to WS if WSS is unavailable.

Choose **Save** to keep the connection without connecting immediately. Saved
connections, the local identity and preferences are stored in `desktop.json` in
the operating system's application-data directory.

Open **Profile and identity** from the right-hand toolbar to manage your identity.
Choose an RSA key size from the dropdown, then click **Save identity**. Changes
apply to your next connection. An existing identity keeps its saved key size,
including sizes that are not among the usual dropdown choices.

Use **Import .lmn** or **Export .lmn** in that panel to move an
identity between the browser and desktop clients. The file contains the raw
identity string. Choose the same RSA key size in both clients so they derive the
same public key. Keep the file private because anyone who has it can use that
identity.

## Voice chat

Open the audio settings to choose the microphone and speakers, change volume and
microphone gain, and select push-to-talk or voice activation. The default
push-to-talk key is **Q** and works while another application has focus.

## Themes

Themes are CSS files in `assets/themes`. The default Classic theme uses ivory
channel and conversation panels, graphite toolbar controls, satin buttons and
thin metallic trim. Rounded edges catch the light; soft shadows separate the
panels and floating menus.
Slate is cooler and dark; Amber uses warm dark surfaces and gold accents.
Ivory pairs warm light panels with glossy black trim and metallic controls.
Midnight uses charcoal panels, indigo highlights and brighter icons, inspired by
Termix and Discord. Both are separate presets; the original three remain available.
Choose one in **Appearance and chat**, or copy a stylesheet and select
**Custom stylesheet**. **Save changes** reloads the file without rebuilding.

The layout is `assets/main.ui`. Inter handles interface text; Inter Medium and
Manrope SemiBold distinguish controls and headings. The toolbar uses rounded
icons adapted from the browser themes, with matching channel and voice symbols
in Classic. SVG sources and PNGs rendered at four times display size are in
`assets/icons`; see its README for editing them. Fonts, artwork, layout and themes
remain ordinary files beside the executable.

## Chat fonts and colors

Use the font dropdown below the message box to choose a font. **Aa** opens the
color picker; the slider sets a size from 12 to 30 pixels. The message box previews
your selection. **Reset** returns to the default font, theme color and 14 px.
These choices are saved with your other preferences and apply to channel and
private messages. Messages already sent keep their original formatting.
The browser's default combination (default font, white, 12 px) uses the desktop
theme's text color when displayed, so it remains readable in Classic's light
chat area. Its stored message data stays intact.

The package includes all 18 font families used by the browser client. To add
another font, copy a `.ttf` or `.otf` file into `fonts` beside the executable.
For example, `fonts/My Font.ttf` appears as **My Font** in the dropdown within
about two seconds. You can replace or delete the file while the client is open;
there is no rebuild or restart. Removing the selected font returns the composer
to the default. Existing messages use the theme font when their font is missing.

Use one file per face and distinct filenames, with at most 63 UTF-8 bytes before
the extension. The client loads up to 56 local faces, each at most 64 MiB. Font
collections (`.ttc`) and browser containers (`.woff`/`.woff2`) are not read by the
desktop renderer. For variable fonts, use a static face for the weight you want;
the bundled Inter, Manrope and Plus Jakarta Sans files use regular weight 400.

The font name, color and size travel inside the encrypted message. The bundled
fonts use the same names as the browser, so both clients can display them. A
custom font file stays on your computer: another desktop user needs that font
with the same filename to see it. Otherwise their client uses its normal font.

The controls remain in `assets/main.ui` and `assets/themes/*.css`. These files,
the fonts and notification sounds stay outside the executable.

## Game overlay

The overlay is disabled by default. On Windows, enable it in Settings and enter
the executable name of the game. When that Direct3D 11 game is running, the
overlay shows the current channel and its members with speaking indicators.

The overlay contains no network keys or audio processing. Those remain in the
desktop application.

## Source layout

- `main/main.c` starts and stops the application.
- `main/ui` contains the desktop interface and channel tree.
- `main/session` owns connection state and command queues.
- `main/crypto` contains cipher adapters, DH parameters, key derivation, login
  crypto and message/media encryption.
- `main/msg/client` creates messages sent to the server.
- `main/msg/server` handles messages received from the server.
- `main/audio` contains capture, playback, Opus and voice detection.
- `main/attachments` handles pictures and files.
- `main/overlay` contains the optional game overlay.
- `main/platform` contains Windows, Linux and macOS code.
- `assets` contains layouts, themes, icons and flags.
- `fonts` contains editable desktop font files and their licenses.
- `third-party` contains the libraries used by the client.

Application code follows the same C style as the Lemon Chat server. Third-party
libraries keep their original style and licenses.

## Change notification sounds while the client is running

The sound names include `you_were_kicked_from_the_server` and
`you_were_banned_from_the_server`. They play when the server closes an existing
session with the corresponding moderation code. Their Base64 strings are in
the same editable `assets/sounds.json` file as the other effects.

Suppose you want a different sound when you switch channels. Open `assets/sounds.json`
next to the executable and find `channel_switched`. Replace that entry's Base64
string with your recording and save the file. The client checks for changes about
once per second, so the next channel switch uses the replacement without a restart
or rebuild. The sound data stays in this editable text file, like the layout assets.

Keep the JSON property names and quotation marks. A value can be plain Base64 or a
complete `data:audio/mpeg;base64,...` URL, as in the supplied file. MP3, WAV and FLAC
are supported, with clips up to 30 seconds. Set a value to `""` to silence just that
effect. Keep the other entries: a missing entry, invalid Base64, malformed JSON or
an undecodable clip rejects the edit and keeps the previous sounds. The terminal
reports which sound file could not be reloaded.

For example, replacing only `channel_switched` changes channel switching; joining
users still use `user_joined_your_channel`. Four rapid joins start four independent
playbacks, so one announcement no longer swallows the next. The mixer permits up
to 128 simultaneous effects. A successful sound-file reload stops effects already
playing and installs the new clips together, rather than mixing old and new assets.

In **Audio settings**, **Play notification sounds** turns the effects on or off.
They use the selected speakers and listening volume. **Deafen** silences them too.
They remain available when voice chat is disabled by the server, so connection
loss and disconnect announcements can still play. Neither decoding nor reading the
sound file happens in the speaker callback.
