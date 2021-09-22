#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "clib_memory.h"


void clib__set_memory(void* source, long long length, unsigned char value)
{
    if(source == 0)
    {
        return;
    }
    unsigned char* src = (unsigned char*)source;


    while (length--)
    {
        *src++ = (unsigned char)value;
    }
}


void clib__null_memory(void* source, long long length)
{
    if(source == 0)
    {
        return;
    }
    char* src = source;

    while (length--)
    {
        *src++ = (char)0;
    }
}

void clib__copy_memory(void* source, void* destination, long long length)
{
    if(source == 0 || destination == 0)
    {
        return;
    }
    char* src = (char*)source;

    char* dest = (char*)destination;

    while (length--)
    {
        *dest++ = *src++;
    }
}


void clib_print_block_of_memory(void* base,long long length)
{
  unsigned long base_address = (unsigned long)base;

  for(int x = 0; x < length; x++)
  {

    base_address += 1;
    unsigned char value = *(((unsigned char*)base) + x);

    printf("%ld%s%d%s",base_address," -> ", value, "\n");
  }
}
