#ifndef LEMON_CIPHER_AES_H
#define LEMON_CIPHER_AES_H

#include <stddef.h>

int cipher_aes__crypt(const unsigned char key[32], const unsigned char iv[16], unsigned char* data, size_t size);

#endif
