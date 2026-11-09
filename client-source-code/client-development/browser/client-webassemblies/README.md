# Browser WebAssembly projects

The browser client uses C compiled to **WebAssembly (wasm) for speed** in audio
processing, encryption and RSA key generation. The build compiles four `.wasm`
modules and embeds them in `client.html`. JavaScript loads and calls them when
the page runs.

This directory contains the C sources, their libraries, build scripts and tests.
Each project owns its sources and dependencies. The browser build does not read
C files from the desktop client or server.

## Why Zig

**`zig cc` is a C compiler based on Clang/LLVM.** These projects use ordinary
`.c` and `.h` files. Zig also has its own programming language, but that language
is not used here.

Zig includes the compiler, linker and C-library support needed to build these
modules for WebAssembly. One portable installation can build all four projects.
This replaces the old Opus setup that required WSL, Emscripten and CMake, and
allows the Windows build to run directly on Windows. The Linux and macOS scripts
use the same compiler commands.

The build uses **Zig 0.15.2**, the version verified with the module tests.
Node.js runs the build scripts and tests, then assembles the HTML. Node's role
here is independent of the Android bridge. Neither Node nor Zig is needed to
open the finished client in a browser.

## Modules

### libopusjs

Opus encodes outgoing voice and audio streams and decodes incoming audio. It
also provides packet-loss concealment, which estimates replacement audio for
short gaps caused by missing packets.

The project includes the SpeexDSP resampler to convert microphone sample rates,
such as 44.1 kHz, to the 48 kHz rate used by the client's Opus encoder.

The libraries are Opus **1.6.1** and SpeexDSP **1.2.1**. The build includes the
mono/stereo codec and standalone resampler. Optional neural speech enhancements
and 96 kHz extensions are disabled. The resulting packets remain compatible
with the previous Opus 1.5.2 module.

### mp3-to-pcm

This module uses minimp3 to decode an MP3 selected in the browser into PCM
(uncompressed audio samples). The client can then pass those samples to the
Opus encoder for streaming.

### rsa-keygen

This module generates the RSA keypair from an identity passphrase and performs
the RSA operations used for authentication and message-key handling. Doing the
large-number calculations in WebAssembly makes them faster than the previous
JavaScript implementation.

The same passphrase must produce exactly the same keypair after a rebuild.
Changing that result would change the user's identity. Tests compare every key
component with recorded results from the old implementation.

The compiler flag `-ffp-contract=off` preserves intermediate rounding in the
seeded number generator. Its exact output determines the keypair, so an
optimization that changes that output would break existing identities.
[The RSA README](rsa-keygen/README.md) explains the calculation.

The old JavaScript RSA libraries are kept in `rsa-keygen/test/legacy-reference`
for compatibility tests. They are not included in the client as a fallback;
the wasm module is required.

### connection-crypto

This module contains the supported ciphers, hashing helpers and connection-key
derivation code. The JavaScript wrappers in `../main/scripts/app/crypto` call
its functions.

The browser's cipher implementations are in `connection-crypto/main/crypto`.
Mbed TLS and LibTomCrypt are under that project's `third-party` directory.
The desktop client and server maintain their own implementations separately.

## Building the client

From this `client-webassemblies` directory, run on Windows:

```bat
..\build.bat --unminified
```

On Linux or macOS:

```sh
sh ../build.sh --unminified
```

`--unminified` keeps the assembled JavaScript and CSS readable. Use `--minified`
for smaller HTML, or omit the option to choose interactively. Both modes compile
all four WebAssembly projects from source.

