#ifndef LEMON_CIPHER_SERPENT_H
#define LEMON_CIPHER_SERPENT_H

#include "../definitions.h"

#include <stddef.h>

boole cipher_serpent__crypt(const ubyte key[32], const ubyte iv[16], ubyte *data, uint64 size);

#endif
