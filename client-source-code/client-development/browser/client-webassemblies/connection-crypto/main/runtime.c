// no filesystem, clock, randomness or network imports
#include <stddef.h>

/**
 * @brief clears sensitive bytes for the Mbed TLS runtime
 *
 * @param void* data -> buffer to clear
 * @param size_t size -> number of bytes in the buffer
 *
 * @return void
 */
void mbedtls_platform_zeroize(void *data, size_t size)
{
    volatile unsigned char *bytes = data;
    while (size > 0)
    {
        *bytes++ = 0;
        size--;
    }
}
