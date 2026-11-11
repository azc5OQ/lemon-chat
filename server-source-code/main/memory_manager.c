#include "definitions.h"

#include "memory_manager.h"
#include "clib/clib.h"

#include "../third-party/rxi-log/log.h"

#ifndef LEMON_TRACK_ALLOCATIONS
#ifdef NDEBUG
#define LEMON_TRACK_ALLOCATIONS 0
#else
#define LEMON_TRACK_ALLOCATIONS 1
#endif
#endif

// Each allocation records its size and a value used to detect damaged headers.
#define MEMORYMANAGER_GUARD_LIVE 0xA110C0DEu
#define MEMORYMANAGER_GUARD_FREED 0xDEADBEEFu

// The size lets free clear the whole allocation. Optional list pointers let
// debug builds print allocations that have not been freed. Both layouts keep
// the returned address aligned to 16 bytes on 64-bit systems.
typedef struct memorymanager_header_t
{
#if LEMON_TRACK_ALLOCATIONS
    struct memorymanager_header_t *prev;
    struct memorymanager_header_t *next;
#endif
    uint64 size;
    // Fixed header fields keep payloads aligned to 16 bytes.
    uint type;
    uint guard;
} memorymanager_header_t;

#if LEMON_TRACK_ALLOCATIONS
static memorymanager_header_t *g_memorymanager_head = NULL_POINTER;
static uint64 g_memorymanager_live_count = 0;
static pthread_mutex_t g_memorymanager_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static atomic_uint_fast64_t g_upload_bytes;
static atomic_uint_fast64_t g_music_bytes;

static atomic_uint_fast64_t *_budget(uint64 type, uint64 *limit)
{
    // choose the upload limit
    if (type == MEMALLOC_FILE_UPLOAD_BY_PARTS)
    {
        *limit = 256ULL * 1024 * 1024;
        return &g_upload_bytes;
    }

    // choose the song limit
    if (type == MEMALLOC_MUSICBOT_SONG)
    {
        *limit = 512ULL * 1024 * 1024;
        return &g_music_bytes;
    }

    // other types have none
    return NULL_POINTER;
}

static boole _reserve(uint64 size, uint64 type)
{
    uint64 limit = 0;
    atomic_uint_fast64_t *counter = _budget(type, &limit);
    uint_fast64_t used = 0;

    // no limit applies
    if (counter == NULL_POINTER)
    {
        return TRUE;
    }

    // add size within limit
    used = atomic_load(counter);
    do
    {
        if (size > limit || used > limit - size)
        {
            return FALSE;
        }
    } while (!atomic_compare_exchange_weak(counter, &used, used + size));
    return TRUE;
}

static void _release(uint64 size, uint64 type)
{
    uint64 limit = 0;
    atomic_uint_fast64_t *counter = _budget(type, &limit);

    // subtract the size
    if (counter != NULL_POINTER)
    {
        atomic_fetch_sub(counter, size);
    }
}

// static functions are defined first

// declarations
static memorymanager_header_t *_memorymanager_internal__header_of(nuint address);
#if LEMON_TRACK_ALLOCATIONS
static void _memorymanager_internal__link(memorymanager_header_t *header);
static void _memorymanager_internal__unlink(memorymanager_header_t *header);
#endif

/**
 * @brief steps from a caller address back to its bookkeeping header
 *
 * @param nuint address -> address previously returned by memorymanager__allocate
 *
 * @return memorymanager_header_t* the header that sits immediately before the caller's bytes
 */
static memorymanager_header_t *_memorymanager_internal__header_of(nuint address)
{
    return ((memorymanager_header_t *)address) - 1;
}

#if LEMON_TRACK_ALLOCATIONS
/**
 * @brief links a header at the front of the live-allocation list
 *
 * @param memorymanager_header_t* header -> allocation header to link in
 *
 * @attention caller must hold g_memorymanager_lock
 *
 * @return void
 */
