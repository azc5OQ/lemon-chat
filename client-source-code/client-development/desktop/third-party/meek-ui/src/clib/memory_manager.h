#ifndef CLIB_MEMORY_MANAGER_H
#define CLIB_MEMORY_MANAGER_H

// clib/memory_manager.h - allocation tracker.
// Wraps the heap so that, at any time, the live blocks, their sizes and
// the type numbers they were allocated with can be read. The metadata is
// an inline header before the user pointer, so alloc and free are O(1):
// no table scan, one step back from the pointer and an unlink.
//
// SELF-CONTAINED
// This header and memory_manager.c are the whole memory manager. They
// depend on nothing of the project that compiles them: the few types they
// use are defined below, the only includes are system headers, and the
// functions carry no export decoration (a DLL that exports them lists
// them in its .def file; meek-ui: src/meek_ui.def). The GUI_* macros at
// the end are meek-ui's allocation entry points; another project may use
// or ignore them.
//
// USAGE
// -----
// #include "memory_manager.h"
// memory_manager__init();
// void* p = memory_manager__alloc(128, 0);
// void* q = memory_manager__calloc(4, 32, 1);   /* 1: a type number of the caller's codebase */
// p = memory_manager__realloc(p, 256);
// memory_manager__free(p);
// memory_manager__free(q);
// memory_manager__shutdown();                    /* prints the leak report */
//
// CONFIGURATION (presence macros: defined or not, the value is not read)
// GUI_TRACK_ALLOCATIONS: the GUI_* macros go through the tracked API.
// GUI_DISABLE_MEMORY_TRACKING: overrides it, the macros use the untracked API.
// MEMORY_MANAGER__NO_COMMON_RUNTIME: the program links no C runtime. On
//   Windows the heap and the mutex are then the Win32 functions passed to
//   memory_manager__init_win32 (without this switch memory_manager__init
//   fills them itself with the linked functions and the process heap), and
//   nothing is printed. On other platforms the standalone heap is gone
//   (every allocation returns NULL) and nothing is printed either.
// MEMORY_MANAGER__TYPES_PROVIDED: the project defines nuint, uint, int64,
//   uint64, boole, TRUE, FALSE and NULL_POINTER itself, with the
//   definitions below, and this header does not. meek-ui's types.h is
//   recognised on its own.
//
// REPORTING
// Leaks at shutdown, a refused delegation, a bad or double free and the
// live-block listing go to printf. With MEMORY_MANAGER__NO_COMMON_RUNTIME
// nothing is printed; memory_manager__get_stats is the report.
//
// TWO MODES
// Standalone (nothing passed to memory_manager__delegate_to): this manager
// is the only one. It allocates from the heap of the platform and keeps the
// header, the list, the statistics and the leak report described above.
// Delegating (the functions of another copy of this manager passed to
// memory_manager__delegate_to before init): the memory manager that was
// initialised first owns the memory, and this one is a pass-through. Every
// call goes straight to those functions, the pointer returned is theirs
// with no header in front, and this manager keeps no list, no statistics
// and no lock; get_stats reads zero and shutdown reports nothing, because
// the other manager does both. The mode is chosen at init (or the first
// allocation) and holds until shutdown.
//
// THREADING
// Thread-safe by default: one mutex (CRITICAL_SECTION on Windows,
// pthread_mutex_t elsewhere) guards the live list and the statistics.
// memory_manager__init_single_threaded removes it; every allocation, free
// and reporting call must then stay on one thread.
//
// NAMING
// The GUI_ prefix of the macros sidesteps a Windows SDK collision: winnt.h
// defines MEM_FREE as a VirtualAlloc flag constant.

#include <stdint.h>

