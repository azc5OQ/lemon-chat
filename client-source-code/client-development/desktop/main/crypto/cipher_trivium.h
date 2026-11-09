#ifndef LEMON_CIPHER_TRIVIUM_H
#define LEMON_CIPHER_TRIVIUM_H

#include "../definitions.h"

#include <stddef.h>

boole cipher_trivium__crypt(const ubyte *key, uint64 key_size, ubyte *data, uint64 size);

#endif
