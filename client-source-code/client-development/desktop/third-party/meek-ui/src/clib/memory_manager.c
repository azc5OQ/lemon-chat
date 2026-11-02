// clib/memory_manager.c - the library's allocations. memory_manager.h has
// the modes, the configuration and the reporting rules; this file depends
// on that header and on system headers only.
//
// ONE STANDALONE BLOCK
//   [ header | user bytes ... ]
//   ^        ^
//   from the heap    returned to the caller
// alloc asks the heap for sizeof(header) + size, fills the header, links
// it into a doubly-linked list and returns (header + 1). free steps back
// to the header, unlinks it and releases the whole allocation. Both are
// O(1). The guard reports header corruption in a live block; as with libc
// free, foreign pointers, double frees and concurrent access to the same
// allocation are undefined behaviour, the guard does not make them safe.
//
// THE STANDALONE HEAP
//   Windows: the functions in _win32 (memory_manager_win32_t), a table
//   filled by memory_manager__init_win32 with what the program resolved, or,
//   in a build with a C runtime, by init itself with the linked kernel32
//   functions and the process heap. Nothing here calls HeapAlloc or the
//   critical-section functions by name; MEMORY_MANAGER__NO_COMMON_RUNTIME
//   only removes the default fill. Other platforms: malloc, realloc, free
//   and a pthread mutex; MEMORY_MANAGER__NO_COMMON_RUNTIME removes the heap.
//
// THE MODE
//   Decided once, at init or at the first allocation, and held until
//   shutdown, so no block is allocated in one mode and released in the
//   other. The four functions passed to memory_manager__delegate_to are
//   copied at that moment; a delegate_to during the run does not move the
//   live blocks.
//
// THREAD SAFETY (standalone)
//   One mutex protects the list head and the statistics; it exists between
//   init and shutdown of a multi-threaded run, and calls at any other time
//   come from one thread. The heap calls themselves are thread-safe, so
//   they run outside the mutex; "link, update the statistics" is one step
//   under it.
//
// HOW THIS FILE IS ARRANGED
//   1. includes, the header, the file state, the mutex, zeroing, reporting
//   2. the standalone heap: raw_*
//   3. choosing the mode:   is_own, delegating_now, the delegate_to / export / is_delegating API
//   4. the tracker:         the list, the tracked allocation
//   5. lifecycle:           init, init_single_threaded, init_win32, shutdown
//   6. the allocation API:  alloc, calloc, realloc, free
//   7. the untracked API:   memory_manager__host_*
//   8. reporting:           get_stats, dump_live
#include "memory_manager.h"

#if defined(_WIN32)
#include <windows.h>
typedef CRITICAL_SECTION _memory_manager_internal__mutex;
#else
#include <pthread.h>
typedef pthread_mutex_t _memory_manager_internal__mutex;
#if !defined(MEMORY_MANAGER__NO_COMMON_RUNTIME)
#include <stdlib.h>
#endif
#endif

// Reporting goes to printf; without a C runtime nothing is printed.
#if !defined(MEMORY_MANAGER__NO_COMMON_RUNTIME)
#include <stdio.h>
#define _MEMORY_MANAGER_INTERNAL__REPORT(...) printf(__VA_ARGS__)
#else
#define _MEMORY_MANAGER_INTERNAL__REPORT(...) ((void)0)
#endif

#define _MEMORY_MANAGER_INTERNAL__GUARD 0xA110C0DEu
#define _MEMORY_MANAGER_INTERNAL__FREED 0xDEADBEEFu

// Fundamental alignment without relying on optional max_align_t definitions.
typedef union _memory_manager_internal__alignment_t
{
    long double floating;
    uint64 integer;
    void* pointer;
} _memory_manager_internal__alignment_t;

typedef struct _memory_manager_internal__header_t
{
    _Alignas(_memory_manager_internal__alignment_t) struct _memory_manager_internal__header_t* prev;
    struct _memory_manager_internal__header_t* next;
    nuint size;
    uint type;
    uint guard;
} _memory_manager_internal__header_t;

// The list head and the statistics are read and written under _lock while
// _has_mutex is TRUE (a multi-threaded standalone run between init and
// shutdown); at every other time there is no mutex.
static _memory_manager_internal__header_t* _memory_manager_internal__head = NULL_POINTER;
static memory_manager_stats_t _memory_manager_internal__stats;
static _memory_manager_internal__mutex _memory_manager_internal__lock;
static boole _memory_manager_internal__has_mutex = FALSE;
static boole _memory_manager_internal__initialized = FALSE;