// ===== types ===============================================================
// The same definitions as meek-ui's types.h (skipped when that header, or
// the project's own equivalent, came first).
#if !defined(TYPES_H) && !defined(MEMORY_MANAGER__TYPES_PROVIDED)
#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
typedef unsigned long long nuint;
#elif UINTPTR_MAX == 0xFFFFFFFFu
typedef unsigned int nuint;
#else
typedef unsigned short nuint;
#endif
typedef signed char        boole;
typedef unsigned int       uint;
typedef unsigned long long uint64;
typedef signed long long   int64;
#ifndef FALSE
#define FALSE ((boole)0)
#endif
#ifndef TRUE
#define TRUE  ((boole)1)
#endif
#ifndef NULL_POINTER
#define NULL_POINTER 0
#endif
#endif

// ===== statistics ==========================================================

// How many type numbers have a live-byte counter of their own in
// memory_manager_stats_t (numbers 0 to 254; 255 and above share the last
// counter). Independent of any codebase's type list.
#define MEMORY_MANAGER__TYPE_COUNTERS 256

// Type numbers are plain unsigned integers chosen by the caller. The memory
// manager stores the number given with each block unchanged and counts live
// bytes per number; it does not interpret the numbers and holds no names or
// list of them, because the same memory manager is compiled into more than
// one codebase and each codebase numbers its own types (meek-ui's are in
// memory_types.h).

// Snapshot of the tracker state, returned by value so callers do not lock.
typedef struct memory_manager_stats_t
{
    int64 live_allocs;  // currently outstanding allocations
    int64 total_allocs; // alloc calls since init
    int64 total_frees;  // free calls since init
    int64 live_bytes;   // sum of sizes of live allocations
    int64 peak_bytes;   // highest live_bytes since init
    // live bytes per type number; numbers from MEMORY_MANAGER__TYPE_COUNTERS - 1
    // upwards share the last counter
    int64 live_bytes_by_type[MEMORY_MANAGER__TYPE_COUNTERS];
} memory_manager_stats_t;

// ===== lifecycle ===========================================================

// init decides the mode, creates the mutex (multi-threaded standalone runs)
// and restarts the totals and the peak; blocks allocated before init stay
// tracked and counted as live. shutdown prints any leaks (live_allocs != 0)
// and the live list, destroys the mutex, and releases the mode. Safe to
// call shutdown even if init was never called.
void memory_manager__init(void);
// Every tracked allocation, free and stats query must run on one thread.
void memory_manager__init_single_threaded(void);
void memory_manager__shutdown(void);

// ===== the allocation API ==================================================

// All three alloc flavours take a type number, stored as given with the
// block. Pass 0 when nothing more specific applies. realloc keeps the
// block's type number; realloc(NULL, n) allocates with type 0.
void* memory_manager__alloc(nuint size, uint type);
void* memory_manager__calloc(nuint count, nuint size, uint type);
void* memory_manager__realloc(void* ptr, nuint new_size);
void  memory_manager__free(void* ptr);

// The same heap without the header, the list or the statistics: where the
// GUI_* macros land when tracking is off. Standalone, a block from these
// must be released with memory_manager__host_free, never with
// memory_manager__free; delegating, the two paths are the same thing.
void* memory_manager__host_alloc(nuint size);
void* memory_manager__host_calloc(nuint count, nuint size);
void* memory_manager__host_realloc(void* ptr, nuint size);
void  memory_manager__host_free(void* ptr);

// ===== reporting ===========================================================

memory_manager_stats_t memory_manager__get_stats(void);
void                   memory_manager__dump_live(void);

// ===== one memory manager: delegating to the copy initialised first ========
// The same memory manager is compiled into more than one module. The copy
// initialised first owns the memory and passes the addresses of its own
// four functions to the copy initialised later:
//
//     /* the module initialised first */
//     memory_manager_functions_t functions;
//     memory_manager__export(&functions);            /* or: .allocate = &memory_manager__alloc, ... */
//
//     /* the module initialised later, before its memory_manager__init */
//     memory_manager__delegate_to(&functions);
//     memory_manager__init();
//
// The slot types are the four functions' own types, so the addresses are
// assigned as they are. The later copy then passes every call through,
// type number included, and keeps no header, no list, no statistics and
// no lock. It never calls allocate with a size of 0, allocate_zeroed with
// a zero or an overflowing count * size, reallocate with a NULL block or a
// zero size, or release with NULL: those cases are handled before.
// A copy refuses to delegate to its own functions (recognised by `owner`,
// which export sets, or by the addresses) and to any functions while blocks
// of its own are still live; it prints the refusal and stays standalone.
// The functions are copied when the mode is decided; a delegate_to during
// the run takes effect at the next init.

