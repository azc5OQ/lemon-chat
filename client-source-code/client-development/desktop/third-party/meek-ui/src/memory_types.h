#ifndef MEEK_UI_MEMORY_TYPES_H
#define MEEK_UI_MEMORY_TYPES_H

// meek-ui's allocation type numbers.
//
// The memory manager (clib/memory_manager.h) is compiled into more than one
// codebase and does not know these numbers: it stores the number passed
// with each block and counts live bytes per number. They are meek-ui's
// own, kept out of the memory manager's header. A codebase that shares the
// memory manager with meek-ui uses numbers that do not overlap these.
typedef enum mm_type_e
{
    MM_TYPE__GENERIC = 0,  // allocations without a more specific type
    MM_TYPE__TEXT = 1,     // strings, parser token buffers
    MM_TYPE__NODE = 2,     // scene graph nodes and attribute arrays
    MM_TYPE__STYLE = 3,    // style rules and selector tables
    MM_TYPE__FONT = 4,     // font atlases, glyph arrays, font files
    MM_TYPE__IMAGE = 5,    // texture pixel buffers
    MM_TYPE__RENDERER = 6, // renderer-owned buffers (vertex/index staging)
    MM_TYPE__FS = 7        // file-read buffers
} mm_type_e;

#endif
