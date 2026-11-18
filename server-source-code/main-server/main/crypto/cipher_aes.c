#include "cipher_aes.h"
#include "../clib/clib.h"
#include "ciphers.h"
#include "mbedtls/aes.h"
#include <string.h>

/**
 * @brief encrypts or decrypts data in place with AES CTR
 *
 * @param const ubyte[32] key -> 256 bit key. Must not be NULL
 * @param const ubyte[16] iv -> initial counter. Must not be NULL
 * @param ubyte* data -> buffer to update. Must not be NULL
 * @param uint64 size -> number of bytes in data
 *
 * @return boole -> 1 on success. 0 on failure
 */
boole cipher_aes__crypt(const ubyte key[32], const ubyte iv[16], ubyte *data, uint64 size)
{
    mbedtls_aes_context context = { 0 };
    ubyte counter[16] = { 0 };
    ubyte stream[16] = { 0 };
    size_t offset = 0; // Mbed TLS updates this through a size_t pointer
    boole status = FALSE;

    // check the size
    if (size > SIZE_MAX)
    {
        return 0;
    }

    // apply the cipher
    clib__copy_memory(iv, counter, sizeof(counter), sizeof(counter));
    mbedtls_aes_init(&context);
    status = mbedtls_aes_setkey_enc(&context, key, 256) == 0 && mbedtls_aes_crypt_ctr(&context, size, &offset, counter, stream, data, data) == 0;

    // clear the cipher state
    mbedtls_aes_free(&context);
    ciphers__clear(counter, sizeof(counter));
    ciphers__clear(stream, sizeof(stream));
    return status;
}
