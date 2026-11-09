#ifndef LEMON_CIPHERS_H
#define LEMON_CIPHERS_H

#include <stddef.h>
#include <stdint.h>

enum
{
    LEMON_AES256 = 1,
    LEMON_CHACHA20 = 3,
    LEMON_CAMELLIA256 = 4,
    LEMON_TWOFISH256 = 5,
    LEMON_SERPENT256 = 6,
    LEMON_TRIVIUM80 = 7
};

const char* ciphers__name(int type);
int ciphers__type(const char* name);
int ciphers__crypt(int type, const unsigned char key[32], const unsigned char iv[16], unsigned char* data, size_t size);
int ciphers__sha256(const unsigned char* data, size_t size, unsigned char output[32]);
int ciphers__hmac(const unsigned char key[32], const unsigned char* data, size_t size, unsigned char output[32]);
void ciphers__clear(void* data, size_t size);

#endif
