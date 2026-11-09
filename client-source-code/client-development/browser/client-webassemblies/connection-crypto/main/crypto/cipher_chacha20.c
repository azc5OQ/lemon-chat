#include "cipher_chacha20.h"
#include "ciphers.h"
#include "mbedtls/chacha20.h"
#include <string.h>

// ChaCha20 with the protocol's 128-bit IV adapter, in place.
// The key, IV and data must be non-NULL; encryption and decryption are identical.
// Returns 1 on success, 0 on failure.
int cipher_chacha20__crypt(const unsigned char key[32], const unsigned char iv[16], unsigned char* data, size_t size)
{
    // The 128-bit envelope IV derives a per-message subkey. This avoids
    // truncating it to ChaCha20's 96-bit nonce, including for media counters.
    static const unsigned char domain[] = "lemonchat-chacha20-iv-v2";
    unsigned char input[sizeof(domain) - 1 + 16];
    unsigned char subkey[32];
    unsigned char nonce[12] = {0};
    int status;

    memcpy(input, domain, sizeof(domain) - 1);
    memcpy(input + sizeof(domain) - 1, iv, 16);
    status = size <= UINT32_MAX && ciphers__hmac(key, input, sizeof(input), subkey)
        && mbedtls_chacha20_crypt(subkey, nonce, 0, size, data, data) == 0;
    ciphers__clear(subkey, sizeof(subkey));
    ciphers__clear(input, sizeof(input));
    return status;
}