typedef void* (*memory_manager_allocate_fn)(nuint size, uint type);                      // &memory_manager__alloc
typedef void* (*memory_manager_allocate_zeroed_fn)(nuint count, nuint size, uint type);  // &memory_manager__calloc, optional
typedef void* (*memory_manager_reallocate_fn)(void* ptr, nuint new_size);                // &memory_manager__realloc
typedef void  (*memory_manager_release_fn)(void* ptr);                                   // &memory_manager__free

typedef struct memory_manager_functions_t
{
    memory_manager_allocate_fn        allocate;
    memory_manager_allocate_zeroed_fn allocate_zeroed;   // NULL: the later copy calls allocate and zeroes the block
    memory_manager_reallocate_fn      reallocate;
    memory_manager_release_fn         release;
    const void*                       owner;             // set by memory_manager__export; NULL when filled by hand
} memory_manager_functions_t;

// Call before memory_manager__init. NULL, or a struct with allocate,
// reallocate or release NULL, means standalone. The struct is copied.
void  memory_manager__delegate_to(const memory_manager_functions_t* functions);
// Writes this copy's four addresses and its owner token into `out`.
void  memory_manager__export(memory_manager_functions_t* out);
// TRUE while this copy is passing every call through to another copy.
boole memory_manager__is_delegating(void);

// ===== Windows: the heap and the mutex ======================================
// The functions the standalone heap and the mutex go through. A program
// without a C runtime (MEMORY_MANAGER__NO_COMMON_RUNTIME) resolves them
// itself and passes them once to memory_manager__init_win32, in the module
// whose memory manager is initialised first; a build with a C runtime may
// do the same, or call memory_manager__init and get the linked functions
// and the process heap. A copy that only delegates needs none of this.
//
// memory_manager_win32_t win32;                     /* zero it first */
// win32.HeapCreate                = resolved HeapCreate;
// win32.HeapAlloc                 = resolved HeapAlloc;
// win32.HeapReAlloc               = resolved HeapReAlloc;
// win32.HeapFree                  = resolved HeapFree;
// win32.HeapDestroy               = resolved HeapDestroy;
// win32.InitializeCriticalSection = resolved InitializeCriticalSection;
// win32.EnterCriticalSection      = resolved EnterCriticalSection;
// win32.LeaveCriticalSection      = resolved LeaveCriticalSection;
// win32.DeleteCriticalSection     = resolved DeleteCriticalSection;
// if (memory_manager__init_win32(&win32) == FALSE) { /* a function is missing, or HeapCreate failed */ }
#if defined(_WIN32)

// The Win32 prototypes with the types they are declared with in <windows.h>
// (HANDLE and LPVOID are void*, DWORD is unsigned long, SIZE_T has the
// width of a pointer, WINAPI is __stdcall), spelled out so that this
// header does not include <windows.h>. HeapAlloc itself, or a resolved
// address cast to the field's type, is assigned without an adapter.
#if defined(_WIN64)
typedef unsigned long long memory_manager_win32_size_t;
#else
typedef unsigned long memory_manager_win32_size_t;
#endif
struct _RTL_CRITICAL_SECTION;
typedef void* (__stdcall* memory_manager_heap_create_fn)(unsigned long options, memory_manager_win32_size_t initial_size, memory_manager_win32_size_t maximum_size);
typedef void* (__stdcall* memory_manager_heap_alloc_fn)(void* heap, unsigned long flags, memory_manager_win32_size_t bytes);
typedef void* (__stdcall* memory_manager_heap_realloc_fn)(void* heap, unsigned long flags, void* block, memory_manager_win32_size_t bytes);
typedef int   (__stdcall* memory_manager_heap_free_fn)(void* heap, unsigned long flags, void* block);
typedef int   (__stdcall* memory_manager_heap_destroy_fn)(void* heap);
typedef void  (__stdcall* memory_manager_critical_section_fn)(struct _RTL_CRITICAL_SECTION* section);

