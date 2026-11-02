# What you can build

To build Lemon Chat, run the build script in the folder for the part you want. For the browser and desktop clients, this means `build.bat` on Windows or `sh build.sh` on Linux and macOS. The server has a separate script for each operating system, listed below.

The scripts run the build steps for you. The Windows server script also offers to download missing build tools, including the compiler, CMake and Ninja, and installs Python if needed. Follow its prompts. Node.js must already be installed; the script checks for it before starting.

## Which script to run

Open a terminal in the folder listed below and enter the command for your system. Folder paths start at the Lemon Chat project directory.

| What you want to build | Folder | Command |
| --- | --- | --- |
| Server on Windows | `server-source-code` | `.\windows_build_script.bat` |
| Server on Linux | `server-source-code` | `bash linux_build_script.sh` |
| Server on macOS | `server-source-code` | `bash macos_build_script.sh` |
| Browser client | `client-source-code/client-development/browser` | Windows: `.\build.bat`; Linux/macOS: `sh build.sh` |
| Native desktop client | `client-source-code/client-development/desktop` | Windows: `.\build.bat`; Linux/macOS: `sh build.sh` |
| Android client APK | `client-source-code/client-development/mobile/android` | Windows: `.\gradlew.bat :app:assembleDebug`; Linux/macOS: `sh ./gradlew :app:assembleDebug` |
| Android server APK | `server-source-code/android-server-app` | Windows: `.\gradlew.bat :app:assembleDebug`; Linux/macOS: `sh ./gradlew :app:assembleDebug` |

If you want to host a chat, build the server. That also builds `client.html`, which people can open in a browser to join it. You can use that file as a desktop chat app too. The native C client is a separate option mainly for use alongside games where performance matters.

The sections below explain any tools you need to install first and where each build puts its finished files. Automatic tool downloads are part of the Windows server script; the other scripts use the tools installed on your computer.

## Server for Windows, Linux and macOS

The three server scripts build the required libraries, the server and the browser's `client.html`. They collect the files needed to run the server in `buildresult` at the project root.

Before rebuilding, stop the server and save any settings or other files you need from `buildresult`. The Linux and macOS scripts delete and recreate that directory on every run. The Windows script clears it if you choose to clean the previous build.

### Windows

Install Node.js, then open a terminal in `server-source-code` and run:

```bat
windows_build_script.bat
```

The script handles setup of w64devkit, CMake, Ninja and Python. It offers to download and unpack missing compiler and build tools, and installs Python if it cannot find version 3.8 or newer. Existing tools are reused. Node.js must already be installed before running the script.

Follow the download prompts, then check the displayed tool paths before continuing. The default compiler location is `%USERPROFILE%\Downloads\w64devkit\bin`. If you already keep w64devkit elsewhere, change `MINGWPATH` near the top of the script. The server's library source code is included in the repository and is built by the script.

The result is `buildresult/chat-server.exe`, together with its supporting files. Keep those files beside the executable.

The script also offers to build stunnel for secure WebSocket connections. This optional build needs Git for Windows, which supplies the Bash and Perl tools it uses.

### Linux

Install GCC and G++, CMake, Ninja, Make, pkg-config, Python 3, Node.js, Perl, curl, tar, autoconf, automake and libtool. From `server-source-code`, run:

```sh
bash linux_build_script.sh
```

The result is `buildresult/chat-server.bin`, its supporting files and `start_server.sh`. Open a terminal in `buildresult` and run `bash start_server.sh` to start the server. This build also includes stunnel.

The script has a separate `--wsl` option for building under Windows Subsystem for Linux. It still produces a Linux server executable.

### macOS

Install the Xcode command-line tools, CMake, Ninja, pkg-config, Python 3 and Node.js. The stunnel build also uses Make and Perl. From `server-source-code`, run:

```sh
bash macos_build_script.sh
```

The result is `buildresult/chat-server.bin`, its supporting files and `start_server.sh`. Run `bash start_server.sh` from `buildresult`. The script builds stunnel as well. It builds for the compiler's selected architecture; it does not create one combined Intel and Apple Silicon executable.

