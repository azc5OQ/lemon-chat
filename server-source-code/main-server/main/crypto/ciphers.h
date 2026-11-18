#ifndef LEMON_CIPHERS_H
#define LEMON_CIPHERS_H

#include <stddef.h>
#include "../definitions.h"

const char *ciphers__name(int64 type);
int64 ciphers__type(const char *name);
boole ciphers__crypt(int64 type, const ubyte key[32], const ubyte iv[16], ubyte *data, uint64 size);
boole ciphers__sha256(const ubyte *data, uint64 size, ubyte output[32]);
boole ciphers__hmac(const ubyte key[32], const ubyte *data, uint64 size, ubyte output[32]);
void ciphers__clear(void *data, uint64 size);

#endif
