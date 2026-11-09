#include "platform/memory.h"

/**
 * @brief clears an owned buffer through volatile stores so release-time wiping survives optimization.
 * @param void* buffer -> writable allocation or stack buffer; NULL is accepted
 * @param uint64 capacity -> full known byte capacity, never an inferred string length
 * @return void
 */
void desktop_memory__erase(void *buffer, uint64 capacity)
{
    volatile ubyte *bytes = buffer;
    uint64 index = 0;
    if (bytes)
    {
        for (index = 0; index < capacity; index++)
        {
            bytes[index] = 0;
        }
    }
}
