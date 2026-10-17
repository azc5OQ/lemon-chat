#include "../definitions.h"
#include "clib_memory.h"

/**
 * @brief Fills a memory block with the given byte value.
 * @param void* source -> block to fill; NULL is ignored
 * @param long long length -> number of bytes to fill; must not be negative
 * @param unsigned char value -> value written to each byte
 * @return void
 */
void clib__set_memory(void* source, long long length, unsigned char value)
{
    unsigned char* src = NULL_POINTER;
    if (source == 0)
    {
        return;
    }
    src = (unsigned char*)source;

    while (length--)
    {
        *src++ = (unsigned char)value;
    }
}

/**
 * @brief Fills a memory block with zero bytes.
 * @param void* source -> block to clear; NULL is ignored
 * @param uint64 length -> number of bytes to clear
 * @return void
 */
void clib__null_memory(void* source, uint64 length)
{
    char* src = NULL_POINTER;
    if (source == 0)
    {
        return;
    }
    src = source;

    while (length--)
    {
        *src++ = (char)0;
    }
}

/**
 * @brief Copies bytes into a destination buffer, stopping at its given capacity.
 * @param void* source -> bytes to copy; NULL is ignored
 * @param void* out_destination -> buffer to fill; NULL is ignored
 * @param uint64 length -> number of source bytes to copy
 * @param uint64 max_allowed_length -> destination capacity; must be positive when copying bytes
 * @return void
 */
void clib__copy_memory(void* source, void* out_destination, uint64 length, uint64 max_allowed_length)
{
    long long already_copied_bytes = 0;
    char* src = NULL_POINTER;
    char* dest = NULL_POINTER;
    if (source == 0 || out_destination == 0)
    {
        return;
    }
    src = (char*)source;

    dest = (char*)out_destination;

    while (length--)
    {
        *dest++ = *src++;
        already_copied_bytes++;
        if (already_copied_bytes >= max_allowed_length)
        {
            break;
        }
    }
}

/**
 * @brief Prints the byte values in a memory block to the console.
 * @param void* base -> start of the memory block
 * @param long long length -> number of bytes to print; must not be negative
 * @return void
 */
void clib_print_block_of_memory(void* base, long long length)
{
    unsigned long long base_address = (unsigned long long)base;

    for (uint64 x = 0; x < length; x++)
    {
        unsigned char value = 0;
        base_address += 1;
        value = *(((unsigned char*)base) + x);

        printf("%lld %s %d %s", base_address, " -> ", value, "\n");
    }
}
