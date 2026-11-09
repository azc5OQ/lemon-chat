#ifndef LEMON_CIPHER_TRIVIUM_H
#define LEMON_CIPHER_TRIVIUM_H

#include <stddef.h>

int cipher_trivium__crypt(const unsigned char* key, size_t key_size, unsigned char* data, size_t size);

#endif
