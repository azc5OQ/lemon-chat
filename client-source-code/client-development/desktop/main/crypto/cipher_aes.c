#include "cipher_aes.h"
#include "ciphers.h"
#include "mbedtls/aes.h"
#include <string.h>

// AES-256-CTR, in place. Returns 1 on success, 0 on failure.
// The key, IV and data must be non-NULL; encryption and decryption are identical.
boole cipher_aes__crypt(const ubyte key[32], const ubyte iv[16], ubyte *data, uint64 size)
{
    mbedtls_aes_context context = { 0 };
    ubyte counter[16] = { 0 };
    ubyte stream[16] = { 0 };
    size_t offset = 0; // Mbed TLS updates this through a size_t pointer.
    boole status = FALSE;

    if (size > SIZE_MAX)
    {
        return 0;
    }
    memcpy(counter, iv, sizeof(counter));
    mbedtls_aes_init(&context);
    status = mbedtls_aes_setkey_enc(&context, key, 256) == 0 && mbedtls_aes_crypt_ctr(&context, size, &offset, counter, stream, data, data) == 0;
    mbedtls_aes_free(&context);
    ciphers__clear(counter, sizeof(counter));
    ciphers__clear(stream, sizeof(stream));
    return status;
}
