# Client crypto

## Naming

When debugging an encrypted audio packet, start in `crypto_media.c`; its public
functions start with `crypto_media__`. If the problem is the derived session
keys, follow the calls into `keys_session.c` and `keys_session__...`.

Names use `<responsibility>_<subject>`: `auth_*` handles authentication,
`keys_*` handles key derivation and management, and `crypto_*` protects a
particular kind of data with encryption and authentication. Public function
prefixes match their filename. Algorithm adapters use `cipher_<algorithm>`,
such as `cipher_chacha20.c` and `cipher_chacha20__crypt`. The DH constants live in
`dh_parameters.h`.
The server follows the same convention in its crypto directory.

## Files and dependencies

Suppose you want to follow a ChaCha20 encryption call. Open `cipher_chacha20.h` for
its function signature, then `cipher_chacha20.c` for the IV adapter and Mbed TLS call.
Every supported cipher has the same visible `.c`/`.h` pair in this flat directory:

- `cipher_aes.c` / `cipher_aes.h`: AES-256-CTR through Mbed TLS.
- `cipher_chacha20.c` / `cipher_chacha20.h`: ChaCha20 through Mbed TLS.
- `cipher_camellia.c` / `cipher_camellia.h`: Camellia-256-CTR through Mbed TLS.
- `cipher_twofish.c` / `cipher_twofish.h`: Twofish-256-CTR through LibTomCrypt.
- `cipher_serpent.c` / `cipher_serpent.h`: Serpent-256-CTR through LibTomCrypt.
- `cipher_trivium.c` / `cipher_trivium.h`: exact Trivium-80 compatibility, including its fixed IV and byte ordering.

`ciphers.c` maps names and IDs to these functions. It also supplies the common
SHA-256, HMAC and memory-clearing helpers. A caller uses `ciphers__crypt`
when its cipher is chosen at runtime; this checks the pointers before dispatch.
The individual adapters expect valid buffers, encrypt or decrypt in place,
and return 1 for success or 0 for failure.

Library dependencies stay in `../../third-party`: Mbed TLS supplies AES,
Camellia, ChaCha20 and SHA-256; LibTomCrypt supplies Twofish and Serpent. Its
`lemon_config.h` selects those algorithms and prefixes their symbols. The
upstream source, license and pinned revision stay together in that library.

When a client connects, `crypto_session.c` handles its identity, login and
encrypted message envelopes. `dh_parameters.h` holds the DH parameters and
`keys_session.c` derives the negotiated session keys. `crypto_media.c`
protects audio and video packets. These connection helpers live beside the
algorithm adapters so the complete encryption path is in this directory.

The server keeps matching algorithm files in its own `main/crypto`. Update
both copies together. The browser's WebAssembly module uses these client-side
sources and dependencies; neither client build reads the server source tree.
The older tiny-AES implementation remains in `../../third-party/kokke-tiny-aes-c`;
its existing `lemon-aes` runtime library is built by the third-party CMake file.

See the [protocol document](../../../../../docs/connection-encryption-protocol.md)
for message formats and key derivation.
