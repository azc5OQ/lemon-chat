#ifndef MEEK_UI_HOST_SERVICES_H
#define MEEK_UI_HOST_SERVICES_H

#include "types.h"
#include "meek_ui_config.h"
#include "clib/memory_manager.h"

// What the library cannot implement for itself: a heap, a place to write
// log lines, and reading a file. A host hands them over once, before
// anything else runs.
//
// ONE MEMORY MANAGER
// ------------------
// The same memory manager (clib/memory_manager.c) is compiled into more
// than one module. The copy that is initialised first owns the memory and
// hands its own functions to the copy initialised later:
//
//     /* in the module initialised first */
//     meek_host_services_t services;
//
//     stdlib__null_memory(&services, sizeof(services));
//     services.allocate        = &memory_manager__alloc;
//     services.allocate_zeroed = &memory_manager__calloc;
//     services.reallocate      = &memory_manager__realloc;
//     services.release         = &memory_manager__free;
//
//     /* in the module initialised later, before its memory_manager__init */
//     host_services__set(&services);
//
// The slot types are the memory manager's own function types, so the
// functions are passed by address as they are. The later copy then passes
// every call through to them, type number included, and keeps no header,
// no list, no statistics and no lock of its own. memory_manager__export_services
// fills the four slots the same way.
//
// A memory manager does not forward to its own functions: installing a
// copy's functions in that same copy would make every allocation call
// itself. It is recognised by the function addresses (and by `owner` when
// export_services filled the struct), printed, and refused; the copy then
// manages memory itself.
//
// Install before memory_manager__init. The mode is fixed there and holds
// until memory_manager__shutdown, so no block is ever allocated in one
// mode and released in the other; the four heap functions are copied at
// that moment, so a host_services__set during the run changes only the
// log and file slots. A copy with standalone blocks still live refuses to
// delegate as well.
//
// A missing log sink or file reader falls back to the C runtime when the
// library was built with MEEK_UI_HOST_SERVICES_FALLBACK (the default);
// with that switch off the feature is unavailable. A missing heap means
// the memory manager manages memory itself (clib/memory_manager.h:
// the C runtime, or the Win32 functions of memory_manager__init_win32,
// or nothing under MEMORY_MANAGER__NO_COMMON_RUNTIME).
//
// The struct is copied, so the caller's copy may go out of scope; the
// functions themselves must stay valid for the life of the library.

typedef enum meek_log_level_e
{
    MEEK_LOG__TRACE = 0,
    MEEK_LOG__DEBUG = 1,
    MEEK_LOG__INFO  = 2,
    MEEK_LOG__WARN  = 3,
    MEEK_LOG__ERROR = 4,
    MEEK_LOG__FATAL = 5
} meek_log_level_e;

// The heap slots have the memory manager's own function types
// (clib/memory_manager.h, memory_manager_functions_t): host_services__set
// hands the four slots and `owner` to memory_manager__delegate_to, which
// is where the memory manager reads them.
// memory_manager__alloc: `size` bytes, recorded with type number `type`,
// uninitialised, or NULL. The later copy never calls it with a size of 0.
typedef memory_manager_allocate_fn        meek_allocate_fn;
// memory_manager__calloc: `count * size` zeroed bytes with type number
// `type`, or NULL. The later copy checks the multiplication for overflow
// before calling and never passes a zero. Optional: without it, the later
// copy calls allocate and zeroes the block.
typedef memory_manager_allocate_zeroed_fn meek_allocate_zeroed_fn;
// memory_manager__realloc: resize a block, keeping its contents up to the
// smaller size and its type number, or return NULL and leave the original
// valid. The later copy only calls it with a live block and a size above
// 0; a NULL block goes to allocate and a zero size goes to release.
typedef memory_manager_reallocate_fn      meek_reallocate_fn;
// memory_manager__free: release a block. The later copy never calls it
// with NULL.
typedef memory_manager_release_fn         meek_release_fn;
// One finished log line, without a trailing newline. `file` and `line`
// are the library's own source position; the host decides where the text
// goes and must not call back into the library.
typedef void  (*meek_write_log_fn)(meek_log_level_e level, const char* file, int line, const char* message);
// Reads a whole file. On success it writes the bytes and their length and
// returns TRUE; the library later hands the same pointer to free_file, so
// a host may return a pointer into its own archive. FALSE means "not
// found", and the out parameters are then untouched.
typedef boole (*meek_read_file_fn)(const char* path, ubyte** out_bytes, int64* out_size);
// Releases what read_file returned. Required whenever read_file is set.
typedef void  (*meek_free_file_fn)(ubyte* bytes);

typedef struct meek_host_services_t
{
    // The memory manager of the module initialised first: allocate,
    // reallocate and release go together, and a partial set is ignored.
    // allocate_zeroed is optional.
    // &memory_manager__alloc:   void* (nuint size, uint type)
    meek_allocate_fn        allocate;
    // &memory_manager__calloc:  void* (nuint count, nuint size, uint type)
    meek_allocate_zeroed_fn allocate_zeroed;
    // &memory_manager__realloc: void* (void* block, nuint size)
    meek_reallocate_fn      reallocate;
    // &memory_manager__free:    void (void* block)
    meek_release_fn         release;

    // Which copy of the memory manager these functions belong to. Set by
    // memory_manager__export_services; NULL when the slots are filled by
    // hand. Used, with the function addresses, to refuse forwarding to the
    // same copy.
    const void*             owner;

    // where log lines go (NULL: the library's own sink, or silence in a
    // freestanding build)
    // void (*write_log)(meek_log_level_e level, const char* file, int line, const char* message)
    meek_write_log_fn       write_log;

    // how files are read (NULL: the platform backend's own file access)
    // boole (*read_file)(const char* path, ubyte** out_bytes, int64* out_size)
    meek_read_file_fn       read_file;
    // void (*free_file)(ubyte* bytes)
    meek_free_file_fn       free_file;
} meek_host_services_t;

// Install the services. Call before memory_manager__init and before any
// other library call; passing NULL restores the built-in fallbacks. The
// struct is copied, and the heap slots are handed to
// memory_manager__delegate_to.
GUI_API void host_services__set(const meek_host_services_t* services);
// The installed services, never NULL: the fields are NULL when the host
// left them out.
GUI_API const meek_host_services_t* host_services__get(void);
// Whether a complete heap (allocate, reallocate and release) is installed.
GUI_API boole host_services__has_heap(void);
// Writes this module's memory manager functions (&memory_manager__alloc,
// &memory_manager__calloc, &memory_manager__realloc, &memory_manager__free)
// and its owner token into the heap slots of `out`; the other fields are
// left as they are, so zero the struct first. memory_manager__export with
// the same result in the memory manager's own struct.
GUI_API void memory_manager__export_services(meek_host_services_t* out);

#endif
