#include "cipher_trivium.h"
#include "ciphers.h"

// Trivium's three shift registers, packed into 3 + 3 + 4 words.
// The uint words must stay 32-bit: shifts discard bits beyond each word.
// One update advances all registers by 32 bits. The unused high bits in the
// last word of each register are never read by a feedback tap.
static uint _cipher_trivium_internal__next_word(uint state[10])
{
    uint first = ((state[2] << 30) | (state[1] >> 2)) ^ ((state[2] << 3) | (state[1] >> 29));
    uint second = ((state[5] << 27) | (state[4] >> 5)) ^ ((state[5] << 12) | (state[4] >> 20));
    uint third = ((state[8] << 30) | (state[7] >> 2)) ^ ((state[9] << 17) | (state[8] >> 15));
    uint output = first ^ second ^ third;

    first ^= (((state[2] << 5) | (state[1] >> 27)) & ((state[2] << 4) | (state[1] >> 28))) ^ ((state[5] << 18) | (state[4] >> 14));
    second ^= (((state[5] << 14) | (state[4] >> 18)) & ((state[5] << 13) | (state[4] >> 19))) ^ ((state[8] << 9) | (state[7] >> 23));
    third ^= (((state[9] << 19) | (state[8] >> 13)) & ((state[9] << 18) | (state[8] >> 14))) ^ ((state[2] << 27) | (state[1] >> 5));

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

// Exact Trivium-80 variant: SHA-256(key)[0..9], fixed IV 01..0a and its word byte order.
// No message IV is used. Repeating a key repeats the keystream. Returns 1 on success.
boole cipher_trivium__crypt(const ubyte *key, uint64 key_size, ubyte *data, uint64 size)
{
    uint64 setup_index = 0;
    uint64 round = 0;
    uint64 index = 0;
    ubyte digest[32] = { 0 };
    uint state[10] = { 0 };
    uint word = 0;

    if (key == NULL || data == NULL || !ciphers__sha256(key, key_size, digest))
    {
        ciphers__clear(digest, sizeof(digest));
        return 0;
    }
    for (setup_index = 0; setup_index < 10; setup_index++)
    {
        state[setup_index / 4] |= (uint)digest[setup_index] << ((setup_index % 4) * 8);
        state[3 + setup_index / 4] |= (uint)(setup_index + 1) << ((setup_index % 4) * 8);
    }
    state[9] = 0x7000;
    for (round = 0; round < 36; round++)
    {
        _cipher_trivium_internal__next_word(state);
    }
    for (index = 0; index < size; index++)
    {
        if (index % 4 == 0)
        {
            word = _cipher_trivium_internal__next_word(state);
        }
        data[index] ^= (ubyte)word;
        word >>= 8;
    }
    ciphers__clear(digest, sizeof(digest));
    ciphers__clear(state, sizeof(state));
    ciphers__clear(&word, sizeof(word));
    return 1;
}