The [Windows](../server-source-code/windows_build_script.bat), [Linux](../server-source-code/linux_build_script.sh) and [macOS](../server-source-code/macos_build_script.sh) scripts contain the build settings. For hosting and public access after building, see the [self-hosting guide](../selfhosting-tools/README.md).

## Browser client

The browser client is built into one HTML file. You can build it separately when changing the interface or client code, without rebuilding the server.

Install Node.js and open a terminal in `client-source-code/client-development/browser`. Run `build.bat` on Windows or `sh build.sh` on Linux or macOS. You can also run the builder directly on any of these systems:

```sh
node build.cjs --unminified
```

The result is `client-source-code/client.html`. The builder also writes `buildresult/client.html` when the `buildresult` directory exists. Open the file in a browser or serve it from a website.

The wrappers let you choose readable or minified output. Minified output removes unnecessary spaces and comments to make the file smaller; it requires Terser. See the [browser build guide](../client-source-code/client-development/browser/README.md#building) for its installation and build options.

## Android client APK

The Android client is an app for joining a server. It displays the interface in Android's WebView and uses an included Node.js runtime to keep its connection running in the background. Building it produces an APK, the file installed on an Android device.

Use JDK 17 or newer, Android SDK 36, NDK `27.2.12479018`, CMake `3.22.1`, Node.js and Python 3. Android Studio can install the SDK, NDK and CMake. Set the Android SDK location in the project's `local.properties` if it is not already configured.

After a fresh checkout, download the required Android Node.js runtime. From `client-source-code/client-development/mobile`, run:

```sh
python nodejs-client/fetch-nodejs-mobile.py
```

Then open a terminal in `client-source-code/client-development/mobile/android`. On Windows, run:

```powershell
.\gradlew.bat :app:assembleDebug
```

On Linux or macOS, run:

```sh
sh ./gradlew :app:assembleDebug
```

The result is `app/build/outputs/apk/debug/app-debug.apk` inside that Android project. Gradle builds the browser page and the background JavaScript code, then includes both in the APK. There is no need to build those parts separately.

The APK includes ARM64 and 32-bit ARM and requires Android 8.0 or newer. The current client configuration and runtime downloader do not include x86 or x86-64. See the [mobile client guide](../client-source-code/client-development/mobile/README.md) and its [build configuration](../client-source-code/client-development/mobile/android/app/build.gradle).

## Android server

The Android server app hosts Lemon Chat on a phone. It contains the native server, the browser client and the Tailscale Funnel program used for optional public access. It is a separate APK from the Android chat client.

Use JDK 17 or newer, Android SDK 36, NDK `27.2.12479018`, CMake `3.22.1`, Node.js, Python 3 and Go `1.27.1` or newer. The build calls `node`, `python` and `go`, so those commands must be available in the terminal. The first build downloads the required Go dependencies.

Open a terminal in `server-source-code/android-server-app`. On Windows, run:

```powershell
.\gradlew.bat :app:assembleDebug
```

On Linux or macOS, run:

```sh
sh ./gradlew :app:assembleDebug
```

The result is `app/build/outputs/apk/debug/app-debug.apk` inside the server app project. It requires Android 8.0 or newer. The default build contains ARM64 only.

To include both 64-bit and 32-bit ARM in the same server APK, add the processor choices to the command:

```powershell
.\gradlew.bat :app:assembleDebug "-PserverAbis=arm64-v8a,armeabi-v7a"
```

The same option works with `sh ./gradlew`. The accepted values are `arm64-v8a`, `armeabi-v7a` and `x86_64`, separated by commas. For example, `-PserverAbis=x86_64` builds only the x86-64 version. Each selected version includes both the server and the Funnel program.

You can also build only the native Android server, without the app interface or Funnel. From `server-source-code/android-server-app/native`, with CMake and Ninja installed, run:

```sh
NDK=/path/to/android-ndk ABI=arm64-v8a sh build-native.sh
```

Replace the NDK path with your installation. The result is `out/arm64-v8a/package/libchat-server.so`. Despite the `.so` name, it is an executable; the app uses that name to package it as an Android native file. It is not an APK. The script accepts the same three processor choices as the server app.

See the [Android server guide](../server-source-code/android-server-app/README.md) for starting the server and setting up public access.

## Native desktop client

The browser client's `client.html` can also be used as a desktop chat app. Simply open the file from your computer in a browser. There is no need to build a native client for this.

The main reason to use the native C client is to run it alongside games, where performance really matters. It uses Meek UI for its own window and includes a Windows game overlay. It is still being developed and does not yet include all browser-client features. Its [feature list](../client-source-code/client-development/desktop/README.md#connect-and-chat) describes the current state.

Use a C11 compiler and CMake 3.22 or newer. Open a terminal in `client-source-code/client-development/desktop`. No IDE is required.

| System | Command | Output inside the desktop directory |
| --- | --- | --- |
| Windows | `build.bat` | `build/package/lemon-chat-desktop.exe` |
| Linux | `sh build.sh` | `build-linux/package/lemon-chat-desktop` |
| macOS | `sh build.sh` | `build-macos/package/lemon-chat-desktop` |

On Windows, use MinGW/w64devkit or a Visual Studio developer command prompt. The script also checks the server build's usual w64devkit directory. On Linux and macOS, use Ninja or Make.

Linux needs pkg-config, X11, EGL and GLES development files, plus libcurl 7.86 or newer built with both `ws` and `wss` support. The window uses X11, including through XWayland on a Wayland desktop.

macOS needs the Xcode command-line tools and libcurl with WebSocket support. The Cocoa/OpenGL build is present, but has not yet been compiled or run on a Mac. Mac app-bundle packaging and signing are not implemented.

Keep the entire `package` directory together. It contains shared libraries, interface files and fonts needed by the executable. The Windows build also includes the D3D11 game overlay; the client and game must have matching architectures to use it.

The default build is optimized and retains debug information. Add `Debug` for a debug build or `--test` to run the desktop checks. See the [desktop build guide](../client-source-code/client-development/desktop/README.md#build) for custom paths and other options.

## Platforms and processor types

A native program must be built for the operating system and processor that will run it. **x86-64**, also called **x64** or **AMD64**, means 64-bit Intel and AMD processors. **ARM64**, also called **AArch64**, means 64-bit ARM processors. Android calls these ARM builds `arm64-v8a`; its 32-bit ARM build is called `armeabi-v7a`.

| Build | Operating system | Processor choice in the current build files |
| --- | --- | --- |
| Server | Windows | x86-64 with the supplied w64devkit setup. |
| Server | Linux | Uses the installed GCC compiler's target. The script does not set a fixed processor type. |
| Server | macOS | Uses the installed Clang compiler's target, normally x86-64 on Intel Macs or ARM64 on Apple Silicon. |
| Browser client | Runs in a browser | One `client.html`; no separate Intel or ARM build. |
| Android client APK | Android 8.0 or newer | ARM64 and 32-bit ARM, both included by default. |
| Android server APK | Android 8.0 or newer | ARM64 by default. 32-bit ARM and x86-64 can also be selected. |
| Native desktop client | Windows | x86-64 with the usual w64devkit setup. |
| Native desktop client | Linux | Uses the selected C compiler's target. Requires the Linux graphics and network libraries described above. |
| Native desktop client | macOS | Uses the selected C compiler's target. This build has not yet been compiled or run on a Mac. |

The Windows server and desktop executables in the current build output are x86-64. The Android client APK contains both ARM builds; the Android server APK contains ARM64. The other Android server choices are accepted by its build scripts, but are not in that default APK.

The Linux and macOS scripts build for one compiler target at a time. They do not produce every processor version in one run. Their compiler choice alone does not establish tested support for every CPU. In particular, a complete ARM Linux or 32-bit Windows build still needs verification before it can be listed here as confirmed support. The [desktop guide](../client-source-code/client-development/desktop/README.md#linux) records the Linux checks run under Ubuntu in WSL.

## Optional source formatting

Formatting is separate from building. The [Windows formatting script](../server-source-code/main/windows_clangformat_script.bat) uses `clang-format` to change C and C++ source formatting in place. Run it from `server-source-code/main` with `clang-format` installed and available in the terminal. It processes that directory and its subdirectories. This tool is not required to build the server.
