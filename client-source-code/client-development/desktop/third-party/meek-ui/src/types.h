#ifndef TYPES_H
#define TYPES_H

// types.h - project-wide fixed-width and native-size typedefs
// used in place of stdint. every .c/.h in this project includes this.
// we pull in <stddef.h> so NULL, size_t, ptrdiff_t, and offsetof are
// available wherever types.h is included. avoids needing each
// widget file to remember to drag in stdlib/stdio just for NULL.

#include <stddef.h>

#include "architecture.h"

// byte-aliasing helper
typedef union safe_byte_t
{
    signed char   safe1;   // 0x0
    unsigned char saf2;    // 0x0
} safe_byte_t;

// Native pointer integers also cover ILP32 ABIs on 64-bit instruction sets.
#if ARCHITECTURE_POINTER_BYTES == 8
 typedef unsigned long long nuint;
 typedef signed long long nint;
#elif ARCHITECTURE_POINTER_BYTES == 4
 typedef unsigned int nuint;
 typedef signed int nint;
#else
 typedef unsigned short nuint;
 typedef signed short nint;
#endif

// fixed-width convenience types. use these across the project so that
// number sizes are unambiguous without dragging stdint everywhere.
typedef signed char        boole;
typedef unsigned char      ubyte;
typedef unsigned int       uint;
typedef unsigned short     ushort;
typedef unsigned long long uint64;
typedef signed long long   int64;
typedef unsigned long long timestamp;
typedef const char* cstring;

// These names have fixed contracts; never silently accept a mismatched ABI.
#if defined(__cplusplus)
static_assert(sizeof(nuint) == sizeof(void*) && sizeof(nint) == sizeof(void*), "native integer width");
static_assert(sizeof(ubyte) == 1 && sizeof(ushort) == 2 && sizeof(uint) == 4 && sizeof(uint64) == 8 && sizeof(int64) == 8, "project integer widths");
#else
_Static_assert(sizeof(nuint) == sizeof(void*) && sizeof(nint) == sizeof(void*), "native integer width");
_Static_assert(sizeof(ubyte) == 1 && sizeof(ushort) == 2 && sizeof(uint) == 4 && sizeof(uint64) == 8 && sizeof(int64) == 8, "project integer widths");
#endif

// boolean literals. boole holds 0 or 1; any non-zero is "true" if tested,
// but we always write these two values explicitly.
// guarded with #ifndef because windows.h (via minwindef.h) already
// defines FALSE / TRUE as plain 0 / 1 ints, and our redefinitions
// trigger -Wmacro-redefined warnings in any TU that pulls in both
// headers (platform_win32.c, fs_win32.c). the Win32 defs evaluate to
// the same values our casts would produce (0 and 1 implicitly
// convertible to boole), so skipping the redefine is safe.
#ifndef FALSE
#define FALSE ((boole)0)
#endif
#ifndef TRUE
#define TRUE  ((boole)1)
#endif

// null address literal; pointers are compared against this, never against a bare 0 or NULL
#ifndef NULL_POINTER
#define NULL_POINTER 0
#endif

#endif
