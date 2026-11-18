#include "cipher_chacha20.h"
#include "../../../common/clib.h"
#include "ciphers.h"
#include "mbedtls/chacha20.h"
#include <string.h>

/**
 * @brief encrypts or decrypts data in place with ChaCha20
 *
 * @param const unsigned char[32] key -> 256 bit key. Must not be NULL
 * @param const unsigned char[16] iv -> full protocol IV used to derive the subkey. Must not be NULL
 * @param unsigned char* data -> buffer to update. Must not be NULL
 * @param size_t size -> number of bytes in data
 *
 * @return int -> 1 on success. 0 on failure
 */
int cipher_chacha20__crypt(const unsigned char key[32], const unsigned char iv[16], unsigned char *data, size_t size)
{
    // derive a subkey from the full IV so no IV bits are discarded
    static const unsigned char domain[] = "lemonchat-chacha20-iv-v2";
    unsigned char input[sizeof(domain) - 1 + 16];
    unsigned char subkey[32];
    unsigned char nonce[12] = { 0 };
    int status;

    clib__copy_memory(domain, input, sizeof(domain) - 1, sizeof(input));
    clib__copy_memory(iv, input + sizeof(domain) - 1, 16, sizeof(input) - (sizeof(domain) - 1));
    status = size <= UINT32_MAX && ciphers__hmac(key, input, sizeof(input), subkey) && mbedtls_chacha20_crypt(subkey, nonce, 0, size, data, data) == 0;
    ciphers__clear(subkey, sizeof(subkey));
    ciphers__clear(input, sizeof(input));
    return status;
}
