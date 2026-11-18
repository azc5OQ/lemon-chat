#include "crypto_storage.h"
#include "../clib/clib.h"
#include "ciphers.h"
#include "keys_session.h"
#include "../util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// header: magic[8], layer count[1], cipher IDs[100], random salt[32], IV[16]
// the trailing 32-byte tag covers the complete header and stored payload
#define STORAGE_HEADER_SIZE 157
#define STORAGE_SALT_OFFSET 109
#define STORAGE_IV_OFFSET 141

/**
 * @brief derives a snapshot authentication key from ordered password keys
 *
 * @param const crypto_key_t * keys -> ordered password keys
 * @param uint64 count -> number of keys
 * @param const ubyte [32] salt -> snapshot salt
 * @param ubyte [32] result -> receives the 32 byte authentication key
 *
 * @return boole -> TRUE when every derivation step succeeds
 */
static boole _crypto_storage_internal__mac_key(const crypto_key_t *keys, uint64 count, const ubyte salt[32], ubyte result[32])
{
    uint64 index = 0;
    ubyte combined[32] = { 0 };
    ubyte entry[33] = { 0 };
    boole success = FALSE;

    // combine all keys
    for (index = 0; index < count; index++)
    {
        entry[0] = (ubyte)keys[index].type;
        clib__copy_memory(keys[index].material.raw, entry + 1, 32, sizeof(entry) - 1);
        if (!keys_session__mix(combined, entry, sizeof(entry)))
        {
            goto done;
        }
    }

    // create the MAC key
    success = keys_session__hkdf(combined, salt, "lemonchat/music-bots/auth/v1", result);
done:
    // clear the buffers
    ciphers__clear(combined, sizeof(combined));
    ciphers__clear(entry, sizeof(entry));
    return success;
}

/**
 * @brief applies snapshot cipher layers in encryption or reverse decryption order
 *
 * @param const crypto_key_t * keys -> ordered password keys
 * @param uint64 count -> number of layers
 * @param ubyte * file -> snapshot header followed by the mutable payload
 * @param uint64 size -> payload byte count
 * @param boole encrypt -> TRUE to encrypt or FALSE to decrypt
 *
 * @return boole -> TRUE when every cipher layer succeeds
 */
static boole _crypto_storage_internal__layers(const crypto_key_t *keys, uint64 count, ubyte *file, uint64 size, boole encrypt)
{
    uint64 step = 0;
    uint64 index = 0;
    ubyte key[32] = { 0 };
    char purpose[80] = { 0 };
    boole success = FALSE;

    // apply every cipher layer
    for (step = 0; step < count; step++)
    {
        index = encrypt ? step : count - 1 - step;
        snprintf(purpose, sizeof(purpose), "lemonchat/music-bots/layer/v1/%llu/%u", (uint64)index, (uint)keys[index].type);

        // each snapshot has fresh derived keys, including for fixed-IV ciphers
        if (!keys_session__hkdf(keys[index].material.raw, file + STORAGE_SALT_OFFSET, purpose, key) || !ciphers__crypt(keys[index].type, key, file + STORAGE_IV_OFFSET, file + STORAGE_HEADER_SIZE, size))
        {
            goto done;
        }
    }
    success = TRUE;
done:
    // clear the key
    ciphers__clear(key, sizeof(key));
    return success;
}

// encrypts a RAM payload with the configured password layers. The caller frees
// the returned buffer. With no keys, a checksum detects accidental corruption
/**
 * @brief builds a snapshot envelope using the configured password keys
 * free the returned file buffer after use
 * without keys the payload remains plaintext and uses an unkeyed checksum
 *
 * @param const crypto_key_t * keys -> ordered password keys
 * @param uint64 count -> number of keys from zero to 100
 * @param const ubyte * data -> snapshot payload bytes
 * @param uint64 size -> payload byte count
 * @param ubyte ** output -> receives an allocated file buffer owned by the caller
 * @param uint64 * output_size -> receives the file byte count
 *
 * @return boole -> TRUE when built, otherwise FALSE with empty outputs
 */
