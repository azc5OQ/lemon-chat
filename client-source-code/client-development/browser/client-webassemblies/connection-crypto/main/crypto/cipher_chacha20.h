#ifndef LEMON_CIPHER_CHACHA20_H
#define LEMON_CIPHER_CHACHA20_H

#include <stddef.h>

int cipher_chacha20__crypt(const unsigned char key[32], const unsigned char iv[16], unsigned char* data, size_t size);

#endif
