#include "cipher_camellia.h"
#include "ciphers.h"
#include "mbedtls/camellia.h"
#include <string.h>

// Camellia-256-CTR, in place. Returns 1 on success, 0 on failure.
// The key, IV and data must be non-NULL; encryption and decryption are identical.
int cipher_camellia__crypt(const unsigned char key[32], const unsigned char iv[16], unsigned char* data, size_t size)
{
    mbedtls_camellia_context context;
    unsigned char counter[16];
    unsigned char stream[16] = {0};
    size_t offset = 0;
    int status;

    memcpy(counter, iv, sizeof(counter));
    mbedtls_camellia_init(&context);
    status = mbedtls_camellia_setkey_enc(&context, key, 256) == 0
        && mbedtls_camellia_crypt_ctr(&context, size, &offset, counter, stream, data, data) == 0;
    mbedtls_camellia_free(&context);
    ciphers__clear(counter, sizeof(counter));
    ciphers__clear(stream, sizeof(stream));
    return status;
}
