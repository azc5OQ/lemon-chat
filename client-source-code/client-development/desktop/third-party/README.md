# Desktop client libraries

The desktop client keeps the libraries it needs in this directory. This makes the
build reproducible and avoids requiring a separately installed copy of each
library. Keep every library's license when distributing the client.

## Included libraries

- `meek-ui` draws the desktop interface and the game overlay. The bundled fonts
  and their license are included with it.
- `minhook` installs the Direct3D 11 hooks used by the optional Windows overlay.
- `dave-g-json` provides cJSON for reading and writing protocol data.
- `kokke-tiny-aes-c` provides the existing AES-256 CTR implementation.
- `libopus-1.5.2` encodes and decodes voice audio.
- `mbedtls-3.6.6` provides the cryptographic operations used by identities and
  encrypted sessions.
- `identity-keygen` contains the same public-key identity calculation used by the
  browser client.
- `miniaudio-0.11.26` opens microphones and speakers.
- `libfvad` detects speech for voice activation. It does not provide WebRTC
  networking.

Some libraries are also used by the Lemon Chat server. Their local copies should
be updated together when a shared implementation changes.

## Build and distribution

CMake builds the required libraries with the desktop client. Build output and
caches do not belong in this directory.

The packaged Windows client uses these shared libraries:

- `meek_ui.dll` for the desktop window
- `opus.dll` for voice encoding and decoding
- `lemon-audio.dll` for microphone and speaker access
- `lemon-vad.dll` for voice activation
- `lemon-aes.dll`, `lemon-json.dll` and `lemon-crypto.dll` for encryption and
  protocol data
- `lemon-overlay-d3d11.dll` for the optional game overlay

The overlay DLL contains its own small Meek UI renderer. Networking, encryption
keys and audio processing stay in the desktop application.

Windows uses WinHTTP for WebSocket connections. Linux and macOS use the system's
libcurl development files with WebSocket support.

Third-party source keeps its original coding style. Lemon Chat's application
style rules apply only to code outside this directory.