// Delegating or standalone. Decided once, at init or at the first
// allocation, and held until shutdown.
static boole _memory_manager_internal__delegating = FALSE;
static boole _memory_manager_internal__mode_decided = FALSE;

// The functions passed to memory_manager__delegate_to (_offered, complete
// or not) and, once the mode is decided, the copy in use (_active).
static memory_manager_functions_t _memory_manager_internal__offered;
static memory_manager_functions_t _memory_manager_internal__active;

#if defined(_WIN32)
// The Win32 functions the standalone heap and the mutex go through, kept
// across shutdown. _heap_created_here: the handle came from HeapCreate in
// init and is destroyed at shutdown; a supplied handle is not.
static memory_manager_win32_t _memory_manager_internal__win32;
static boole _memory_manager_internal__heap_created_here = FALSE;
#endif

// This manager's identity, handed out as `owner` by export. Only its
// address matters: two copies of this file in two modules have two
// addresses, one copy has one.
static const char _memory_manager_internal__identity = 0;

// Every static in this file, declared here so the sections below can be
// read in any order.
static void  _memory_manager_internal__zero(void* dst, nuint count);
static void  _memory_manager_internal__mutex_init(void);
static void  _memory_manager_internal__mutex_destroy(void);
static void  _memory_manager_internal__enter(void);
static void  _memory_manager_internal__leave(void);
static void* _memory_manager_internal__raw_allocate(nuint bytes, boole zero);
static void* _memory_manager_internal__raw_reallocate(void* block, nuint bytes);
static void  _memory_manager_internal__raw_release(void* block);
static void* _memory_manager_internal__delegate_allocate(nuint count, nuint size, boole zero, uint type);
static boole _memory_manager_internal__is_own(const memory_manager_functions_t* functions);
static boole _memory_manager_internal__delegating_now(void);
static nuint _memory_manager_internal__counter(uint type);
static void  _memory_manager_internal__link(_memory_manager_internal__header_t* h);
static void  _memory_manager_internal__unlink(_memory_manager_internal__header_t* h);
static void* _memory_manager_internal__allocate(nuint size, uint type, boole zero);
static boole _memory_manager_internal__init(boole single_threaded);

// No memset: this file has no C runtime to rely on.
static void _memory_manager_internal__zero(void* dst, nuint count)
{
    unsigned char* bytes = (unsigned char*)dst;
    nuint i = 0;

    for (i = 0; i < count; i++)
    {
        bytes[i] = 0;
    }
}

// The mutex: a CRITICAL_SECTION through the _win32 table, or a pthread
// mutex. enter and leave do nothing while no mutex exists.
static void _memory_manager_internal__mutex_init(void)
{
#if defined(_WIN32)
    _memory_manager_internal__win32.InitializeCriticalSection(&_memory_manager_internal__lock);
#else
    pthread_mutex_init(&_memory_manager_internal__lock, NULL_POINTER);
#endif
}

static void _memory_manager_internal__mutex_destroy(void)
{
#if defined(_WIN32)
    // optional in the table: without it the section is left as it is
    if (_memory_manager_internal__win32.DeleteCriticalSection != NULL_POINTER)
    {
        _memory_manager_internal__win32.DeleteCriticalSection(&_memory_manager_internal__lock);
    }
#else
    pthread_mutex_destroy(&_memory_manager_internal__lock);
#endif
}

static void _memory_manager_internal__enter(void)
{
    if (_memory_manager_internal__has_mutex == FALSE)
    {
        return;
    }
#if defined(_WIN32)
    _memory_manager_internal__win32.EnterCriticalSection(&_memory_manager_internal__lock);
#else
    pthread_mutex_lock(&_memory_manager_internal__lock);
#endif
}

static void _memory_manager_internal__leave(void)
{
    if (_memory_manager_internal__has_mutex == FALSE)
    {
        return;
    }
#if defined(_WIN32)
    _memory_manager_internal__win32.LeaveCriticalSection(&_memory_manager_internal__lock);
#else
    pthread_mutex_unlock(&_memory_manager_internal__lock);
#endif
}

// ===== 2. the standalone heap ===================================================
// Windows: the _win32 table; NULL from every call until it holds a heap.
// Elsewhere: the C runtime, or NULL from every call without one.

