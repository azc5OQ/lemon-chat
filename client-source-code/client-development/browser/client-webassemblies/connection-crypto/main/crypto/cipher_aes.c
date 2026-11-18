#include "cipher_aes.h"
#include "../../../common/clib.h"
#include "ciphers.h"
#include "mbedtls/aes.h"
#include <string.h>

/**
 * @brief encrypts or decrypts data in place with AES CTR
 *
 * @param const unsigned char[32] key -> 256 bit key. Must not be NULL
 * @param const unsigned char[16] iv -> initial counter. Must not be NULL
 * @param unsigned char* data -> buffer to update. Must not be NULL
 * @param size_t size -> number of bytes in data
 *
 * @return int -> 1 on success. 0 on failure
 */
int cipher_aes__crypt(const unsigned char key[32], const unsigned char iv[16], unsigned char *data, size_t size)
{
    mbedtls_aes_context context;
    unsigned char counter[16];
    unsigned char stream[16] = { 0 };
    size_t offset = 0;
    int status;

    clib__copy_memory(iv, counter, sizeof(counter), sizeof(counter));
    mbedtls_aes_init(&context);
    status = mbedtls_aes_setkey_enc(&context, key, 256) == 0 && mbedtls_aes_crypt_ctr(&context, size, &offset, counter, stream, data, data) == 0;
    mbedtls_aes_free(&context);
    ciphers__clear(counter, sizeof(counter));
    ciphers__clear(stream, sizeof(stream));
    return status;
}
