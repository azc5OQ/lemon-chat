#include "cipher_twofish.h"
#include "../clib/clib.h"
#include "ciphers.h"
#include <string.h>
#include "../../third-party/libtomcrypt/lemon_config.h"
#include "../../third-party/libtomcrypt/src/ciphers/twofish/twofish.c"

/**
 * @brief encrypts or decrypts data in place with Twofish CTR
 *
 * @param const ubyte[32] key -> 256 bit key. Must not be NULL
 * @param const ubyte[16] iv -> initial counter. Must not be NULL
 * @param ubyte* data -> buffer to update. Must not be NULL
 * @param uint64 size -> number of bytes in data
 *
 * @return boole -> 1 on success. 0 on failure
 */
boole cipher_twofish__crypt(const ubyte key[32], const ubyte iv[16], ubyte *data, uint64 size)
{
    int64 position = 0;
    symmetric_key scheduled = { 0 };
    ubyte counter[16] = { 0 };
    ubyte stream[16] = { 0 };
    uint64 offset = 0;
    uint64 index = 0;
    boole status = FALSE;

    // prepare counter and key
    clib__copy_memory(iv, counter, sizeof(counter), sizeof(counter));
    status = twofish_setup(key, 32, 0, &scheduled) == CRYPT_OK;

    // apply the cipher
    for (index = 0; status && index < size; index++)
    {
        // create next keystream block
        if (offset == 0)
        {
            position = 15;
            status = twofish_ecb_encrypt(counter, stream, &scheduled) == CRYPT_OK;
            if (!status)
            {
                break;
            }

            // increase the counter
            while (position >= 0)
            {
                counter[position]++;
                if (counter[position] != 0)
                {
                    break;
                }
                position--;
            }
        }

        // XOR data with keystream
        data[index] ^= stream[offset];
        offset = (offset + 1) % 16;
    }

    // clear the cipher state
    ciphers__clear(&scheduled, sizeof(scheduled));
    ciphers__clear(counter, sizeof(counter));
    ciphers__clear(stream, sizeof(stream));
    return status;
}
