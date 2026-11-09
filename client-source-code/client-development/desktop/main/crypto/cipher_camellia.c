#include "cipher_camellia.h"
#include "ciphers.h"
#include "mbedtls/camellia.h"
#include <string.h>

// Camellia-256-CTR, in place. Returns 1 on success, 0 on failure.
// The key, IV and data must be non-NULL; encryption and decryption are identical.
boole cipher_camellia__crypt(const ubyte key[32], const ubyte iv[16], ubyte *data, uint64 size)
{
    mbedtls_camellia_context context = { 0 };
    ubyte counter[16] = { 0 };
    ubyte stream[16] = { 0 };
    size_t offset = 0; // Mbed TLS updates this through a size_t pointer.
    boole status = FALSE;

    if (size > SIZE_MAX)
    {
        return 0;
    }
    memcpy(counter, iv, sizeof(counter));
    mbedtls_camellia_init(&context);
    status = mbedtls_camellia_setkey_enc(&context, key, 256) == 0 && mbedtls_camellia_crypt_ctr(&context, size, &offset, counter, stream, data, data) == 0;
    mbedtls_camellia_free(&context);
    ciphers__clear(counter, sizeof(counter));
    ciphers__clear(stream, sizeof(stream));
    return status;
}