static void _memorymanager_internal__link(memorymanager_header_t *header)
{
    // set the header links
    header->prev = 0;
    header->next = g_memorymanager_head;

    // link the old head
    if (g_memorymanager_head != NULL_POINTER)
    {
        g_memorymanager_head->prev = header;
    }

    // set the new head
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
static void _memorymanager_internal__unlink(memorymanager_header_t *header)
{
    // connect previous to next
    if (header->prev != NULL_POINTER)
    {
        header->prev->next = header->next;
    }
    else
    {
        g_memorymanager_head = header->next;
    }

    // connect next to previous
    if (header->next != NULL_POINTER)
    {
        header->next->prev = header->prev;
    }

    // clear the header links
    header->prev = 0;
    header->next = 0;
}
#endif

/**
 * @brief resets the allocator's live-allocation list to empty
 *
 * @attention the global mutex is statically initialized (PTHREAD_MUTEX_INITIALIZER), so it is ready before this runs and must not be re-initialized here
 *
 * @return void
 */
void memorymanager__init(void)
{
#if LEMON_TRACK_ALLOCATIONS
    g_memorymanager_head = 0;
    g_memorymanager_live_count = 0;
#endif
}

/**
 * @brief Allocates zeroed bytes and records the block if allocation tracking is enabled.
 *
 * @param uint64 size -> number of bytes the caller asked for
 * @param uint64 type -> caller-defined MEMALLOC_* tag identifying the allocation site
 *
 * @attention Only builds with allocation tracking take the shared list lock.
 *
 * @return nuint address of the zeroed block, or 0 on allocation failure
 */
nuint memorymanager__allocate(uint64 size, uint64 type)
{
    memorymanager_header_t *header = NULL_POINTER;
    nuint result = 0;

    // refuse wrong size
    if (size == 0 || size > (uint64)INT64_MAX || size > SIZE_MAX - sizeof(memorymanager_header_t))
    {
        return 0;
    }

    // check the type limit
    if (!_reserve(size, type))
    {
        return 0;
    }

    // allocate the memory
    header = (memorymanager_header_t *)calloc(1, sizeof(memorymanager_header_t) + (uint64)size);

    if (header == NULL_POINTER)
    {
        _release(size, type);
        return 0;
    }

    // fill the header
    header->size = size;
    header->type = (uint)type;
    header->guard = MEMORYMANAGER_GUARD_LIVE;

    // set the returned address
    result = (nuint)(header + 1);

    // calloc has already zeroed the user region

#if LEMON_TRACK_ALLOCATIONS
    pthread_mutex_lock(&g_memorymanager_lock);

    // add to the list
    _memorymanager_internal__link(header);
    g_memorymanager_live_count++;

    pthread_mutex_unlock(&g_memorymanager_lock);
#endif

    // log the allocation
    DBG_MEMORY_MANAGER log_info("%s %p %s %llu %s %llu %s", "memorymanager__allocate address ", (void *)result, " size ", size, " type ", type, "\n");

    return result;
}

/**
 * @brief Resizes an allocation, keeping the old block valid if resizing fails.
 *
 * @param nuint address -> address of the existing allocation
 * @param uint64 newsize -> new size in bytes
 *
 * @attention The caller must own a live allocation; nobody else may free or resize it.
 *
 * @return nuint address of the resized block, or 0 if the address was not tracked or realloc failed
 */
nuint memorymanager__realloc(nuint address, uint64 newsize)
{
    memorymanager_header_t *header = NULL_POINTER;
    uint64 old_size = 0;
    uint64 copy_size = 0;
    uint64 type = 0;
    nuint replacement = 0;

    // allocate a new block
    if (address == 0)
    {
        return memorymanager__allocate(newsize, 0);
    }

    // free for size zero
    if (newsize == 0)
    {
        memorymanager__free(address);
        return 0;
    }

    // refuse wrong size
    if (newsize > (uint64)INT64_MAX || newsize > SIZE_MAX - sizeof(memorymanager_header_t))
    {
        return 0;
    }

    // check the block header
    header = _memorymanager_internal__header_of(address);
#if LEMON_TRACK_ALLOCATIONS
    pthread_mutex_lock(&g_memorymanager_lock);
#endif
    if (header->guard != MEMORYMANAGER_GUARD_LIVE)
    {
#if LEMON_TRACK_ALLOCATIONS
        pthread_mutex_unlock(&g_memorymanager_lock);
#endif
        return 0;
    }

    // read size and type
    old_size = header->size;
    type = header->type;
#if LEMON_TRACK_ALLOCATIONS
    pthread_mutex_unlock(&g_memorymanager_lock);
#endif
    // keep the old allocation intact on failure, zero any growth, and wipe all
    // old bytes before releasing them even when the block becomes smaller
    replacement = memorymanager__allocate(newsize, type);
    if (replacement == 0)
    {
        return 0;
    }

    // copy the old bytes
    copy_size = old_size < newsize ? old_size : newsize;
    clib__memcpy((void *)replacement, (void *)address, (int64)copy_size);

    // free the old block
    memorymanager__free(address);
    return replacement;
}

/**
 * @brief Clears and frees an allocation, removing it from the optional tracking list.
 *
 * @param nuint address -> address of the allocation to free
 *
 * @attention The caller must own a live allocation; freeing the same address twice is invalid.
 *
 * @return boole TRUE if the address was a live tracked block, FALSE otherwise
 */
boole memorymanager__free(nuint address)
{
    memorymanager_header_t *header = NULL_POINTER;
    uint64 size = 0;

    // nothing to free
    if (address == 0)
    {
        return FALSE;
    }

    // find the header
    header = _memorymanager_internal__header_of(address);

#if LEMON_TRACK_ALLOCATIONS
    // lock the list
    pthread_mutex_lock(&g_memorymanager_lock);
#endif

    // refuse a second free
    if (header->guard == MEMORYMANAGER_GUARD_FREED)
    {
#if LEMON_TRACK_ALLOCATIONS
        pthread_mutex_unlock(&g_memorymanager_lock);
#endif
        DBG_MEMORY_MANAGER log_info("%s %p %s", "memorymanager__free double-free on ", (void *)address, " \n");
        return FALSE;
    }

    // refuse an unknown block
    if (header->guard != MEMORYMANAGER_GUARD_LIVE)
    {
#if LEMON_TRACK_ALLOCATIONS
        pthread_mutex_unlock(&g_memorymanager_lock);
#endif
        DBG_MEMORY_MANAGER log_info("%s %p %s", "memorymanager__free bad guard on ", (void *)address, " (not a tracked block) \n");
        return FALSE;
    }

    // read the size
    size = header->size;

    // mark the block freed
    header->guard = MEMORYMANAGER_GUARD_FREED;
#if LEMON_TRACK_ALLOCATIONS
    _memorymanager_internal__unlink(header);
    g_memorymanager_live_count--;

    pthread_mutex_unlock(&g_memorymanager_lock);
#endif

    // log the freed block
    DBG_MEMORY_MANAGER log_info("%s %p %s %llu %s", "memorymanager__free address ", (void *)address, " size ", size, "\n");

    // zero the caller's bytes before returning the block to libc, matching the old contract
    clib__null_memory((void *)address, size);
    _release(size, header->type);
    free((void *)header);

    return TRUE;
}

/**
 * @brief prints every live tracked allocation (address, size, type) for leak debugging
 *
 * @return void
 */
void memorymanager__print_all_allocations(void)
{
#if LEMON_TRACK_ALLOCATIONS
    memorymanager_header_t *header = NULL_POINTER;

    // lock the list
    pthread_mutex_lock(&g_memorymanager_lock);

    // print every allocation
    for (header = g_memorymanager_head; header != NULL_POINTER; header = header->next)
    {
        log_info("%s %p %s %llu %s %u %s", "address ", (void *)(header + 1), " size ", header->size, " type ", header->type, "\n");
    }

    // unlock the list
    pthread_mutex_unlock(&g_memorymanager_lock);
#endif
}

/**
 * @brief prints how many allocations are currently live
 *
 * @return void
 */
void memorymanager__print_allocations_count(void)
{
#if LEMON_TRACK_ALLOCATIONS
    // print the count
    pthread_mutex_lock(&g_memorymanager_lock);
    log_info("%s %llu %s", "memorymanager__print_allocations_count ", g_memorymanager_live_count, "\n");
    pthread_mutex_unlock(&g_memorymanager_lock);
#endif
}
