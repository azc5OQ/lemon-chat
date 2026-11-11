#include "cipher_twofish.h"
#include "ciphers.h"
#include <string.h>
#include "../../third-party/libtomcrypt/lemon_config.h"
#include "../../third-party/libtomcrypt/src/ciphers/twofish/twofish.c"

// Twofish-256-CTR, in place. Returns 1 on success, 0 on failure.
// The key, IV and data must be non-NULL; encryption and decryption are identical.
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
    memcpy(counter, iv, sizeof(counter));
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
