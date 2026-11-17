#include "cipher_chacha20.h"
#include "../clib/clib.h"
#include "ciphers.h"
#include "mbedtls/chacha20.h"
#include <string.h>

/**
 * @brief encrypts or decrypts data in place with ChaCha20
 *
 * @param const ubyte[32] key -> 256 bit key. Must not be NULL
 * @param const ubyte[16] iv -> full protocol IV used to derive the subkey. Must not be NULL
 * @param ubyte* data -> buffer to update. Must not be NULL
 * @param uint64 size -> number of bytes in data
 *
 * @return boole -> 1 on success. 0 on failure
 */
boole cipher_chacha20__crypt(const ubyte key[32], const ubyte iv[16], ubyte *data, uint64 size)
{
    // derive a subkey from the full IV so no IV bits are discarded
    static const ubyte domain[] = "lemonchat-chacha20-iv-v2";
    ubyte input[sizeof(domain) - 1 + 16] = { 0 };
    ubyte subkey[32] = { 0 };
    ubyte nonce[12] = { 0 };
    boole status = FALSE;

    // join domain and IV
    clib__copy_memory(domain, input, sizeof(domain) - 1, sizeof(input));
    clib__copy_memory(iv, input + sizeof(domain) - 1, 16, sizeof(input) - (sizeof(domain) - 1));

    // encrypt with a subkey
    status = size <= UINT32_MAX && ciphers__hmac(key, input, sizeof(input), subkey) && mbedtls_chacha20_crypt(subkey, nonce, 0, size, data, data) == 0;

    // clear the buffers
    ciphers__clear(subkey, sizeof(subkey));
    ciphers__clear(input, sizeof(input));
    return status;
}
