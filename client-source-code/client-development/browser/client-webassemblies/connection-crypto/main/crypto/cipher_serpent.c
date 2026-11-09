#include "cipher_serpent.h"
#include "ciphers.h"
#include <string.h>
#include "../../third-party/libtomcrypt/lemon_config.h"
#include "../../third-party/libtomcrypt/src/ciphers/serpent.c"

// Serpent-256-CTR, in place. Returns 1 on success, 0 on failure.
// The key, IV and data must be non-NULL; encryption and decryption are identical.
int cipher_serpent__crypt(const unsigned char key[32], const unsigned char iv[16], unsigned char* data, size_t size)
{
    symmetric_key scheduled;
    unsigned char counter[16];
    unsigned char stream[16] = {0};
    size_t offset = 0;
    size_t index;
    int status;

    memcpy(counter, iv, sizeof(counter));
    status = serpent_setup(key, 32, 0, &scheduled) == CRYPT_OK;
    for (index = 0; status && index < size; index++)
    {
        if (offset == 0)
        {
            int position = 15;
            status = serpent_ecb_encrypt(counter, stream, &scheduled) == CRYPT_OK;
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