typedef struct memory_manager_win32_t
{
    // The heap the blocks come from. NULL: the manager calls
    // HeapCreate(0, 0, 0) at init and HeapDestroy at shutdown, so HeapCreate
    // and HeapDestroy are then required. Supplied: used as it is and never
    // destroyed here. In both cases a heap without HEAP_GENERATE_EXCEPTIONS,
    // so that a failed allocation returns NULL.
    void*                              heap_handle;
    memory_manager_heap_create_fn      HeapCreate;
    memory_manager_heap_alloc_fn       HeapAlloc;       // required
    memory_manager_heap_realloc_fn     HeapReAlloc;     // required
    memory_manager_heap_free_fn        HeapFree;        // required
    memory_manager_heap_destroy_fn     HeapDestroy;
    // Initialize, Enter and Leave all set: multi-threaded, the manager's own
    // mutex is a CRITICAL_SECTION driven through them. All three NULL: every
    // run is single-threaded, as memory_manager__init_single_threaded.
    // Delete is optional; without it the section is left as it is at
    // shutdown.
    memory_manager_critical_section_fn InitializeCriticalSection;
    memory_manager_critical_section_fn EnterCriticalSection;
    memory_manager_critical_section_fn LeaveCriticalSection;
    memory_manager_critical_section_fn DeleteCriticalSection;
} memory_manager_win32_t;

// The first-time init of the copy that owns memory. Copies the struct,
// creates the heap when none was supplied, then initialises the manager
// as memory_manager__init does (or single-threaded, see the struct).
// FALSE, and nothing initialised, when a required function is missing,
// HeapCreate fails, or the manager is already initialised. Functions passed
// to memory_manager__delegate_to still take precedence: the copy then
// delegates and the Win32 functions are kept unused. The struct is kept
// across shutdown, so a plain memory_manager__init afterwards reuses the
// functions and creates the heap again when it was created here.
boole memory_manager__init_win32(const memory_manager_win32_t* win32);
#endif

// ===== meek-ui's allocation macros =========================================
// GUI_TRACK_ALLOCATIONS off (or GUI_DISABLE_MEMORY_TRACKING defined) drops
// the tracker and its overhead; the type argument is then discarded. The
// heap is the same either way.
#if defined(GUI_TRACK_ALLOCATIONS) && !defined(GUI_DISABLE_MEMORY_TRACKING)
    #define GUI_MALLOC(size)              memory_manager__alloc((size), 0)
    #define GUI_MALLOC_T(size, type)      memory_manager__alloc((size), (type))
    #define GUI_CALLOC(n, size)           memory_manager__calloc((n), (size), 0)
    #define GUI_CALLOC_T(n, size, type)   memory_manager__calloc((n), (size), (type))
    #define GUI_REALLOC(ptr, size)        memory_manager__realloc((ptr), (size))
    #define GUI_FREE(ptr)                 memory_manager__free((ptr))
#else
    #define GUI_MALLOC(size)              memory_manager__host_alloc((size))
    #define GUI_MALLOC_T(size, type)      ((void)(type), memory_manager__host_alloc((size)))
    #define GUI_CALLOC(n, size)           memory_manager__host_calloc((n), (size))
    #define GUI_CALLOC_T(n, size, type)   ((void)(type), memory_manager__host_calloc((n), (size)))
    #define GUI_REALLOC(ptr, size)        memory_manager__host_realloc((ptr), (size))
    #define GUI_FREE(ptr)                 memory_manager__host_free((ptr))
#endif

#endif
