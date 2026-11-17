#include "cipher_camellia.h"
#include "../../../common/clib.h"
#include "ciphers.h"
#include "mbedtls/camellia.h"
#include <string.h>

/**
 * @brief encrypts or decrypts data in place with Camellia CTR
 *
 * @param const unsigned char[32] key -> 256 bit key. Must not be NULL
 * @param const unsigned char[16] iv -> initial counter. Must not be NULL
 * @param unsigned char* data -> buffer to update. Must not be NULL
 * @param size_t size -> number of bytes in data
 *
 * @return int -> 1 on success. 0 on failure
 */
int cipher_camellia__crypt(const unsigned char key[32], const unsigned char iv[16], unsigned char* data, size_t size)
{
    mbedtls_camellia_context context;
    unsigned char counter[16];
    unsigned char stream[16] = {0};
    size_t offset = 0;
    int status;

    clib__copy_memory(iv, counter, sizeof(counter), sizeof(counter));
    mbedtls_camellia_init(&context);
    status = mbedtls_camellia_setkey_enc(&context, key, 256) == 0
        && mbedtls_camellia_crypt_ctr(&context, size, &offset, counter, stream, data, data) == 0;
    mbedtls_camellia_free(&context);
    ciphers__clear(counter, sizeof(counter));
    ciphers__clear(stream, sizeof(stream));
    return status;
}
