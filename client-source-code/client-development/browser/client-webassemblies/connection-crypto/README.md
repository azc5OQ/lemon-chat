# Browser connection encryption

When changing a browser cipher, edit this project and rebuild its WebAssembly
module. The browser does not compile or load source from the desktop client or
the server. Those projects own their separate implementations.

## Source layout

- `main/crypto/cipher_*.c/.h`: the supported cipher adapters and Trivium-80 implementation.
- `main/crypto/ciphers.c/.h`: cipher selection, hashing and HMAC helpers.
- `main/crypto/keys_session.c/.h`: connection key derivation.
- `main/config.h` and `main/runtime.c`: the WebAssembly library configuration and runtime support.
- `third-party/mbedtls-3.6.6`: the required source files, headers and license.
- `third-party/libtomcrypt`: Twofish/Serpent sources, headers, configuration and license.

## Build

The normal browser build compiles and tests this project automatically before
embedding it. Run `../../build.bat` or `sh ../../build.sh` from this directory.

For an isolated compile, run `node build.cjs` with Zig 0.15.2 on PATH or `ZIG`
pointing to its executable, then `node test/ciphers.cjs`. The output is
`build/connection_crypto.wasm`; an optional positional path chooses another
output. No application source paths leave this project.

For example, after editing `main/crypto/cipher_chacha20.c`, the isolated test
compares its output with OpenSSL. The normal browser build repeats that test
before publishing the module into `browser/main/wasm` and embedding it in HTML.
Editing C without rebuilding leaves the previous algorithm in the shipped page.
