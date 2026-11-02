# Building Lemon Chat

Run the build script in the folder for the part you want. The script builds that part and places the finished files in its output directory. You do not need to load the CMake project into an IDE.

## Build commands

Paths start at the Lemon Chat project directory.

| What you want to build | Folder | Command |
| --- | --- | --- |
| Server on Windows | `server-source-code` | `.\windows_build_script.bat` |
| Server on Linux | `server-source-code` | `bash linux_build_script.sh` |
| Server on macOS | `server-source-code` | `bash macos_build_script.sh` |
| Browser client | `client-source-code/client-development/browser` | Windows: `.\build.bat`; Linux/macOS: `sh build.sh` |
| Native desktop client | `client-source-code/client-development/desktop` | Windows: `.\build.bat`; Linux/macOS: `sh build.sh` |
| Android client APK | `client-source-code/client-development/mobile/android` | Windows: `.\gradlew.bat :app:assembleDebug`; Linux/macOS: `sh ./gradlew :app:assembleDebug` |
| Android server APK | `server-source-code/android-server-app` | Windows: `.\gradlew.bat :app:assembleDebug`; Linux/macOS: `sh ./gradlew :app:assembleDebug` |

If you want to host a chat, build the server. The server build also creates the browser client's `client.html`.

## Server

The server scripts build the required libraries, the server and `client.html`. They collect the files needed to run the server in `buildresult` at the project root.

Stop the server before rebuilding. Save any settings or other files you need from `buildresult`: the Linux and macOS scripts recreate that directory, and the Windows script can clear it when you choose a clean build.

### Windows

Install Node.js, open a terminal in `server-source-code`, then run:

```bat
windows_build_script.bat
```

The script can download w64devkit, CMake, Ninja and Python when they are missing. It reuses installed tools when possible. The result is `buildresult/chat-server.exe` with its supporting files. Keep those files together.

The optional stunnel build needs Git for Windows because it supplies Bash and Perl.

### Linux

Install GCC, G++, CMake, Ninja, Make, pkg-config, Python 3, Node.js, Perl, curl, tar, autoconf, automake and libtool. From `server-source-code`, run `bash linux_build_script.sh`.

The result is `buildresult/chat-server.bin` with its supporting files and `start_server.sh`. The script also builds stunnel. Use `--wsl` when building a Linux executable under Windows Subsystem for Linux.

### macOS

Install the Xcode command-line tools, CMake, Ninja, pkg-config, Python 3 and Node.js. The stunnel build also uses Make and Perl. From `server-source-code`, run `bash macos_build_script.sh`.

The result is `buildresult/chat-server.bin` with its supporting files and `start_server.sh`. The script builds for the compiler's selected architecture.

For hosting and public access after building, read the [self-hosting guide](../selfhosting-tools/README.md).

## Browser client

Install Node.js, open `client-source-code/client-development/browser`, then run `build.bat` on Windows or `sh build.sh` on Linux and macOS. The result is `client-source-code/client.html`.

The script offers readable and minified output. Minifying requires Terser; the readable build does not. The [browser build guide](../client-source-code/client-development/browser/README.md#building) explains the choices and direct `node build.cjs` commands.

## Android client

The Android client requires JDK 17 or newer, Android SDK 36, NDK `27.2.12479018`, CMake `3.22.1`, Node.js and Python 3. Android Studio can install the Android tools.

After a fresh checkout, run `python nodejs-client/fetch-nodejs-mobile.py` from `client-source-code/client-development/mobile`. Then run the Gradle command from the table above. The APK is written to `app/build/outputs/apk/debug/app-debug.apk` inside the Android client project.

The APK supports Android 8.0 or newer and includes ARM64 and 32-bit ARM. See the [mobile client guide](../client-source-code/client-development/mobile/README.md) for setup details.

## Android server

The Android server requires the same Android tools, plus Python 3 and Go `1.27.1` or newer. Run the Gradle command from the table above. The APK is written to `app/build/outputs/apk/debug/app-debug.apk` inside the server app project.

The default build includes ARM64. Supply `-PserverAbis=arm64-v8a,armeabi-v7a` to include 32-bit ARM too, or choose from `arm64-v8a`, `armeabi-v7a` and `x86_64`. See the [Android server guide](../server-source-code/android-server-app/README.md) for starting it and configuring public access.

## Native desktop client

The native C client is intended for low-overhead use and for its game overlay. It is still being developed and does not yet contain every browser-client feature.

Use a C11 compiler and CMake 3.22 or newer. Run the desktop build script from the table above. Its package is written below the desktop directory:

| System | Output |
| --- | --- |
| Windows | `build/package/lemon-chat-desktop.exe` |
| Linux | `build-linux/package/lemon-chat-desktop` |
| macOS | `build-macos/package/lemon-chat-desktop` |

Keep the entire `package` directory together because it contains the libraries, interface files and fonts used by the executable. The Windows package also contains the D3D11 game overlay.

The [desktop guide](../client-source-code/client-development/desktop/README.md) lists the platform dependencies, build options and current feature coverage.

## Processor types

Native programs must be built for the operating system and processor that will run them. The browser client uses one `client.html` on every processor. The Android client includes both ARM builds by default, while the Android server uses ARM64 unless other ABIs are selected.

The Windows server and desktop build for x86-64 with the supplied w64devkit setup. Linux and macOS use the selected compiler's target. A script accepting a target does not by itself mean that target has been tested.

## Source formatting

Formatting is separate from building. `server-source-code/main/windows_clangformat_script.bat` runs `clang-format` over the server C and C++ sources. It changes files in place and is not required for a build.