static void* _memory_manager_internal__raw_allocate(nuint bytes, boole zero)
{
#if defined(_WIN32)
    if (_memory_manager_internal__win32.heap_handle == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    // the heap zeroes the block itself, fresh pages included
    return _memory_manager_internal__win32.HeapAlloc(_memory_manager_internal__win32.heap_handle, (zero == TRUE) ? HEAP_ZERO_MEMORY : 0, (memory_manager_win32_size_t)bytes);
#elif !defined(MEMORY_MANAGER__NO_COMMON_RUNTIME)
    return (zero == TRUE) ? calloc(1, (size_t)bytes) : malloc((size_t)bytes);
#else
    (void)bytes;
    (void)zero;
    return NULL_POINTER;
#endif
}

static void* _memory_manager_internal__raw_reallocate(void* block, nuint bytes)
{
#if defined(_WIN32)
    if (_memory_manager_internal__win32.heap_handle == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    // never a NULL block or a zero size here (the callers turned those into
    // an allocation or a release), which HeapReAlloc would reject
    return _memory_manager_internal__win32.HeapReAlloc(_memory_manager_internal__win32.heap_handle, 0, block, (memory_manager_win32_size_t)bytes);
#elif !defined(MEMORY_MANAGER__NO_COMMON_RUNTIME)
    return realloc(block, (size_t)bytes);
#else
    (void)block;
    (void)bytes;
    return NULL_POINTER;
#endif
}

static void _memory_manager_internal__raw_release(void* block)
{
#if defined(_WIN32)
    if (_memory_manager_internal__win32.heap_handle != NULL_POINTER)
    {
        _memory_manager_internal__win32.HeapFree(_memory_manager_internal__win32.heap_handle, 0, block);
    }
#elif !defined(MEMORY_MANAGER__NO_COMMON_RUNTIME)
    free(block);
#else
    (void)block;
#endif
}

// Delegating: the callers have already turned a NULL block into an
// allocation, a zero size into a release and checked count * size for
// overflow, so the other copy only receives the calls memory_manager.h
// lists. reallocate and release are called directly.
static void* _memory_manager_internal__delegate_allocate(nuint count, nuint size, boole zero, uint type)
{
    void* block = NULL_POINTER;
    nuint bytes = count * size;

    // the type number is passed on unchanged: the memory manager
    // initialised first records it with the block
    if (zero == TRUE && _memory_manager_internal__active.allocate_zeroed != NULL_POINTER)
    {
        return _memory_manager_internal__active.allocate_zeroed(count, size, type);
    }
    block = _memory_manager_internal__active.allocate(bytes, type);
    // no calloc of the other copy's own: zero what its allocate returned
    if (block != NULL_POINTER && zero == TRUE)
    {
        _memory_manager_internal__zero(block, bytes);
    }
    return block;
}

// ===== 3. choosing the mode ======================================================

/**
 * @brief recognise functions that would lead straight back into this same manager
 * @param const memory_manager_functions_t* functions -> the offered functions
 * @return boole -> TRUE when delegating to them would make every allocation call itself
 */
static boole _memory_manager_internal__is_own(const memory_manager_functions_t* functions)
{
    // the token covers everything export hands out, even through a DLL
    // import thunk whose address differs from the function's
    if (functions->owner == (const void*)&_memory_manager_internal__identity)
    {
        return TRUE;
    }
    // the same copy's functions, assigned by address
    if (functions->allocate == memory_manager__alloc || functions->allocate_zeroed == memory_manager__calloc)
    {
        return TRUE;
    }
    if (functions->reallocate == memory_manager__realloc || functions->reallocate == memory_manager__host_realloc)
    {
        return TRUE;
    }
    if (functions->release == memory_manager__free || functions->release == memory_manager__host_free)
    {
        return TRUE;
    }
    return FALSE;
}

/**
 * @brief the mode, decided on the first call and held until shutdown
 * @return boole -> TRUE when every call is passed to the copied functions of the other copy
 */
static boole _memory_manager_internal__delegating_now(void)
{
    const memory_manager_functions_t* offered = &_memory_manager_internal__offered;
    const char* refusal = NULL_POINTER;
    boole delegating = FALSE;

    if (_memory_manager_internal__mode_decided == TRUE)
    {
        return _memory_manager_internal__delegating;
    }
    if (offered->allocate != NULL_POINTER && offered->reallocate != NULL_POINTER && offered->release != NULL_POINTER)
    {
        if (_memory_manager_internal__is_own(offered) == TRUE)
        {
            // standalone instead: the alternative is an allocation that
            // calls itself until the stack runs out
            refusal = "memory_manager: the functions offered are this manager's own, delegation refused, managing memory here\n";
        }
        else if (_memory_manager_internal__head != NULL_POINTER)
        {
            // blocks with a header of this manager are live (allocated
            // before init, or left over from an earlier run); released
            // through the other copy they would be foreign pointers
            refusal = "memory_manager: blocks allocated standalone are still live, delegation refused, managing memory here\n";
        }
        else
        {
            delegating = TRUE;
            _memory_manager_internal__active = *offered;
        }
    }
    // the decision is published in one write each, before anything is
    // printed
    _memory_manager_internal__delegating = delegating;
    _memory_manager_internal__mode_decided = TRUE;
    if (refusal != NULL_POINTER)
    {
        _MEMORY_MANAGER_INTERNAL__REPORT("%s", refusal);
    }
    return delegating;
}

void memory_manager__delegate_to(const memory_manager_functions_t* functions)
{
    // takes effect when the mode is next decided; the copy in use for the
    // live blocks is not touched
    if (functions == NULL_POINTER)
    {
        _memory_manager_internal__zero(&_memory_manager_internal__offered, sizeof(_memory_manager_internal__offered));
        return;
    }
    _memory_manager_internal__offered = *functions;
}

void memory_manager__export(memory_manager_functions_t* out)
{
    if (out == NULL_POINTER)
    {
        return;
    }
    out->allocate = &memory_manager__alloc;
    out->allocate_zeroed = &memory_manager__calloc;
    out->reallocate = &memory_manager__realloc;
    out->release = &memory_manager__free;
    // lets the receiving copy tell whether these are its own functions
    out->owner = (const void*)&_memory_manager_internal__identity;
}

boole memory_manager__is_delegating(void)
{
    return (boole)(_memory_manager_internal__mode_decided == TRUE && _memory_manager_internal__delegating == TRUE);
}

// ===== 4. the tracker: only standalone mode reaches this =========================

/**
 * @brief the live-byte counter a type number is added to
 * @param uint type -> the number the block was allocated with, stored unchanged in its header
 * @return nuint -> index into memory_manager_stats_t.live_bytes_by_type
 */
static nuint _memory_manager_internal__counter(uint type)
{
    // the manager does not interpret type numbers (each codebase that
    // compiles this file numbers its own types); numbers past the counters
    // share the last one
    if ((nuint)type < (nuint)MEMORY_MANAGER__TYPE_COUNTERS)
    {
        return (nuint)type;
    }
    return (nuint)MEMORY_MANAGER__TYPE_COUNTERS - 1;
}

// List helpers. Must be called with the mutex held.
static void _memory_manager_internal__link(_memory_manager_internal__header_t* h)
{
    h->prev = NULL_POINTER;
    h->next = _memory_manager_internal__head;
    if (_memory_manager_internal__head != NULL_POINTER)
    {
        _memory_manager_internal__head->prev = h;
    }
    _memory_manager_internal__head = h;
}

static void _memory_manager_internal__unlink(_memory_manager_internal__header_t* h)
{
    if (h->prev != NULL_POINTER)
    {
        h->prev->next = h->next;
    }
    else
    {
        _memory_manager_internal__head = h->next;
    }
    if (h->next != NULL_POINTER)
    {
        h->next->prev = h->prev;
    }
    h->prev = NULL_POINTER;
    h->next = NULL_POINTER;
}

static void* _memory_manager_internal__allocate(nuint size, uint type, boole zero)
{
    _memory_manager_internal__header_t* h = NULL_POINTER;

    if (size == 0 || size > (nuint)-1 - sizeof(_memory_manager_internal__header_t))
    {
        return NULL_POINTER;
    }
    // the heap call is thread-safe on its own, so it runs outside the mutex;
    // only the list and the statistics need it
    h = (_memory_manager_internal__header_t*)_memory_manager_internal__raw_allocate(sizeof(_memory_manager_internal__header_t) + size, zero);
    if (h == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    h->size = size;
    h->type = type;
    h->guard = _MEMORY_MANAGER_INTERNAL__GUARD;

    _memory_manager_internal__enter();
    _memory_manager_internal__link(h);
    _memory_manager_internal__stats.live_allocs += 1;
    _memory_manager_internal__stats.total_allocs += 1;
    _memory_manager_internal__stats.live_bytes += (int64)size;
    _memory_manager_internal__stats.live_bytes_by_type[_memory_manager_internal__counter(type)] += (int64)size;
    if (_memory_manager_internal__stats.live_bytes > _memory_manager_internal__stats.peak_bytes)
    {
        _memory_manager_internal__stats.peak_bytes = _memory_manager_internal__stats.live_bytes;
    }
    _memory_manager_internal__leave();

    return (void*)(h + 1);
}

// ===== 5. lifecycle ==============================================================

/**
 * @brief decide the mode, set up the standalone heap and the mutex, restart the totals
 * @param boole single_threaded -> TRUE: no mutex, every call from one thread
 * @return boole -> TRUE when initialised; FALSE when already initialised, or when a standalone run has no heap to allocate from
 */
static boole _memory_manager_internal__init(boole single_threaded)
{
    boole delegating = FALSE;

    if (_memory_manager_internal__initialized == TRUE)
    {
        return FALSE;
    }
    delegating = _memory_manager_internal__delegating_now();
#if defined(_WIN32)
    if (delegating == FALSE)
    {
        if (_memory_manager_internal__win32.HeapAlloc == NULL_POINTER)
        {
#if !defined(MEMORY_MANAGER__NO_COMMON_RUNTIME)
            // a build with a C runtime: the linked functions, on the heap
            // malloc uses; the program may still supply its own instead
            _memory_manager_internal__win32.heap_handle = GetProcessHeap();
            _memory_manager_internal__win32.HeapAlloc = HeapAlloc;
            _memory_manager_internal__win32.HeapReAlloc = HeapReAlloc;
            _memory_manager_internal__win32.HeapFree = HeapFree;
            _memory_manager_internal__win32.InitializeCriticalSection = InitializeCriticalSection;
            _memory_manager_internal__win32.EnterCriticalSection = EnterCriticalSection;
            _memory_manager_internal__win32.LeaveCriticalSection = LeaveCriticalSection;
            _memory_manager_internal__win32.DeleteCriticalSection = DeleteCriticalSection;
#else
            return FALSE;
#endif
        }
        if (_memory_manager_internal__win32.heap_handle == NULL_POINTER)
        {
            _memory_manager_internal__win32.heap_handle = _memory_manager_internal__win32.HeapCreate(0, 0, 0);
            if (_memory_manager_internal__win32.heap_handle == NULL_POINTER)
            {
                _MEMORY_MANAGER_INTERNAL__REPORT("%s", "memory_manager: HeapCreate failed\n");
                return FALSE;
            }
            _memory_manager_internal__heap_created_here = TRUE;
        }
        // no critical-section functions in the table: every run is single-threaded
        if (_memory_manager_internal__win32.InitializeCriticalSection == NULL_POINTER)
        {
            single_threaded = TRUE;
        }
    }
#endif
    // a pass-through touches no shared state of its own, so it needs no
    // lock: the manager initialised first serialises its own bookkeeping
    if (single_threaded == FALSE && delegating == FALSE)
    {
        _memory_manager_internal__mutex_init();
        _memory_manager_internal__has_mutex = TRUE;
    }
    // blocks allocated before init, or reported as leaked by an earlier
    // shutdown, are still live: they stay in the list and in the live
    // figures. The totals and the peak count from this init.
    _memory_manager_internal__stats.total_allocs = 0;
    _memory_manager_internal__stats.total_frees = 0;
    _memory_manager_internal__stats.peak_bytes = _memory_manager_internal__stats.live_bytes;
    _memory_manager_internal__initialized = TRUE;
    return TRUE;
}

void memory_manager__init(void)
{
    _memory_manager_internal__init(FALSE);
}

void memory_manager__init_single_threaded(void)
{
    _memory_manager_internal__init(TRUE);
}

#if defined(_WIN32)
boole memory_manager__init_win32(const memory_manager_win32_t* win32)
{
    boole sections = FALSE;

    if (win32 == NULL_POINTER || _memory_manager_internal__initialized == TRUE)
    {
        return FALSE;
    }
    // the heap functions, and a heap to use them on: a handle, or
    // HeapCreate and HeapDestroy to make and later destroy one here
    if (win32->HeapAlloc == NULL_POINTER || win32->HeapReAlloc == NULL_POINTER || win32->HeapFree == NULL_POINTER)
    {
        return FALSE;
    }
    if (win32->heap_handle == NULL_POINTER && (win32->HeapCreate == NULL_POINTER || win32->HeapDestroy == NULL_POINTER))
    {
        return FALSE;
    }
    // the critical-section functions: Initialize, Enter and Leave together,
    // or none of them for a single-threaded run
    sections = (boole)(win32->InitializeCriticalSection != NULL_POINTER || win32->EnterCriticalSection != NULL_POINTER || win32->LeaveCriticalSection != NULL_POINTER);
    if (sections == TRUE && (win32->InitializeCriticalSection == NULL_POINTER || win32->EnterCriticalSection == NULL_POINTER || win32->LeaveCriticalSection == NULL_POINTER))
    {
        return FALSE;
    }
    _memory_manager_internal__win32 = *win32;
    _memory_manager_internal__heap_created_here = FALSE;
    return _memory_manager_internal__init((boole)(sections == FALSE));
}
#endif

void memory_manager__shutdown(void)
{
    memory_manager_stats_t s;

    if (_memory_manager_internal__initialized == FALSE)
    {
        // never initialised, or shut down already: only the mode is
        // released, so that the next init decides it again
        _memory_manager_internal__mode_decided = FALSE;
        _memory_manager_internal__delegating = FALSE;
        return;
    }
    // delegating: there is nothing here to report or release, the manager
    // initialised first holds every block and reports its own leaks
    if (_memory_manager_internal__delegating == FALSE)
    {
        _memory_manager_internal__enter();
        s = _memory_manager_internal__stats;
        _memory_manager_internal__leave();
        if (s.live_allocs != 0 || s.live_bytes != 0)
        {
            _MEMORY_MANAGER_INTERNAL__REPORT("memory_manager: %lld leaked allocation(s), %lld bytes\n", (long long)s.live_allocs, (long long)s.live_bytes);
            memory_manager__dump_live();
        }
        else
        {
            _MEMORY_MANAGER_INTERNAL__REPORT("memory_manager: no leaks. %lld allocs / %lld frees, peak %lld bytes\n", (long long)s.total_allocs, (long long)s.total_frees, (long long)s.peak_bytes);
        }
    }
    if (_memory_manager_internal__has_mutex == TRUE)
    {
        _memory_manager_internal__mutex_destroy();
        _memory_manager_internal__has_mutex = FALSE;
    }
#if defined(_WIN32)
    if (_memory_manager_internal__heap_created_here == TRUE)
    {
        // the heap goes, and every block in it with it, leaked ones
        // included: nothing of this run is live afterwards
        _memory_manager_internal__win32.HeapDestroy(_memory_manager_internal__win32.heap_handle);
        _memory_manager_internal__win32.heap_handle = NULL_POINTER;
        _memory_manager_internal__heap_created_here = FALSE;
        _memory_manager_internal__head = NULL_POINTER;
        _memory_manager_internal__stats.live_allocs = 0;
        _memory_manager_internal__stats.live_bytes = 0;
        _memory_manager_internal__zero(_memory_manager_internal__stats.live_bytes_by_type, sizeof(_memory_manager_internal__stats.live_bytes_by_type));
    }
#endif
    _memory_manager_internal__zero(&_memory_manager_internal__active, sizeof(_memory_manager_internal__active));
    _memory_manager_internal__initialized = FALSE;
    _memory_manager_internal__mode_decided = FALSE;
    _memory_manager_internal__delegating = FALSE;
}

// ===== 6. the allocation API =====================================================

void* memory_manager__alloc(nuint size, uint type)
{
    if (_memory_manager_internal__delegating_now() == TRUE)
    {
        if (size == 0)
        {
            return NULL_POINTER;
        }
        return _memory_manager_internal__delegate_allocate(1, size, FALSE, type);
    }
    return _memory_manager_internal__allocate(size, type, FALSE);
}

void* memory_manager__calloc(nuint count, nuint size, uint type)
{
    nuint total = 0;

    // count * size wrapping would allocate a small block for a caller that
    // expects count * size bytes
    if (count != 0 && size > (nuint)-1 / count)
    {
        return NULL_POINTER;
    }
    total = count * size;
    if (_memory_manager_internal__delegating_now() == TRUE)
    {
        if (total == 0)
        {
            return NULL_POINTER;
        }
        return _memory_manager_internal__delegate_allocate(count, size, TRUE, type);
    }
    return _memory_manager_internal__allocate(total, type, TRUE);
}

void* memory_manager__realloc(void* ptr, nuint new_size)
{
    _memory_manager_internal__header_t* old_h = NULL_POINTER;
    _memory_manager_internal__header_t* new_h = NULL_POINTER;
    nuint old_size = 0;
    uint type = 0;
    int64 delta = 0;

    if (ptr == NULL_POINTER)
    {
        // as libc: realloc(NULL, n) is an allocation; no type is known here
        return memory_manager__alloc(new_size, 0);
    }
    if (new_size == 0)
    {
        memory_manager__free(ptr);
        return NULL_POINTER;
    }
    if (_memory_manager_internal__delegating_now() == TRUE)
    {
        return _memory_manager_internal__active.reallocate(ptr, new_size);
    }
    if (new_size > (nuint)-1 - sizeof(_memory_manager_internal__header_t))
    {
        return NULL_POINTER;
    }
    old_h = ((_memory_manager_internal__header_t*)ptr) - 1;

    // the heap runs under the mutex here, so the list stays consistent
    // while the block may move; a failed resize leaves the block as it was
    _memory_manager_internal__enter();
    if (old_h->guard != _MEMORY_MANAGER_INTERNAL__GUARD)
    {
        _memory_manager_internal__leave();
        _MEMORY_MANAGER_INTERNAL__REPORT("memory_manager__realloc: bad guard on %p (not a tracked block, or corrupted)\n", ptr);
        return NULL_POINTER;
    }
    old_size = old_h->size;
    type = old_h->type;
    if (new_size == old_size)
    {
        _memory_manager_internal__leave();
        return ptr;
    }
    new_h = (_memory_manager_internal__header_t*)_memory_manager_internal__raw_reallocate(old_h, sizeof(*old_h) + new_size);
    if (new_h == NULL_POINTER)
    {
        _memory_manager_internal__leave();
        return NULL_POINTER;
    }
    if (new_h != old_h)
    {
        // the block moved: its neighbours still point at the old address
        if (new_h->prev != NULL_POINTER)
        {
            new_h->prev->next = new_h;
        }
        else
        {
            _memory_manager_internal__head = new_h;
        }
        if (new_h->next != NULL_POINTER)
        {
            new_h->next->prev = new_h;
        }
    }
    new_h->size = new_size;
    delta = (int64)new_size - (int64)old_size;
    _memory_manager_internal__stats.live_bytes += delta;
    _memory_manager_internal__stats.live_bytes_by_type[_memory_manager_internal__counter(type)] += delta;
    if (_memory_manager_internal__stats.live_bytes > _memory_manager_internal__stats.peak_bytes)
    {
        _memory_manager_internal__stats.peak_bytes = _memory_manager_internal__stats.live_bytes;
    }
    _memory_manager_internal__leave();

    return (void*)(new_h + 1);
}

void memory_manager__free(void* ptr)
{
    _memory_manager_internal__header_t* h = NULL_POINTER;

    if (ptr == NULL_POINTER)
    {
        return;
    }
    if (_memory_manager_internal__delegating_now() == TRUE)
    {
        _memory_manager_internal__active.release(ptr);
        return;
    }
    h = ((_memory_manager_internal__header_t*)ptr) - 1;

    _memory_manager_internal__enter();
    if (h->guard == _MEMORY_MANAGER_INTERNAL__FREED)
    {
        _memory_manager_internal__leave();
        _MEMORY_MANAGER_INTERNAL__REPORT("memory_manager__free: double-free detected on %p\n", ptr);
        return;
    }
    if (h->guard != _MEMORY_MANAGER_INTERNAL__GUARD)
    {
        _memory_manager_internal__leave();
        _MEMORY_MANAGER_INTERNAL__REPORT("memory_manager__free: bad guard on %p (not a tracked block, or corrupted header)\n", ptr);
        return;
    }
    _memory_manager_internal__stats.live_allocs -= 1;
    _memory_manager_internal__stats.total_frees += 1;
    _memory_manager_internal__stats.live_bytes -= (int64)h->size;
    _memory_manager_internal__stats.live_bytes_by_type[_memory_manager_internal__counter(h->type)] -= (int64)h->size;
    _memory_manager_internal__unlink(h);
    h->guard = _MEMORY_MANAGER_INTERNAL__FREED;
    _memory_manager_internal__leave();

    // the release itself is thread-safe; no need to hold the mutex for it
    _memory_manager_internal__raw_release(h);
}

// ===== 7. the untracked API ======================================================

void* memory_manager__host_alloc(nuint size)
{
    if (size == 0)
    {
        return NULL_POINTER;
    }
    if (_memory_manager_internal__delegating_now() == TRUE)
    {
        return _memory_manager_internal__delegate_allocate(1, size, FALSE, 0);
    }
    return _memory_manager_internal__raw_allocate(size, FALSE);
}

void* memory_manager__host_calloc(nuint count, nuint size)
{
    if (count == 0 || size == 0 || size > (nuint)-1 / count)
    {
        return NULL_POINTER;
    }
    if (_memory_manager_internal__delegating_now() == TRUE)
    {
        return _memory_manager_internal__delegate_allocate(count, size, TRUE, 0);
    }
    return _memory_manager_internal__raw_allocate(count * size, TRUE);
}

void* memory_manager__host_realloc(void* ptr, nuint size)
{
    if (ptr == NULL_POINTER)
    {
        return memory_manager__host_alloc(size);
    }
    if (size == 0)
    {
        memory_manager__host_free(ptr);
        return NULL_POINTER;
    }
    if (_memory_manager_internal__delegating_now() == TRUE)
    {
        return _memory_manager_internal__active.reallocate(ptr, size);
    }
    return _memory_manager_internal__raw_reallocate(ptr, size);
}

void memory_manager__host_free(void* ptr)
{
    if (ptr == NULL_POINTER)
    {
        return;
    }
    if (_memory_manager_internal__delegating_now() == TRUE)
    {
        _memory_manager_internal__active.release(ptr);
        return;
    }
    _memory_manager_internal__raw_release(ptr);
}

// ===== 8. reporting ==============================================================

memory_manager_stats_t memory_manager__get_stats(void)
{
    memory_manager_stats_t snapshot;

    // delegating: every block is counted by the manager initialised first
    if (_memory_manager_internal__mode_decided == TRUE && _memory_manager_internal__delegating == TRUE)
    {
        _memory_manager_internal__zero(&snapshot, sizeof(snapshot));
        return snapshot;
    }
    // before init and after shutdown there is no mutex and no other thread
    // allocates; after shutdown these are the final figures
    _memory_manager_internal__enter();
    snapshot = _memory_manager_internal__stats;
    _memory_manager_internal__leave();
    return snapshot;
}

void memory_manager__dump_live(void)
{
#if !defined(MEMORY_MANAGER__NO_COMMON_RUNTIME)
    memory_manager_stats_t s;
    _memory_manager_internal__header_t* h = NULL_POINTER;
    int shown = 0;
    int i = 0;

    if (_memory_manager_internal__mode_decided == TRUE && _memory_manager_internal__delegating == TRUE)
    {
        _MEMORY_MANAGER_INTERNAL__REPORT("%s", "memory_manager: delegating, the live blocks are listed by the other copy\n");
        return;
    }
    _memory_manager_internal__enter();
    s = _memory_manager_internal__stats;
    _MEMORY_MANAGER_INTERNAL__REPORT("memory_manager: live=%lld bytes across %lld allocs (peak %lld bytes)\n", (long long)s.live_bytes, (long long)s.live_allocs, (long long)s.peak_bytes);
    // type numbers only: the names belong to the codebase that chose them
    for (i = 0; i < MEMORY_MANAGER__TYPE_COUNTERS; i++)
    {
        if (s.live_bytes_by_type[i] != 0)
        {
            _MEMORY_MANAGER_INTERNAL__REPORT("  type %d%s %lld bytes\n", i, (i == MEMORY_MANAGER__TYPE_COUNTERS - 1) ? "+" : "", (long long)s.live_bytes_by_type[i]);
        }
    }
    for (h = _memory_manager_internal__head; h != NULL_POINTER; h = h->next)
    {
        if (shown < 32)
        {
            _MEMORY_MANAGER_INTERNAL__REPORT("  [type %u] %llu bytes @ %p\n", (unsigned)h->type, (unsigned long long)h->size, (void*)(h + 1));
        }
        shown++;
    }
    if (shown > 32)
    {
        _MEMORY_MANAGER_INTERNAL__REPORT("  ... and %d more\n", shown - 32);
    }
    _memory_manager_internal__leave();
#endif
}
