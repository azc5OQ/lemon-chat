#include "definitions.h"

#include "memory_manager.h"
#include "clib/clib_memory.h"

#include "../third-party/rxi-log/log.h"

/* guards stamped into each block's header. LIVE marks a tracked block; FREED is written on free
   so an immediate double-free is caught before the memory is handed back to libc. */
#define MEMORYMANAGER_GUARD_LIVE 0xA110C0DEu
#define MEMORYMANAGER_GUARD_FREED 0xDEADBEEFu

/* bookkeeping header stored immediately before the bytes returned to the caller:
   [ header | user bytes ... ]. the caller receives (header + 1). prev/next thread every live
   block into one doubly-linked list, so allocate links and free unlinks in O(1) with no table scan.
   type is kept in 4 bytes (not uint64) so the header stays 32 bytes and the user region keeps
   16-byte alignment; the MEMALLOC_* tags are small, so 4 bytes is ample. */
typedef struct memorymanager_header_t
{
    struct memorymanager_header_t* prev;   // 0x0
    struct memorymanager_header_t* next;   // 0x8
    uint64 size;                           // 0x10
    uint type;                             // 0x18
    uint guard;                            // 0x1C
} memorymanager_header_t;

static memorymanager_header_t* g_memorymanager_head = 0;
static uint64 g_memorymanager_live_count = 0;
static pthread_mutex_t g_memorymanager_lock = PTHREAD_MUTEX_INITIALIZER;

/* static functions are defined first */

/* declarations */
static memorymanager_header_t* _memorymanager_internal__header_of(nuint address);
static void _memorymanager_internal__link(memorymanager_header_t* header);
static void _memorymanager_internal__unlink(memorymanager_header_t* header);

/**
 * @brief steps from a caller address back to its bookkeeping header
 *
 * @param nuint address -> address previously returned by memorymanager__allocate
 *
 * @return memorymanager_header_t* the header that sits immediately before the caller's bytes
 */
static memorymanager_header_t* _memorymanager_internal__header_of(nuint address)
{
    return ((memorymanager_header_t*)address) - 1;
}

/**
 * @brief links a header at the front of the live-allocation list
 *
 * @param memorymanager_header_t* header -> allocation header to link in
 *
 * @attention caller must hold g_memorymanager_lock
 *
 * @return void
 */
static void _memorymanager_internal__link(memorymanager_header_t* header)
{
    header->prev = 0;
    header->next = g_memorymanager_head;

    if (g_memorymanager_head != NULL_POINTER)
    {
        g_memorymanager_head->prev = header;
    }

    g_memorymanager_head = header;
}

/**
 * @brief unlinks a header from the live-allocation list
 *
 * @param memorymanager_header_t* header -> allocation header to unlink
 *
 * @attention caller must hold g_memorymanager_lock
 *
 * @return void
 */
static void _memorymanager_internal__unlink(memorymanager_header_t* header)
{
    if (header->prev != NULL_POINTER)
    {
        header->prev->next = header->next;
    }
    else
    {
        g_memorymanager_head = header->next;
    }

    if (header->next != NULL_POINTER)
    {
        header->next->prev = header->prev;
    }

    header->prev = 0;
    header->next = 0;
}

/**
 * @brief resets the allocator's live-allocation list to empty
 *
 * @attention the global mutex is statically initialized (PTHREAD_MUTEX_INITIALIZER), so it is ready before this runs and must not be re-initialized here
 *
 * @return void
 */
void memorymanager__init(void)
{
    g_memorymanager_head = 0;
    g_memorymanager_live_count = 0;
}

/**
 * @brief allocates size zeroed bytes and records the block in the live list for leak tracking
 *
 * @param uint64 size -> number of bytes the caller asked for
 * @param uint64 type -> caller-defined MEMALLOC_* tag identifying the allocation site
 *
 * @attention malloc and the zeroing run outside the lock; only the list and counter update are serialized
 *
 * @return nuint address of the zeroed block, or 0 on allocation failure
 */
nuint memorymanager__allocate(uint64 size, uint64 type)
{
    memorymanager_header_t* header = 0;
    nuint result = 0;

    if (size == 0)
    {
        return 0;
    }

    header = (memorymanager_header_t*)malloc(sizeof(memorymanager_header_t) + size);

    if (header == NULL_POINTER)
    {
        return 0;
    }

    header->size = size;
    header->type = (uint)type;
    header->guard = MEMORYMANAGER_GUARD_LIVE;

    result = (nuint)(header + 1);

    /* hand back zeroed memory, matching the old calloc-backed contract callers rely on */
    clib__null_memory((void*)result, size);

    pthread_mutex_lock(&g_memorymanager_lock);

    _memorymanager_internal__link(header);
    g_memorymanager_live_count++;

    pthread_mutex_unlock(&g_memorymanager_lock);

    DBG_MEMORY_MANAGER log_info("%s %p %s %llu %s %llu %s", "memorymanager__allocate address ", (void*)result, " size ", size, " type ", type, "\n");

    return result;
}

