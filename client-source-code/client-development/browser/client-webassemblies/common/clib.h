#ifndef LEMON_WASM_CLIB_H
#define LEMON_WASM_CLIB_H

#include <stddef.h>
#include <string.h>

/**
 * @brief copies bytes within the destination capacity, allowing overlap
 *
 * @param const void* source -> bytes to copy
 * @param void* destination -> buffer receiving the bytes
 * @param size_t length -> requested byte count
 * @param size_t capacity -> available bytes in destination
 *
 * @return void
 */
static inline void clib__copy_memory(const void* source, void* destination, size_t length, size_t capacity)
{
    if (source == NULL || destination == NULL)
    {
        return;
    }
    if (length > capacity)
    {
        length = capacity;
    }
    memmove(destination, source, length);
}

#endif
