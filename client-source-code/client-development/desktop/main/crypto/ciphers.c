// Native clients, the server and the browser's WebAssembly use the same primitives.
#include "ciphers.h"
#include "cipher_aes.h"
#include "cipher_camellia.h"
#include "cipher_chacha20.h"
#include "cipher_twofish.h"
#include "cipher_serpent.h"
#include "cipher_trivium.h"
#include "mbedtls/sha256.h"
#include <string.h>

void ciphers__clear(void *data, uint64 size)
{
    volatile ubyte *bytes = data;

    // write zero bytes
    while (size > 0)
    {
        *bytes++ = 0;
        size--;
    }
}

const char *ciphers__name(int64 type)
{
    // convert type to name
    switch (type)
    {
    case LEMON_AES256:
        return "aes256";
    case LEMON_CHACHA20:
        return "chacha20";
    case LEMON_CAMELLIA256:
        return "camellia256";
    case LEMON_TWOFISH256:
        return "twofish256";
    case LEMON_SERPENT256:
        return "serpent256";
    case LEMON_TRIVIUM80:
        return "trivium80";
    default:
        return NULL;
    }
}

int64 ciphers__type(const char *name)
{
    // convert name to type
    if (name != NULL)
    {
        if (strcmp(name, "aes256") == 0)
        {
            return LEMON_AES256;
        }
        if (strcmp(name, "chacha20") == 0)
        {
            return LEMON_CHACHA20;
        }
        if (strcmp(name, "camellia256") == 0)
        {
            return LEMON_CAMELLIA256;
        }
        if (strcmp(name, "twofish256") == 0)
        {
            return LEMON_TWOFISH256;
        }
        if (strcmp(name, "trivium80") == 0)
        {
            return LEMON_TRIVIUM80;
        }
        if (strcmp(name, "serpent256") == 0)
        {
            return LEMON_SERPENT256;
        }
    }

    // no cipher found
    return 0;
}

boole ciphers__sha256(const ubyte *data, uint64 size, ubyte output[32])
{
    return size <= SIZE_MAX && mbedtls_sha256(data, size, output, 0) == 0;
}

/** @brief HMAC-SHA256 with the protocol's fixed 32-byte keys; no heap allocation. */
boole ciphers__hmac(const ubyte key[32], const ubyte *data, uint64 size, ubyte output[32])
{
    mbedtls_sha256_context context = { 0 };
    ubyte pad[64] = { 0 };
    ubyte digest[32] = { 0 };
    uint64 index = 0;
    boole status = FALSE;

    // check the size
    if (size > SIZE_MAX)
    {
        return 0;
    }

    // build the inner pad
    mbedtls_sha256_init(&context);
    for (index = 0; index < sizeof(pad); index++)
    {
        pad[index] = (index < 32 ? key[index] : 0) ^ 0x36;
    }

    // hash pad and data
    status = mbedtls_sha256_starts(&context, 0) == 0 && mbedtls_sha256_update(&context, pad, sizeof(pad)) == 0 && mbedtls_sha256_update(&context, data, size) == 0 && mbedtls_sha256_finish(&context, digest) == 0;

    // build the outer pad
    for (index = 0; index < sizeof(pad); index++)
    {
        pad[index] ^= 0x36 ^ 0x5c;
    }

    // hash pad and digest
    status = status && mbedtls_sha256_starts(&context, 0) == 0 && mbedtls_sha256_update(&context, pad, sizeof(pad)) == 0 && mbedtls_sha256_update(&context, digest, sizeof(digest)) == 0 && mbedtls_sha256_finish(&context, output) == 0;

    // clear the buffers
    mbedtls_sha256_free(&context);
    ciphers__clear(pad, sizeof(pad));
    ciphers__clear(digest, sizeof(digest));
    return status;
}

/** @brief Applies a cipher symmetrically in place. Trivium-80 compatibility deliberately ignores the message IV. */
boole ciphers__crypt(int64 type, const ubyte key[32], const ubyte iv[16], ubyte *data, uint64 size)
{
    // check arguments
    if (key == NULL || iv == NULL || data == NULL || size > SIZE_MAX)
    {
        return 0;
    }

    // choose the cipher
    switch (type)
    {
    case LEMON_AES256:
        return cipher_aes__crypt(key, iv, data, size);
    case LEMON_CHACHA20:
        return cipher_chacha20__crypt(key, iv, data, size);
    case LEMON_CAMELLIA256:
        return cipher_camellia__crypt(key, iv, data, size);
    case LEMON_TWOFISH256:
        return cipher_twofish__crypt(key, iv, data, size);
    case LEMON_SERPENT256:
        return cipher_serpent__crypt(key, iv, data, size);
    case LEMON_TRIVIUM80:
        return cipher_trivium__crypt(key, 32, data, size);
    default:
        return 0;
    }
}
