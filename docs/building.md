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

If you want to host a chat, build the server. When Node.js is installed, the server build also rebuilds the browser client's `client.html`.

## Server

The server scripts build the required libraries and the server, and attempt to rebuild `client.html` when Node.js is available. They collect the files needed to run the server in `buildresult` at the project root.

Stop the server before rebuilding. The Windows cleanup keeps settings, certificates and uploaded files in `buildresult`. The Linux and macOS scripts recreate that directory, so save any files you need before running them.

If you build the server on a machine without Node.js, the scripts warn and skip rebuilding the browser client. They copy the existing `client-source-code/client.html` into `buildresult` if available; that page may not include your latest client source changes. If the file is missing, they warn that the bundled HTTP server has no page to serve, but still build the server. If Node.js is available but the client build fails, the scripts also warn and continue building the server, using the existing page if available. Node.js and a successful browser client rebuild are not server build requirements.

### Opus and CPU optimizations

The server uses Opus 1.6.1 to encode music bots' audio. It has its own copy in
`server-source-code/third-party/libopus-1.6.1`. Voice packets from clients are
forwarded without re-encoding them.

Use a Release build for normal use. Debug builds leave compiler optimization
off, which can produce Opus's "compiling without optimization" warning and make
encoding much slower. Release builds use `-O3` on Windows, Linux and macOS.

On x86, the build includes the CPU optimizations supported by the compiler.
Opus selects a supported instruction set when it runs: an AVX2-capable machine
can use AVX2, while an older processor can use an earlier instruction set.
The build scripts reset cached x86 overrides to Opus's automatic defaults.

The CMake feature list can be confusing. `OPUS_X86_MAY_HAVE_AVX2` enabled means
AVX2 code is included and selected after checking the CPU.
`OPUS_X86_PRESUME_AVX2` disabled means that check is still required; it does not
disable AVX2. Likewise, `OPUS_DISABLE_INTRINSICS` disabled means CPU
optimizations are allowed. These lines are configuration information, not
performance warnings.

### Windows

Open a terminal in `server-source-code`, then run:

```bat
windows_build_script.bat
```

The script can download w64devkit, CMake, Ninja and Python when they are missing. It reuses installed tools when possible. The result is `buildresult/chat-server.exe` with its supporting files. Keep those files together.

The optional stunnel build needs Git for Windows because it supplies Bash and Perl.

If you want to rebuild the Windows server and its libraries from scratch, answer `y` to **Clean previous Windows build files?** The cleanup runs after you confirm the build. Answer `n` to reuse previous build files and compile only what changed.

Cleanup removes compiled files and generated CMake/Ninja files, including those inside the libraries' subdirectories. It keeps source files, downloaded tools, IDE settings and the separate Linux, macOS and Android output directories. The Windows OpenSSL build and stunnel files are also cleaned when you choose to rebuild stunnel. If you skip stunnel, its existing files stay available.

To see the files that would be removed, run this from the project directory:

```bat
python tools\clean.py --scope server-windows --dry-run
```

Add `--include-stunnel` to preview the optional stunnel cleanup too. The preview does not remove files. During the build, cleanup prints a file count and stops on a removal error, such as an executable still used by a running server.

### Linux

Install GCC, G++, CMake, Ninja, Make, pkg-config, Python 3, Perl, curl, tar, autoconf, automake and libtool. From `server-source-code`, run `bash linux_build_script.sh`.

The result is `buildresult/chat-server.bin` with its supporting files and `start_server.sh`. The script also builds stunnel. Use `--wsl` when building a Linux executable under Windows Subsystem for Linux.

### macOS

Install the Xcode command-line tools, CMake, Ninja, pkg-config and Python 3. The stunnel build also uses Make and Perl. From `server-source-code`, run `bash macos_build_script.sh`.

The result is `buildresult/chat-server.bin` with its supporting files and `start_server.sh`. The script builds for the compiler's selected architecture.

For hosting and public access after building, read the [self-hosting guide](../selfhosting-tools/README.md).

## Browser client

Open `client-source-code/client-development/browser`, then run `build.bat` on Windows or `sh build.sh` on Linux and macOS. The result is `client-source-code/client.html`.

The wrappers offer to download missing Node.js and Zig tools locally. Node.js runs the build script; Zig 0.15.2 compiles all four WebAssembly projects from their own browser sources. The builder tests them, then assembles the page, embeds their bytes and validates JavaScript. This is separate from the Android runtime: opening the finished `client.html` in a browser does not require Node.js.

The script offers readable and minified output. Minifying requires optional Terser, which the script can also download; the readable build does not. Use `--download-tools` for unattended downloads or `--no-download-tools` to forbid them. `--prebuilt-wasm` explicitly uses the checked prebuilt binaries and matching loader after verifying their hashes and running compatibility and security regressions. Server builds disable client-tool downloads and continue if any client build step fails, including a missing Zig compiler. The [browser build guide](../client-source-code/client-development/browser/README.md#building) explains the choices and direct `node build.cjs` commands.

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

To format the server sources on Windows, run `server-source-code/main/windows_clangformat_script.bat`. For the desktop client, run `client-source-code/client-development/desktop/main/windows_clangformat_script.bat`.

Both scripts use the bundled `tools/clang-format.exe` and the `.clang-format` file in their own `main` directory. They apply Allman braces and explicit conditional and loop bodies. Run either script from any working directory; it formats only that application's source tree and skips build directories, bundled dependencies and directory links.

Formatting changes source files in place. To check formatting without changing files, add `-Check` after the script name. The scripts stop and return an error if formatting or checking fails. Formatting is separate from building and is not required for a build.

## Rebuilding browser WebAssembly

Suppose you change a browser cipher. Edit its own source under
`client-source-code/client-development/browser/client-webassemblies/connection-crypto/main/crypto`,
then run the normal browser build. It now compiles and tests **all four** modules
before embedding them; changing a C source no longer requires a separate manual
binary-copy step. These browser projects do not compile desktop or server source.

For a source build with tools already installed, run from the repository root:

```powershell
$env:ZIG = 'C:\tools\zig-0.15.2\zig.exe'
node client-source-code/client-development/browser/build.cjs --unminified --no-download-tools
```

The generated modules have no external host imports. The client embeds them as
Base64 and loads them from the page itself. Each project owns its required
third-party libraries; the desktop and server retain their own separate sources.
The [browser build guide](../client-source-code/client-development/browser/README.md#building)
explains staging, tests, tool downloads, backups, rollback and failure handling.
The [WebAssembly guide](../client-source-code/client-development/browser/client-webassemblies/README.md)
lists the individual projects and isolated compile/test commands.

If you cannot compile the modules, `--prebuilt-wasm` deliberately embeds the
checked set from `client-webassemblies/prebuilt-backup`, with its matching older
Opus loader. RSA and MP3 in that set include the current secret-clearing and
input-bounds fixes. The build verifies hashes and compatibility before writing
the page. That directory is never overwritten by a build, and a normal build
does not fall back silently. A client build failure always remains optional for
a server build: the server still builds and packages the existing page when
available.

The Android Node bundle is generated from browser sources, so regenerate that
bundle and APK when shipping a protocol change. Node.js remains a browser build
tool; users opening the HTML run its JavaScript and WebAssembly in their browser.
