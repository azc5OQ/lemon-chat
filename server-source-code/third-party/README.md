# Third-party libraries

The `third-party` directory contains the libraries used by the LemonChat
server. They can be grouped along two axes: **how they are compiled** and
**how they are used**.

**By how they are compiled**

- Libraries that contain their own `CMakeLists.txt`.
- Libraries that do not contain their own `CMakeLists.txt`.

**By how they are used**

- Direct dependencies of the main project.
- Dependencies of other libraries.
- Dependencies of both the main project and other libraries.

## Libraries that contain their own `CMakeLists.txt`

These are independent projects, separate from the main LemonChat project. Each
is built into either a static or a dynamic library.

- **Static** libraries have the extension `.a` or `.lib`.
- **Dynamic** libraries have the extension `.so` or `.dll`.

On Windows, dynamic libraries use `.dll` files; on Linux, they use `.so` files.
For static libraries, `.a` is common on both platforms depending on the
toolchain (MinGW typically produces `.a`, while MSVC produces `.lib`).

| Library | Linkage |
| --- | --- |
| libopus-1.5.2 | static |
| mbedtls-3.6.6 | static |
| libviolet | static |
| theldus-websocket | static |
| libtom | static |
| libdatachannel | dynamic |
| libmaxminddb-1.12.2 | static |

## Libraries that do not contain their own `CMakeLists.txt`

These are usually small (often just a few files) and are compiled directly into
the main project. They previously lived in the main directory but were moved
here for better organization.

- zhicheng
- rxi-log
- ITH-SHA
- dave-g-json
- eteran-cvector
- dr_mp3 (single header file)
- kokke-tiny-aes-c

## stunnel (standalone TLS proxy)

Unlike the entries above, `stunnel` is **not** a library linked into the server —
it is a standalone executable. It terminates TLS in front of the plaintext
WebSocket so the client works on HTTPS pages (over `wss://`), and is patched to
pass the real client IP to the server as an `X-Stunnel-Client-IP` header.

It is built separately from the CMake libraries by `stunnel/_bundle_and_build.sh`,
which bundles a static OpenSSL and produces a self-contained binary. The main
build scripts call that script and copy the resulting `stunnel` next to
`chat-server` so the server can launch it on demand.

## How the libraries are used

Libraries can also be categorized by how they are used. For example,
libdatachannel depends on libviolet and mbedtls, while the main project also
uses libviolet directly.

> **TODO:** Create a dependency tree showing the relationships between the
> libraries.

## Purpose of each library

| Library | Purpose |
| --- | --- |
| dave-g-json | JSON parsing and handling |
| dr_mp3 | Decodes MP3 files to PCM for music bots (when streaming songs) |
| eteran-cvector | Lightweight `std::vector`-like container; currently experimental |
| ITH-SHA | Cryptographic hash functions (SHA) |
| kokke-tiny-aes-c | Small AES encryption implementation |
| libdatachannel | Sends WebRTC DataChannel data (used for audio transmission) |
| libmaxminddb-1.12.2 | Looks up an IP address's country from a local `.mmdb` file; used to show country flags when enabled |
| libopus-1.5.2 | Encodes PCM audio to Opus format for music bots |
| libtom | Cryptographic functionality |
| libviolet | TURN/STUN functionality for the main project; also a dependency of libdatachannel |
| mbedtls-3.6.6 | Dependency of libdatachannel; also used for general cryptographic operations |
| rxi-log | Logging |
| stunnel | Standalone TLS proxy (not linked in); terminates HTTPS/`wss` in front of the plaintext WebSocket so the chat works on HTTPS sites, forwarding the real client IP as `X-Stunnel-Client-IP`. The server can launch it automatically |
| theldus-websocket | WebSocket functionality |
| zhicheng | Base64 encoding/decoding |