/**
 * @brief resizes a tracked allocation, keeping it linked in the live list
 *
 * @param nuint address -> address of the existing allocation
 * @param uint64 newsize -> new size in bytes
 *
 * @attention a foreign or corrupted address is rejected by the guard and returns 0
 *
 * @return nuint address of the resized block, or 0 if the address was not tracked or realloc failed
 */
nuint memorymanager__realloc(nuint address, uint64 newsize)
{
    memorymanager_header_t* old_header = 0;
    memorymanager_header_t* new_header = 0;
    memorymanager_header_t* prev = 0;
    memorymanager_header_t* next = 0;
    uint type = 0;
    nuint result = 0;

    if (address == 0)
    {
        return memorymanager__allocate(newsize, 0);
    }

    if (newsize == 0)
    {
        memorymanager__free(address);
        return 0;
    }

    old_header = _memorymanager_internal__header_of(address);

    pthread_mutex_lock(&g_memorymanager_lock);

    if (old_header->guard != MEMORYMANAGER_GUARD_LIVE)
    {
        pthread_mutex_unlock(&g_memorymanager_lock);
        DBG_MEMORY_MANAGER log_info("%s %p %s", "memorymanager__realloc bad guard on ", (void*)address, " (not a tracked block) \n");
        return 0;
    }

    prev = old_header->prev;
    next = old_header->next;
    type = old_header->type;

    _memorymanager_internal__unlink(old_header);

    new_header = (memorymanager_header_t*)realloc((void*)old_header, sizeof(memorymanager_header_t) + newsize);

    if (new_header == NULL_POINTER)
    {
        /* realloc failed, so the original block is still valid; relink it and report failure */
        old_header->prev = prev;
        old_header->next = next;

        if (prev != NULL_POINTER)
        {
            prev->next = old_header;
        }
        else
        {
            g_memorymanager_head = old_header;
        }

        if (next != NULL_POINTER)
        {
            next->prev = old_header;
        }

        pthread_mutex_unlock(&g_memorymanager_lock);

        return 0;
    }

    new_header->prev = prev;
    new_header->next = next;
    new_header->size = newsize;
    new_header->type = type;
    new_header->guard = MEMORYMANAGER_GUARD_LIVE;

    if (prev != NULL_POINTER)
    {
        prev->next = new_header;
    }
    else
    {
        g_memorymanager_head = new_header;
    }

    if (next != NULL_POINTER)
    {
        next->prev = new_header;
    }

    result = (nuint)(new_header + 1);

    pthread_mutex_unlock(&g_memorymanager_lock);

    return result;
}

/**
 * @brief frees a tracked allocation, zeroing its bytes and unlinking it from the live list
 *
 * @param nuint address -> address of the allocation to free
 *
 * @attention the guard check runs inside the lock so a concurrent double-free cannot pass it twice
 *
 * @return boole TRUE if the address was a live tracked block, FALSE otherwise
 */
boole memorymanager__free(nuint address)
{
    memorymanager_header_t* header = 0;
    uint64 size = 0;

    if (address == 0)
    {
        return FALSE;
    }

    header = _memorymanager_internal__header_of(address);

    pthread_mutex_lock(&g_memorymanager_lock);

    if (header->guard == MEMORYMANAGER_GUARD_FREED)
    {
        pthread_mutex_unlock(&g_memorymanager_lock);
        DBG_MEMORY_MANAGER log_info("%s %p %s", "memorymanager__free double-free on ", (void*)address, " \n");
        return FALSE;
    }

    if (header->guard != MEMORYMANAGER_GUARD_LIVE)
    {
        pthread_mutex_unlock(&g_memorymanager_lock);
        DBG_MEMORY_MANAGER log_info("%s %p %s", "memorymanager__free bad guard on ", (void*)address, " (not a tracked block) \n");
        return FALSE;
    }

    size = header->size;

    _memorymanager_internal__unlink(header);
    header->guard = MEMORYMANAGER_GUARD_FREED;
    g_memorymanager_live_count--;

    pthread_mutex_unlock(&g_memorymanager_lock);

    DBG_MEMORY_MANAGER log_info("%s %p %s %llu %s", "memorymanager__free address ", (void*)address, " size ", size, "\n");

    /* zero the caller's bytes before returning the block to libc, matching the old contract */
    clib__null_memory((void*)address, size);
    free((void*)header);

    return TRUE;
}

/**
 * @brief prints every live tracked allocation (address, size, type) for leak debugging
 *
 * @return void
 */
void memorymanager__print_all_allocations(void)
{
    memorymanager_header_t* header = 0;

    pthread_mutex_lock(&g_memorymanager_lock);

    for (header = g_memorymanager_head; header != NULL_POINTER; header = header->next)
    {
        log_info("%s %p %s %llu %s %u %s", "address ", (void*)(header + 1), " size ", header->size, " type ", header->type, "\n");
    }

    pthread_mutex_unlock(&g_memorymanager_lock);
}

/**
 * @brief prints how many allocations are currently live
 *
 * @return void
 */
void memorymanager__print_allocations_count(void)
{
    log_info("%s %llu %s", "memorymanager__print_allocations_count ", g_memorymanager_live_count, "\n");
}
