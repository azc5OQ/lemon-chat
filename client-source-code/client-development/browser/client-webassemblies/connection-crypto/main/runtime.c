// No filesystem, clock, randomness or network imports.
#include <stddef.h>

void mbedtls_platform_zeroize(void* data, size_t size)
{
    volatile unsigned char* bytes = data;
    while (size > 0)
    {
        *bytes++ = 0;
        size--;
    }
}
