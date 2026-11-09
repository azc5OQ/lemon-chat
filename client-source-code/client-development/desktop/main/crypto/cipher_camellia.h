#ifndef LEMON_CIPHER_CAMELLIA_H
#define LEMON_CIPHER_CAMELLIA_H

#include "../definitions.h"

#include <stddef.h>

boole cipher_camellia__crypt(const ubyte key[32], const ubyte iv[16], ubyte *data, uint64 size);

#endif
