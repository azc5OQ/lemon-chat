#include "cipher_trivium.h"
#include "ciphers.h"

/**
 * @brief advances the Trivium registers by 32 bits
 *
 * @param uint[10] state -> registers packed into 3, 3 and 4 words
 *
 * @return uint -> the next 32 bits of keystream
 */
// state words must remain 32 bits. Unused high bits are not read
/**
 * @brief advances the Trivium registers by one output word
 *
 * @param uint [10] state -> ten words holding the mutable cipher state
 *
 * @return uint -> next 32 keystream bits
 */
static uint _cipher_trivium_internal__next_word(uint state[10])
{
    uint first = ((state[2] << 30) | (state[1] >> 2)) ^ ((state[2] << 3) | (state[1] >> 29));
    uint second = ((state[5] << 27) | (state[4] >> 5)) ^ ((state[5] << 12) | (state[4] >> 20));
    uint third = ((state[8] << 30) | (state[7] >> 2)) ^ ((state[9] << 17) | (state[8] >> 15));
    uint output = first ^ second ^ third;

    // calculate the feedback words
    first ^= (((state[2] << 5) | (state[1] >> 27)) & ((state[2] << 4) | (state[1] >> 28))) ^ ((state[5] << 18) | (state[4] >> 14));
    second ^= (((state[5] << 14) | (state[4] >> 18)) & ((state[5] << 13) | (state[4] >> 19))) ^ ((state[8] << 9) | (state[7] >> 23));
    third ^= (((state[9] << 19) | (state[8] >> 13)) & ((state[9] << 18) | (state[8] >> 14))) ^ ((state[2] << 27) | (state[1] >> 5));

    // shift the registers
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
 * @param const ubyte* key -> input bytes hashed to derive the cipher key
 * @param uint64 key_size -> number of bytes in key
 * @param ubyte* data -> buffer to update
 * @param uint64 size -> number of bytes in data
 *
 * @return boole -> 1 on success. 0 for a NULL pointer or a hash failure
 */
// uses the first 10 SHA256 bytes and the fixed IV 01..0a
// reusing the key repeats the keystream
/**
 * @brief applies the Trivium keystream derived from the supplied key
 *
 * @param const ubyte * key -> source key bytes
 * @param uint64 key_size -> source key byte count
 * @param ubyte * data -> buffer to transform in place
 * @param uint64 size -> buffer byte count
 *
 * @return boole -> TRUE when the key digest and transformation succeed
 */
boole cipher_trivium__crypt(const ubyte *key, uint64 key_size, ubyte *data, uint64 size)
{
    uint64 setup_index = 0;
    uint64 round = 0;
    uint64 index = 0;
    ubyte digest[32] = { 0 };
    uint state[10] = { 0 };
    uint word = 0;

    // hash the key
    if (key == NULL || data == NULL || !ciphers__sha256(key, key_size, digest))
    {
        ciphers__clear(digest, sizeof(digest));
        return 0;
    }

    // set the start state
    for (setup_index = 0; setup_index < 10; setup_index++)
    {
        state[setup_index / 4] |= (uint)digest[setup_index] << ((setup_index % 4) * 8);
        state[3 + setup_index / 4] |= (uint)(setup_index + 1) << ((setup_index % 4) * 8);
    }
    state[9] = 0x7000;

    // run the setup rounds
    for (round = 0; round < 36; round++)
    {
        _cipher_trivium_internal__next_word(state);
    }

    // XOR data with keystream
    for (index = 0; index < size; index++)
    {
        if (index % 4 == 0)
        {
            word = _cipher_trivium_internal__next_word(state);
        }
        data[index] ^= (ubyte)word;
        word >>= 8;
    }

    // clear the cipher state
    ciphers__clear(digest, sizeof(digest));
    ciphers__clear(state, sizeof(state));
    ciphers__clear(&word, sizeof(word));
    return 1;
}
