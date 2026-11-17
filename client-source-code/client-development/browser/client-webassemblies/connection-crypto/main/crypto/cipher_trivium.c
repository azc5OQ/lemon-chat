#include "cipher_trivium.h"
#include "ciphers.h"

/**
 * @brief advances the Trivium registers by 32 bits
 *
 * @param uint32_t[10] state -> registers packed into 3, 3 and 4 words
 *
 * @return uint32_t -> the next 32 bits of keystream
 */
// state words must remain 32 bits. Unused high bits are not read
static uint32_t _cipher_trivium_internal__next_word(uint32_t state[10])
{
    uint32_t first = ((state[2] << 30) | (state[1] >> 2)) ^ ((state[2] << 3) | (state[1] >> 29));
    uint32_t second = ((state[5] << 27) | (state[4] >> 5)) ^ ((state[5] << 12) | (state[4] >> 20));
    uint32_t third = ((state[8] << 30) | (state[7] >> 2)) ^ ((state[9] << 17) | (state[8] >> 15));
    uint32_t output = first ^ second ^ third;

    first ^= (((state[2] << 5) | (state[1] >> 27)) & ((state[2] << 4) | (state[1] >> 28)))
        ^ ((state[5] << 18) | (state[4] >> 14));
    second ^= (((state[5] << 14) | (state[4] >> 18)) & ((state[5] << 13) | (state[4] >> 19)))
        ^ ((state[8] << 9) | (state[7] >> 23));
    third ^= (((state[9] << 19) | (state[8] >> 13)) & ((state[9] << 18) | (state[8] >> 14)))
        ^ ((state[2] << 27) | (state[1] >> 5));

    state[2] = state[1];
    state[1] = state[0];
    state[0] = third;
    state[5] = state[4];
    state[4] = state[3];
    state[3] = first;
    state[9] = state[8];
    state[8] = state[7];
    state[7] = state[6];
    state[6] = second;
    return output;
}

/**
 * @brief encrypts or decrypts data in place with the legacy Trivium variant
 *
 * @param const unsigned char* key -> input bytes hashed to derive the cipher key
 * @param size_t key_size -> number of bytes in key
 * @param unsigned char* data -> buffer to update
 * @param size_t size -> number of bytes in data
 *
 * @return int -> 1 on success. 0 for a NULL pointer or a hash failure
 */
// uses the first 10 SHA256 bytes and the fixed IV 01..0a
// reusing the key repeats the keystream
int cipher_trivium__crypt(const unsigned char* key, size_t key_size, unsigned char* data, size_t size)
{
    unsigned char digest[32];
    uint32_t state[10] = {0};
    uint32_t word = 0;

    if (key == NULL || data == NULL || !ciphers__sha256(key, key_size, digest))
    {
        ciphers__clear(digest, sizeof(digest));
        return 0;
    }
    for (size_t index = 0; index < 10; index++)
    {
        state[index / 4] |= (uint32_t)digest[index] << ((index % 4) * 8);
        state[3 + index / 4] |= (uint32_t)(index + 1) << ((index % 4) * 8);
    }
    state[9] = 0x7000;
    for (unsigned int round = 0; round < 36; round++)
    {
        _cipher_trivium_internal__next_word(state);
    }
    for (size_t index = 0; index < size; index++)
    {
        if (index % 4 == 0)
        {
            word = _cipher_trivium_internal__next_word(state);
        }
        data[index] ^= (unsigned char)word;
        word >>= 8;
    }
    ciphers__clear(digest, sizeof(digest));
    ciphers__clear(state, sizeof(state));
    ciphers__clear(&word, sizeof(word));
    return 1;
}
