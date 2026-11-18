#ifndef CRYPTO_STORAGE_H
#define CRYPTO_STORAGE_H

#include "../definitions.h"

boole crypto_storage__seal(const crypto_key_t *keys, uint64 count, const ubyte *data, uint64 size, ubyte **output, uint64 *output_size);
boole crypto_storage__open(const crypto_key_t *keys, uint64 count, ubyte *file, uint64 file_size, ubyte **data, uint64 *size);

#endif
