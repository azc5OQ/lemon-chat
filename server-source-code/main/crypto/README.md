# Server crypto

## Naming

When debugging a failed login proof, start in `auth_handshake.c`; its public
functions start with `auth_handshake__`. If the problem is the derived keys,
follow the calls into `keys_session.c` and `keys_session__...`.

Names use `<responsibility>_<subject>`: `auth_*` handles authentication,
`keys_*` handles key derivation and management, and `crypto_*` protects a
particular kind of data with encryption and authentication. Public function
prefixes match their filename. Algorithm adapters use `cipher_<algorithm>`,
such as `cipher_chacha20.c` and `cipher_chacha20__crypt`. The DH constants live in
`dh_parameters.h`.

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

When a client connects, `auth_handshake.c` handles the negotiated login proof and
`keys_session.c` derives its session keys. `dh_parameters.h` holds the public DH
parameters; `auth_rsa.c` contains the RSA public-key inspection and
encryption helpers used during login.

Once connected, `crypto_metadata.c` protects the message envelopes and manages
connection password snapshots. `crypto_media.c` protects audio and video packets.
`crypto_storage.c` encrypts and authenticates optional music snapshots before they reach disk.
These connection helpers live beside the algorithm adapters so the complete
encryption path is in this directory.

The desktop keeps matching algorithm files in its own `src/crypto`. Update
both copies together. Each native application builds from its own source tree.
The older tiny-AES implementation remains in `../../third-party/kokke-tiny-aes-c`;
it is separate from the Mbed TLS adapter here.

See the [protocol document](../../../docs/connection-encryption-protocol.md)
for message formats and connection key ownership.