An interactive build offers to download missing Node.js and Zig tools into
`browser/.build-tools`. Terser, which minifies JavaScript, is optional and can
also be downloaded. Use `--download-tools` to allow unattended downloads or
`--no-download-tools` to disable downloads and their prompts. The
[browser build guide](../README.md#building) covers tool installation and all
build options.

The build runs in this order:

1. Compile all four projects into a temporary build directory.
2. Test the compiled modules. If compilation or a test fails, keep the previous
   modules in `../main/wasm` and leave the existing HTML unchanged.
3. Replace `../main/wasm` with the tested set, then assemble the page's source
   files and embed the modules as Base64.
4. Validate the assembled scripts, apply minification if selected, and write
   `client-source-code/client.html`. Also update `buildresult/client.html` when
   that directory exists.

Base64 stores the binary wasm data as text inside the HTML. The browser converts
it back to bytes before loading the modules, so no separate wasm download is
needed. Changes to the C sources take effect after rebuilding and opening the
updated HTML.

The modules do not require JavaScript implementations of operating-system
functions such as file access. The Opus loader is
`../main/scripts/app/audio/opus-runtime.js`; it also updates JavaScript's views of
the module's memory when that memory grows.

## Source layout

| Project | Sources and libraries | Output |
| --- | --- | --- |
| `libopusjs` | Opus and SpeexDSP under its own `third-party`; `sources.json` lists the compiled Opus C files | `libopus.wasm` |
| `mp3-to-pcm` | `main/decoder.c` and `third-party/minimp3/minimp3.h` | `mp3_decoder.wasm` |
| `rsa-keygen` | `main/rsa_keygen.c` | `rsa_keygen.wasm` |
| `connection-crypto` | `main/crypto`, plus local Mbed TLS and LibTomCrypt subsets under `third-party` | `connection_crypto.wasm` |

The audio sources are unpacked and can be edited directly. Unused demo players,
Git histories, large test-media collections and old build tools have been
removed. Each library retains its required sources, license and upstream
version information.

Each project's `build.cjs` lists its sources and compiler options.
`compiler.cjs` handles the common compiler invocation. The `build.cjs` in this
directory builds all four projects, runs their tests and replaces the published
modules. Algorithm implementations stay inside their own projects.

## Tests

Compilation checks whether the code builds. The module tests check its results
and compatibility with existing clients. Every normal build runs:

- **Opus:** new encoder with old decoder and old encoder with new decoder;
  mono/stereo audio; all six supported packet durations; bitrate changes;
  reset; two missing packets; resampling; and memory growth.
- **MP3:** sample rate, channel count, sample count and PCM values compared with
  the previous decoder; repeated files; empty and invalid input.
- **Ciphers:** known expected outputs, encryption/decryption round trips, and
  comparison with Node's OpenSSL implementation for AES, Camellia and ChaCha20.
- **RSA:** recorded 512-, 1024- and 2048-bit keypairs, large-number operations,
  and compatibility with messages from the old JavaScript implementation.
  The small key sizes are test fixtures.

Full-page browser tests run separately. The normal build's tests do not use a
physical microphone or speakers.

## Using the backup modules

`prebuilt-backup` holds four checked wasm files and the matching loader for its
older Opus binary. RSA and MP3 were refreshed to include the current
secret-clearing and input-bounds fixes. The Opus loader also handles failed
allocations safely. Normal builds never overwrite this set. Its
[provenance guide](prebuilt-backup/README.md) records the changes.

If you cannot compile the modules, run from this directory:

```sh
node ../build.cjs --unminified --prebuilt-wasm --output prebuilt-modules.html
```

This writes `prebuilt-modules.html` in the current directory. It verifies the
hashes and runs `prebuilt-backup/check.cjs` against the actual browser JavaScript
and saved modules before assembling the page. The checks cover RSA operations
and cleanup, MP3 input bounds, connection ciphers, and Opus audio and allocation
cleanup. It skips C compilation and leaves `../main/wasm` unchanged. It includes
the older Opus loader because that binary requires its matching runtime functions.

A normal build does not fall back automatically. On failure it reports the
error and keeps the existing HTML. Use `--prebuilt-wasm` when you want the backup
set; omit it to compile the current sources.

A failed client build does not stop a server build. The server scripts warn and
continue, packaging an existing `client.html` when available. Node and Zig are
not server build requirements.

## Building one module

Individual build scripts compile only their own project. Put Zig 0.15.2 on PATH
or set `ZIG` to the full path of its executable. From this directory:

```sh
node connection-crypto/build.cjs
node connection-crypto/test/ciphers.cjs
```

These commands write `connection-crypto/build/connection_crypto.wasm` and test
that file. They do not update `../main/wasm` or `client.html`. Run the normal
browser build to publish the tested modules and rebuild the page.

The commands for the other projects are:

```sh
node libopusjs/build.cjs
node libopusjs/test/codec.cjs

node mp3-to-pcm/build.cjs
node mp3-to-pcm/test/decode.cjs

node rsa-keygen/build.cjs
node rsa-keygen/test/compare-wasm.js rsa-keygen/test/vectors-2048.json
```

Individual compile commands accept an optional output filename:

```sh
node connection-crypto/build.cjs preview.wasm
```

The parent builder uses this option to write candidate modules into its
temporary directory. It runs the full test set, including the additional RSA
operation and message-compatibility tests.
