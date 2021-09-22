#ifndef CLIB_MEMORY_H

#define CLIB_MEMORY_H

void clib__set_memory(void* source, long long length, unsigned char value);
void clib__copy_memory(void* source, void* destination, long long length);
void clib__null_memory(void* source, long long length);
void clib__print_block_of_memory(void* base,long long length);

#endif
