#ifndef CLIB_MEMORY_H

#define CLIB_MEMORY_H

#include "../mytypedef.h"

void clib__set_memory(void *source, long long length, unsigned char value);
void clib__copy_memory(void *source, void *destination, nuint length, nuint max_allowed_length);
void clib__null_memory(void *source, nuint length);
void clib__print_block_of_memory(void *base, long long length);

#endif
