#include "cipher_chacha20.h"
#include "ciphers.h"
#include "mbedtls/chacha20.h"
#include <string.h>

// ChaCha20 with the protocol's 128-bit IV adapter, in place.
// The key, IV and data must be non-NULL; encryption and decryption are identical.
// Returns 1 on success, 0 on failure.
boole cipher_chacha20__crypt(const ubyte key[32], const ubyte iv[16], ubyte *data, uint64 size)
{
    // the 128-bit envelope IV derives a per-message subkey. this avoids
    // truncating it to ChaCha20's 96-bit nonce, including for media counters
    static const ubyte domain[] = "lemonchat-chacha20-iv-v2";
    ubyte input[sizeof(domain) - 1 + 16] = { 0 };
    ubyte subkey[32] = { 0 };
    ubyte nonce[12] = { 0 };
    boole status = FALSE;

    // join domain and IV
    memcpy(input, domain, sizeof(domain) - 1);
    memcpy(input + sizeof(domain) - 1, iv, 16);

    // encrypt with a subkey
    status = size <= UINT32_MAX && ciphers__hmac(key, input, sizeof(input), subkey) && mbedtls_chacha20_crypt(subkey, nonce, 0, size, data, data) == 0;

    // clear the buffers
    ciphers__clear(subkey, sizeof(subkey));
    ciphers__clear(input, sizeof(input));
    return status;
}
