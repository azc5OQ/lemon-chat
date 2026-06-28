#include "../definitions.h"
#include "clib_memory.h"

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