boole crypto_storage__seal(const crypto_key_t *keys, uint64 count, const ubyte *data, uint64 size, ubyte **output, uint64 *output_size)
{
    uint64 index = 0;
    ubyte key[32] = { 0 };
    ubyte *file = NULL_POINTER;
    uint64 total;
    boole success = FALSE;

    // reset the outputs
    *output = NULL_POINTER;
    *output_size = 0;

    // check count and size
    if (count > 100 || size > SIZE_MAX - STORAGE_HEADER_SIZE - 32)
    {
        return FALSE;
    }

    // allocate the file buffer
    total = STORAGE_HEADER_SIZE + size + 32;
    file = calloc(total, 1);
    if (file == NULL_POINTER)
    {
        return FALSE;
    }

    // write the header
    clib__copy_memory("LCMUS01", file, 8, total);
    file[8] = (ubyte)count;
    for (index = 0; index < count; index++)
    {
        file[9 + index] = (ubyte)keys[index].type;
    }

    // copy the data
    clib__copy_memory(data, file + STORAGE_HEADER_SIZE, size, total - STORAGE_HEADER_SIZE);

    // create salt, then encrypt
    if (!util__fill_secure_random_bytes(file + STORAGE_SALT_OFFSET, 48) || !_crypto_storage_internal__layers(keys, count, file, size, TRUE))
    {
        goto done;
    }

    // write the tag
    if (count != 0)
    {
        if (!_crypto_storage_internal__mac_key(keys, count, file + STORAGE_SALT_OFFSET, key) || !ciphers__hmac(key, file, total - 32, file + total - 32))
        {
            goto done;
        }
    }
    else if (!ciphers__sha256(file, total - 32, file + total - 32))
    {
        goto done;
    }

    // set the outputs
    *output = file;
    *output_size = total;
    success = TRUE;
done:
    // clear the key
    ciphers__clear(key, sizeof(key));

    // free file on failure
    if (!success)
    {
        ciphers__clear(file, total);
        free(file);
    }
    return success;
}

// authenticates before decrypting in place. The returned payload points into file
/**
 * @brief checks a snapshot envelope before decrypting its payload in place
 * the returned payload shares the file buffer and must not be freed separately
 *
 * @param const crypto_key_t * keys -> ordered password keys
 * @param uint64 count -> number of supplied keys
 * @param ubyte * file -> mutable snapshot file buffer
 * @param uint64 file_size -> file byte count
 * @param ubyte ** data -> receives a borrowed pointer into the file payload
 * @param uint64 * size -> receives the payload byte count
 *
 * @return boole -> TRUE when the envelope and decryption succeed
 */
boole crypto_storage__open(const crypto_key_t *keys, uint64 count, ubyte *file, uint64 file_size, ubyte **data, uint64 *size)
{
    uint64 verification_index = 0;
    uint64 index = 0;
    ubyte key[32] = { 0 };
    ubyte expected[32] = { 0 };
    ubyte difference = 0;
    boole success = FALSE;

    // reset the outputs
    *data = NULL_POINTER;
    *size = 0;

    // check the header
    if (file_size > SIZE_MAX || file_size < STORAGE_HEADER_SIZE + 32 || memcmp(file, "LCMUS01", 8) != 0 || file[8] > 100)
    {
        return FALSE;
    }

    // calculate the expected tag
    if (file[8] != 0)
    {
        // compare the layer count
        if (count != file[8])
        {
            return FALSE;
        }

        // compare the cipher types
        for (verification_index = 0; verification_index < count; verification_index++)
        {
            if (file[9 + verification_index] != keys[verification_index].type)
            {
                return FALSE;
            }
        }

        // calculate the HMAC tag
        if (!_crypto_storage_internal__mac_key(keys, count, file + STORAGE_SALT_OFFSET, key) || !ciphers__hmac(key, file, file_size - 32, expected))
        {
            goto done;
        }
    }
    else if (!ciphers__sha256(file, file_size - 32, expected))
    {
        goto done;
    }

    // compare the tags
    for (index = 0; index < 32; index++)
    {
        difference |= expected[index] ^ file[file_size - 32 + index];
    }

    // decrypt when tags match
    if (difference != 0 || !_crypto_storage_internal__layers(keys, file[8], file, file_size - STORAGE_HEADER_SIZE - 32, FALSE))
    {
        goto done;
    }

    // set the outputs
    *data = file + STORAGE_HEADER_SIZE;
    *size = file_size - STORAGE_HEADER_SIZE - 32;
    success = TRUE;
done:
    // clear the buffers
    ciphers__clear(key, sizeof(key));
    ciphers__clear(expected, sizeof(expected));
    return success;
}
