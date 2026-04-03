#ifndef MEMORY_MANAGER_H

#define MEMORY_MANAGER_H

#include <stdlib.h>

void memorymanager__init(void);
nuint memorymanager__allocate(nuint size, nuint type);
void memorymanager__print_all_allocations();
boole memorymanager__free(nuint address);
nuint memorymanager__realloc(nuint address, nuint newsize);
void memorymanager__print_allocations_count();

#endif