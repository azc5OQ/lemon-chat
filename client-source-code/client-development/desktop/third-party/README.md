# Desktop dependencies

Meek UI's library sources were updated from the upstream project on 2026-11-01. The copy
contains `src`, `backends`, `cmake`, the upstream CMake file, README, and the
Space Grotesk font with its license. Library sources are unchanged. Examples,
test fixtures, build directories and caches are excluded. Upstream shader build
helpers required by its CMake configuration are retained.

This revision adds owned text buffers, validated text replacement, measurement
scroll preservation and stable style handles. Its
[migration notes](meek-ui/docs/integration-updates.md) are included. Rebuild the
desktop executable, `meek_ui.dll` and `lemon-overlay-d3d11.dll` together; the new
node layout is incompatible with binaries built against the old headers.

MinHook comes from that checkout's `examples/embedded_game_overlay/third_party`
directory. Its source and license are retained unchanged. The Lemon overlay
uses its D3D11 Present/ResizeBuffers hooks with Meek UI's embedded renderer;
the example application's menus and input hooks are not included.

cJSON, tiny-AES-c, Opus 1.5.2 and Mbed TLS 3.6.6 are included here. Their sources
were copied from `server-source-code/third-party` on 2026-11-01,
including Lemon Chat's existing tiny-AES changes. The desktop builds its own
shared libraries from these local copies; it does not require the server source
directory. Keep these copies in sync when updating the server libraries.

- `dave-g-json/`: cJSON source and header; license in `cJSON-LICENSE.txt`.
  Two unused includes of server-only headers were removed from `cJSON.c`;
  its parser and allocation functions are unchanged.
- `kokke-tiny-aes-c/`: the server's AES-256 CTR source and header.
- `libopus-1.5.2/`: codec sources, headers, CMake support and licenses.
- `mbedtls-3.6.6/`: library sources, headers and license. The desktop CMake target
  selects the crypto primitives needed for identity and session encryption.
- `identity-keygen/rsa_keygen.c`: the browser's existing deterministic identity
  calculation, copied unchanged from `browser/client-webassemblies/rsa-keygen/src`.
  The wrapper removes only WebAssembly export attributes for native compilation.
  The identity passphrase, key size and resulting public key keep their existing
  meaning; this is not a new identity format.
- `miniaudio-0.11.26/`: the upstream single-header device library and license,
  from <https://github.com/mackron/miniaudio/tree/0.11.26>. Its sources are
  unchanged. The small `miniaudio.c` translation unit builds `lemon-audio` as
  a shared library with file decoding, encoding, resource management and the
  high-level engine disabled; Opus remains the chat codec. The SHA-256 of
  `miniaudio.h` is `6b376c7fbb1352ba1e0e432ab66069b335ea9405e45cb6c30b97ef0f3c9c3e0e`.

Build output and caches are excluded. Upstream support files inside the copied
codec source directories are retained; the desktop build does not run their tools.
Application clib is not injected into these dependencies. WinHTTP on Windows
and libcurl on Linux/macOS provide the platform WebSocket transport; libcurl
development files must be installed on those platforms.

`libfvad/` contains the unchanged WebRTC speech detector fork, including its BSD
license, patent grant and upstream PCM fixtures. The exact revision and archive
checksum are recorded in `libfvad-source.txt`. It builds as `lemon-vad`, a shared
library used by the desktop audio worker. It does not add WebRTC networking.

The desktop window uses `meek_ui.dll`. The game module includes a private Meek UI
embedded renderer inside `lemon-overlay-d3d11.dll`, so the game does not need to
locate the desktop's UI DLL or load its window backend. Networking, Opus and AES
remain outside the game process.
