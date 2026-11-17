// native clients, the server and the browser's WebAssembly use the same primitives
#include "ciphers.h"
#include "cipher_aes.h"
#include "cipher_camellia.h"
#include "cipher_chacha20.h"
#include "cipher_twofish.h"
#include "cipher_serpent.h"
#include "cipher_trivium.h"
#include "mbedtls/sha256.h"
#include <string.h>

/**
 * @brief clears sensitive bytes through a volatile pointer
 *
 * @param void* data -> buffer to clear
 * @param size_t size -> number of bytes in the buffer
 *
 * @return void
 */
void ciphers__clear(void* data, size_t size)
{
    volatile unsigned char* bytes = data;
    while (size > 0)
    {
        *bytes++ = 0;
        size--;
    }
}

/**
 * @brief finds the protocol name of a supported cipher
 *
 * @param int type -> cipher identifier
 *
 * @return const char* -> static name, or NULL for an unsupported cipher
 */
const char* ciphers__name(int type)
{
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

/**
 * @brief finds a cipher by its exact protocol name
 *
 * @param const char* name -> cipher name, or NULL
 *
 * @return int -> cipher identifier, or 0 for an unknown name
 */
int ciphers__type(const char* name)
{
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
    return 0;
}

/**
 * @brief calculates a SHA256 digest
 *
 * @param const unsigned char* data -> bytes to hash
 * @param size_t size -> number of input bytes
 * @param unsigned char[32] output -> receives the digest
 *
 * @return int -> 1 on success, otherwise 0
 */
int ciphers__sha256(const unsigned char* data, size_t size, unsigned char output[32])
{
    return mbedtls_sha256(data, size, output, 0) == 0;
}

/**
 * @brief calculates HMAC using SHA256 without heap allocation
 *
 * @param const unsigned char key[32] -> authentication key
 * @param const unsigned char* data -> message bytes
 * @param size_t size -> message length
 * @param unsigned char output[32] -> receives the tag
 *
 * @return int -> 1 if hashing succeeded, otherwise 0
 */
int ciphers__hmac(const unsigned char key[32], const unsigned char* data, size_t size, unsigned char output[32])
{
    mbedtls_sha256_context context;
    unsigned char pad[64];
    unsigned char digest[32];
    size_t index;
    int status;

    mbedtls_sha256_init(&context);
    for (index = 0; index < sizeof(pad); index++)
    {
        pad[index] = (index < 32 ? key[index] : 0) ^ 0x36;
    }
    status = mbedtls_sha256_starts(&context, 0) == 0
        && mbedtls_sha256_update(&context, pad, sizeof(pad)) == 0
        && mbedtls_sha256_update(&context, data, size) == 0
        && mbedtls_sha256_finish(&context, digest) == 0;
    for (index = 0; index < sizeof(pad); index++)
    {
        pad[index] ^= 0x36 ^ 0x5c;
    }
    status = status && mbedtls_sha256_starts(&context, 0) == 0
        && mbedtls_sha256_update(&context, pad, sizeof(pad)) == 0
        && mbedtls_sha256_update(&context, digest, sizeof(digest)) == 0
        && mbedtls_sha256_finish(&context, output) == 0;
    mbedtls_sha256_free(&context);
    ciphers__clear(pad, sizeof(pad));
    ciphers__clear(digest, sizeof(digest));
    return status;
}

/**
 * @brief applies a cipher in place. Trivium compatibility ignores the message IV
 *
 * @param int type -> cipher identifier
 * @param const unsigned char key[32] -> encryption key
 * @param const unsigned char iv[16] -> initialization vector
 * @param unsigned char* data -> bytes to transform
 * @param size_t size -> number of bytes
 *
 * @return int -> 1 if cipher processing succeeded, otherwise 0
 */
int ciphers__crypt(int type, const unsigned char key[32], const unsigned char iv[16], unsigned char* data, size_t size)
{
    if (key == NULL || iv == NULL || data == NULL)
    {
        return 0;
    }
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
