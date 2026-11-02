# Lemon Chat desktop client

A native Lemon Chat client written in C. It works as a normal desktop program
and can optionally show a small voice overlay inside supported games.

## Features

- channel and private text messages
- channel and private pictures and files
- voice chat with push-to-talk or voice activation
- channel tree with users, groups, country flags and speaking indicators
- channel and client context menus
- saved connections and bookmarks
- Classic, Slate and Amber themes
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

Use **Import .lmn** or **Export .lmn** beside the identity field to move an
identity between the browser and desktop clients. The file contains the raw
identity string. Choose the same RSA key size in both clients so they derive the
same public key. Keep the file private because anyone who has it can use that
identity.

## Voice chat

Open the audio settings to choose the microphone and speakers, change volume and
microphone gain, and select push-to-talk or voice activation. The default
push-to-talk key is **Q** and works while another application has focus.

## Themes

Themes are CSS files in `assets/themes`. Classic and Slate are simple desktop
themes. Amber is the more decorative dark theme. A theme can be changed without
rebuilding the client.

## Game overlay

The overlay is disabled by default. On Windows, enable it in Settings and enter
the executable name of the game. When that Direct3D 11 game is running, the
overlay shows the current channel and its members with speaking indicators.

The overlay contains no network keys or audio processing. Those remain in the
desktop application.

## Source layout

- `src/main.c` starts and stops the application.
- `src/ui` contains the desktop interface and channel tree.
- `src/session` owns connection state and command queues.
- `src/msg/client` creates messages sent to the server.
- `src/msg/server` handles messages received from the server.
- `src/audio` contains capture, playback, Opus and voice detection.
- `src/attachments` handles pictures and files.
- `src/overlay` contains the optional game overlay.
- `src/platform` contains Windows, Linux and macOS code.
- `assets` contains layouts, themes, icons and flags.
- `third-party` contains the libraries used by the client.

Application code follows the same C style as the Lemon Chat server. Third-party
libraries keep their original style and licenses.
