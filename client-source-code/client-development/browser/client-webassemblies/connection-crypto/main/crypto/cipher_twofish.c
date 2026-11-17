#include "cipher_twofish.h"
#include "../../../common/clib.h"
#include "ciphers.h"
#include <string.h>
#include "../../third-party/libtomcrypt/lemon_config.h"
#include "../../third-party/libtomcrypt/src/ciphers/twofish/twofish.c"

/**
 * @brief encrypts or decrypts data in place with Twofish CTR
 *
 * @param const unsigned char[32] key -> 256 bit key. Must not be NULL
 * @param const unsigned char[16] iv -> initial counter. Must not be NULL
 * @param unsigned char* data -> buffer to update. Must not be NULL
 * @param size_t size -> number of bytes in data
 *
 * @return int -> 1 on success. 0 on failure
 */
int cipher_twofish__crypt(const unsigned char key[32], const unsigned char iv[16], unsigned char* data, size_t size)
{
    symmetric_key scheduled;
    unsigned char counter[16];
    unsigned char stream[16] = {0};
    size_t offset = 0;
    size_t index;
    int status;

    clib__copy_memory(iv, counter, sizeof(counter), sizeof(counter));
    status = twofish_setup(key, 32, 0, &scheduled) == CRYPT_OK;
    for (index = 0; status && index < size; index++)
    {
        if (offset == 0)
        {
            int position = 15;
            status = twofish_ecb_encrypt(counter, stream, &scheduled) == CRYPT_OK;
            if (!status)
            {
                break;
            }
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
        data[index] ^= stream[offset];
        offset = (offset + 1) % 16;
    }
    ciphers__clear(&scheduled, sizeof(scheduled));
    ciphers__clear(counter, sizeof(counter));
    ciphers__clear(stream, sizeof(stream));
    return status;
}
