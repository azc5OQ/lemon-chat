# Lemon Chat desktop app (Electron)

The files in this folder pack `client.html` as a standalone desktop program for Windows, Linux and macOS. The
program consists of Electron, which combines Chromium and Node.js, together with `client.html` and a start file
(`app/main.js`). It opens the page in its own window, without the tabs and the address bar of a browser. The page is
not changed, so the program has the same functions, themes and local settings as `client.html` in a browser.

## Differences from a browser

- Push-to-talk works while another program is in front. A browser page receives key presses only while its window
  has the keyboard focus. The app starts a helper program that reports the presses and releases of the talk key from
  anywhere on the desktop and passes them to the page. The talk key and the continuous transmission setting of the
  local settings apply as in a browser, and a changed talk key is used at once.
- The address and port of the last server with a successful login are filled in at the next start. Bookmarks are
  kept as in a browser.
- Microphone, camera, notifications and the clipboard are allowed without a browser prompt. Screen sharing shows a
  list of screens and windows. On Windows the shared screen can include the sound of the computer.
- The timers of the page are not slowed down while the window is in the background or minimized.

## Files

| File | System |
|---|---|
| `client-electron-amd64.exe` | Windows, x64 processors |
| `client-electron-arm64.exe` | Windows, 64-bit ARM processors |
| `client-electron-i386.exe` | Windows, 32-bit x86 processors |
| `client-electron-amd64.AppImage` | Linux, x86-64 processors |
| `client-electron-arm64.AppImage` | Linux, 64-bit ARM processors (aarch64) |
| `client-electron-armhf.AppImage` | Linux, 32-bit ARM processors (armv7 with hardware floating point) |
| `client-electron-amd64.dmg` | macOS, Intel processors |
| `client-electron-arm64.dmg` | macOS, Apple processors (M1 and newer) |

The .exe files are portable programs. At the first start of a new version the program unpacks itself into
`%LOCALAPPDATA%\LemonChat` and removes older unpacked versions. The settings are stored in `%APPDATA%\Lemon Chat` on
Windows, in `~/.config/Lemon Chat` on Linux and in `~/Library/Application Support/Lemon Chat` on macOS.

The 32-bit files use Electron 43.8.11, because Electron 44 and newer are not published for 32-bit Windows and 32-bit
ARM Linux. Electron publishes fixes for its three newest major versions, so Electron 43 gets no fixes after
Electron 46 is released. The other files use Electron 45.3.4. Windows 7, 8 and 8.1 are not supported, because
Electron 23 and newer do not run on them.

## Push-to-talk on each system

- Windows uses a low-level keyboard hook, which needs no setup.
- Linux tries three methods. evdev reads the keyboard devices in `/dev/input` and works in X11 and Wayland sessions
  for members of the `input` group (`sudo usermod -aG input $USER`, then log in again). The GlobalShortcuts portal
  works in Wayland sessions of KDE Plasma and of GNOME 48 and newer, and the desktop asks once to confirm the key.
  XInput 2 works in X11 sessions.
- macOS uses an event tap, which needs the permission in System Settings > Privacy & Security > Input Monitoring.
  Without that permission the helper reads the state of the key every 15 milliseconds.

## Building

| System | Command |
|---|---|
| Windows | `build-windows.bat` |
| Linux | `sh build-linux.sh` |
| macOS | `sh build-mac.sh` |

Each script runs the client build in `../browser` with `--electron`. It builds `client.html` first and then the
files of its system into `buildresult` at the top of the repository. The scripts take the options of the client
build, for example `--minified`. `../browser/build-windows.bat`, `../browser/build-linux.sh` and `../browser/build-mac.sh` ask
whether to build the Electron app as well.

The client build runs with Node.js 20 or newer. When Node.js is not installed, the scripts offer to download a
portable Node.js 24.14.0 into `../browser/.build-tools`, and with `--download-tools` they download it without asking.
The Electron step needs Python 3.8 or newer. Linux also needs `mksquashfs` from the squashfs-tools package, and macOS
needs the Xcode command line tools. Zig, which the client build downloads, compiles the helper programs for Windows and
Linux. No npm packages are installed. The Electron archives are downloaded once into `.downloads` and checked against
the SHA-256 values in `electron-downloads.txt`.

`package_electron.py` can also pack an existing `client.html` for one system, for example
`python package_electron.py --target win32-arm64`.

## Contents of this folder

| Path | Content |
|---|---|
| `app/` | `main.js`, the start file of the app, and `package.json` |
| `ptt-helper/` | the push-to-talk helper programs for Windows, Linux and macOS, in C |
| `windows-launcher/` | the start program of the portable .exe files, in C |
| `definitions.h` | the basic types of the C programs, the same as in the desktop client |
| `linux/` | the start script, the desktop entry and the AppImage runtimes |
| `package_electron.py` | the packaging script |
| `electron-downloads.txt` | the addresses and SHA-256 values of the Electron archives |
