#include "cipher_aes.h"
#include "ciphers.h"
#include "mbedtls/aes.h"
#include <string.h>

// AES-256-CTR, in place. Returns 1 on success, 0 on failure.
// The key, IV and data must be non-NULL; encryption and decryption are identical.
int cipher_aes__crypt(const unsigned char key[32], const unsigned char iv[16], unsigned char* data, size_t size)
{
    mbedtls_aes_context context;
    unsigned char counter[16];
    unsigned char stream[16] = {0};
    size_t offset = 0;
    int status;

    memcpy(counter, iv, sizeof(counter));
    mbedtls_aes_init(&context);
    status = mbedtls_aes_setkey_enc(&context, key, 256) == 0
        && mbedtls_aes_crypt_ctr(&context, size, &offset, counter, stream, data, data) == 0;
    mbedtls_aes_free(&context);
    ciphers__clear(counter, sizeof(counter));
    ciphers__clear(stream, sizeof(stream));
    return status;
}
