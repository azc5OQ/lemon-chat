#ifndef MEMORY_MANAGER_H
#define MEMORY_MANAGER_H

void   memorymanager__init(void);

nuint  memorymanager__allocate(uint64 size, uint64 type);
nuint  memorymanager__realloc(nuint address, uint64 newsize);
boole  memorymanager__free(nuint address);

void   memorymanager__print_all_allocations(void);
void   memorymanager__print_allocations_count(void);

#endif
